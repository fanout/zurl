/*
 * Copyright (C) 2012-2022 Fanout, Inc.
 *
 * This file is part of Zurl.
 *
 * $FANOUT_BEGIN_LICENSE:GPL$
 *
 * Zurl is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Zurl is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Alternatively, Zurl may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
 */

#include "httprequest.h"

#include <assert.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#ifdef HAVE_OPENSSL
#include <openssl/ssl.h>
#endif
#include <QSet>
#include <QCoreApplication>
#include <QSocketNotifier>
#include <QTimer>
#include <QPointer>
#include <QHostAddress>
#include <QUrl>
#include <curl/curl.h>
#include "bufferlist.h"
#include "log.h"
#include "verifyhost.h"

#define BUFFER_SIZE 200000
#define REQUEST_BODY_BUFFER_MAX 1000000

// workaround for earlier curl versions
#define UNPAUSE_WORKAROUND 1

static const char *socketActionToString(int x)
{
	switch(x)
	{
		case CURL_POLL_NONE:   return "CURL_POLL_NONE";
		case CURL_POLL_IN:     return "CURL_POLL_IN";
		case CURL_POLL_OUT:    return "CURL_POLL_OUT";
		case CURL_POLL_INOUT:  return "CURL_POLL_INOUT";
		case CURL_POLL_REMOVE: return "CURL_POLL_REMOVE";
		default: return 0;
	}
}

static const char *msgToString(int x)
{
	switch(x)
	{
		case CURLMSG_DONE: return "CURLMSG_DONE";
		default: return 0;
	}
}

class CurlConnection : public QObject
{
	Q_OBJECT

public:
	CURL *easy;
	QString method;
	int maxRedirects;
	bool expectBody;
	bool alwaysSetBody;
	bool bodyReadFrom;
	struct curl_slist *connectTo;
	struct curl_slist *headersList;
	bool addressBlocked;
	int pauseBits;
	BufferList in;
	BufferList out;
	int outPos;
	bool inFinished;
	bool outFinished;
	bool haveStatusLine;
	int responseCode;
	QByteArray responseReason;
	bool haveResponseHeaders;
	HttpHeaders responseHeaders;
	bool newlyReadOrEof;
	int newlyWritten;
	bool pendingUpdate;
	CURLcode result;
	QStringList checkHosts;

	CurlConnection() :
		maxRedirects(-1),
		expectBody(false),
		alwaysSetBody(false),
		bodyReadFrom(false),
		connectTo(NULL),
		headersList(NULL),
		addressBlocked(false),
		pauseBits(0),
		outPos(0),
		inFinished(false),
		outFinished(false),
		haveStatusLine(false),
		haveResponseHeaders(false),
		newlyReadOrEof(false),
		newlyWritten(0),
		pendingUpdate(false)
	{
		easy = curl_easy_init();

		curl_easy_setopt(easy, CURLOPT_PRIVATE, this);
		curl_easy_setopt(easy, CURLOPT_DEBUGFUNCTION, debugFunction_cb);
		curl_easy_setopt(easy, CURLOPT_DEBUGDATA, this);
		curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, writeFunction_cb);
		curl_easy_setopt(easy, CURLOPT_WRITEDATA, this);
		curl_easy_setopt(easy, CURLOPT_READFUNCTION, readFunction_cb);
		curl_easy_setopt(easy, CURLOPT_READDATA, this);
		curl_easy_setopt(easy, CURLOPT_SEEKFUNCTION, seekFunction_cb);
		curl_easy_setopt(easy, CURLOPT_SEEKDATA, this);
		curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, headerFunction_cb);
		curl_easy_setopt(easy, CURLOPT_HEADERDATA, this);
		curl_easy_setopt(easy, CURLOPT_OPENSOCKETFUNCTION, openSocketFunction_cb);
		curl_easy_setopt(easy, CURLOPT_OPENSOCKETDATA, this);

#ifdef HAVE_OPENSSL
		curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
		curl_easy_setopt(easy, CURLOPT_SSL_CTX_FUNCTION, &sslCtxFunction_cb);
		curl_easy_setopt(easy, CURLOPT_SSL_CTX_DATA, this);
#endif

		curl_easy_setopt(easy, CURLOPT_BUFFERSIZE, (long)BUFFER_SIZE);
		curl_easy_setopt(easy, CURLOPT_ENCODING, "");
		curl_easy_setopt(easy, CURLOPT_HTTP_CONTENT_DECODING, 1L);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L);

#if LIBCURL_VERSION_NUM >= 0x072a00
		curl_easy_setopt(easy, CURLOPT_PATH_AS_IS, 1L);
#endif
	}

	~CurlConnection()
	{
		curl_easy_cleanup(easy);
		curl_slist_free_all(connectTo);
		curl_slist_free_all(headersList);
	}

	void setupMethod(const QString &_method, bool _expectBody)
	{
		method = _method;
		expectBody = _expectBody;
		alwaysSetBody = false;

		if(method == "OPTIONS")
		{
			curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "OPTIONS");
		}
		else if(method == "HEAD")
		{
			assert(!expectBody);
			curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
			curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, NULL);
		}
		else if(method == "GET")
		{
			if(!expectBody)
			{
				curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
				curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, NULL);
			}
			else
			{
				curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "GET");
			}
		}
		else if(method == "POST")
		{
			alwaysSetBody = true;
			//curl_easy_setopt(easy, CURLOPT_POST, 1L);
			//curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, NULL);
			curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "POST");
		}
		else if(method == "PUT")
		{
			alwaysSetBody = true;
			// PUT is implied by the UPLOAD option, which we set
			//   below
			curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, NULL);
		}
		else if(method == "DELETE")
		{
			curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "DELETE");
		}
		else
		{
			alwaysSetBody = true;
			curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method.toLatin1().data());
		}

		if(expectBody || alwaysSetBody)
			curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L);
	}

	void setup(const QUrl &uri, const HttpHeaders &_headers, const QString &connectHost = QString(), int connectPort = -1, int _maxRedirects = -1, bool trustConnectHost = false, bool allowIPv6 = false)
	{
		assert(!method.isEmpty());

		HttpHeaders headers = _headers;

		checkHosts += uri.host(QUrl::FullyEncoded);

		if(!connectHost.isEmpty())
		{
			curl_slist_free_all(connectTo);
			QByteArray entry = QByteArray("::") + connectHost.toUtf8() + ':' + QByteArray::number(connectPort);
			connectTo = curl_slist_append(NULL, entry.data());

			curl_easy_setopt(easy, CURLOPT_CONNECT_TO, connectTo);

			if(trustConnectHost)
				checkHosts += connectHost;
		}

		curl_easy_setopt(easy, CURLOPT_URL, uri.toEncoded().data());

		bool chunked = false;
		if(headers.contains("Content-Length"))
		{
			curl_off_t content_len = (curl_off_t)headers.get("Content-Length").toLongLong();
			/*if(method == "POST")
				curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, content_len);
			else*/
				curl_easy_setopt(easy, CURLOPT_INFILESIZE_LARGE, content_len);

			// curl will set this for us
			headers.removeAll("Content-Length");
		}
		else
		{
			if(expectBody)
				chunked = true;
			else if(alwaysSetBody)
				curl_easy_setopt(easy, CURLOPT_INFILESIZE_LARGE, (curl_off_t)0);
		}

		curl_slist_free_all(headersList);
		foreach(const HttpHeader &h, headers)
		{
			QByteArray i = h.first + ": " + h.second;
			headersList = curl_slist_append(headersList, i.data());
		}

		headers.removeAll("Transfer-Encoding");
		if(chunked)
			headersList = curl_slist_append(headersList, "Transfer-Encoding: chunked");

		// disable expect usage as it appears to be buggy
		curl_slist_append(headersList, "Expect:");
		curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headersList);

		maxRedirects = _maxRedirects;
		if(maxRedirects >= 0)
		{
			curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
			curl_easy_setopt(easy, CURLOPT_MAXREDIRS, (long)maxRedirects);
		}

		curl_easy_setopt(easy, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);

		if(!allowIPv6)
		{
			curl_easy_setopt(easy, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
		}
	}

	void update()
	{
		if(!pendingUpdate)
		{
			pendingUpdate = true;

			QMetaObject::invokeMethod(this, "doUpdate", Qt::QueuedConnection);
		}
	}

	void blockAddress()
	{
		addressBlocked = true;
	}

	static size_t debugFunction_cb(CURL *easy, curl_infotype type, char *ptr, size_t size, void *userdata)
	{
		CurlConnection *self = (CurlConnection *)userdata;
		return self->debugFunction(easy, type, ptr, size);
	}

	static size_t writeFunction_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
	{
		CurlConnection *self = (CurlConnection *)userdata;
		return self->writeFunction(ptr, size * nmemb);
	}

	static size_t readFunction_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
	{
		CurlConnection *self = (CurlConnection *)userdata;
		return self->readFunction((char *)ptr, size * nmemb);
	}

	static int seekFunction_cb(void *userdata, curl_off_t offset, int origin)
	{
		CurlConnection *self = (CurlConnection *)userdata;
		return self->seekFunction(offset, origin);
	}

	static size_t headerFunction_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
	{
		CurlConnection *self = (CurlConnection *)userdata;
		return self->headerFunction((char *)ptr, size * nmemb);
	}

	static curl_socket_t openSocketFunction_cb(void *userdata, curlsocktype purpose, struct curl_sockaddr *address)
	{
		CurlConnection *self = (CurlConnection *)userdata;
		return self->openSocketFunction(purpose, address);
	}

#ifdef HAVE_OPENSSL
	static CURLcode sslCtxFunction_cb(CURL *easy, SSL_CTX *context, void *userdata)
	{
		CurlConnection *self = (CurlConnection *)userdata;
		return self->sslCtxFunction(easy, context);
	}

	static int sslVerifyCallback_cb(X509_STORE_CTX *x509StoreContext, void *data)
	{
		CurlConnection *self = (CurlConnection *)data;
		return self->sslVerifyCallback(x509StoreContext);
	}
#endif

	size_t debugFunction(CURL *easy, curl_infotype type, char *ptr, size_t size)
	{
		Q_UNUSED(easy);

		if(type == CURLINFO_TEXT)
		{
			QByteArray str(ptr, size);
			if(str[str.length() - 1] == '\n')
				str.truncate(str.length() - 1);
			log_debug("curl: %s", str.data());
		}

		return 0;
	}

	size_t writeFunction(char *p, size_t size)
	{
		if(size == 0)
			return 0;

		if(in.size() + size > BUFFER_SIZE)
		{
			// pause if we can't fit the data
			log_debug("writeFunction: pausing");
			pauseBits |= CURLPAUSE_RECV;
			return CURL_WRITEFUNC_PAUSE;
		}
		else
		{
			log_debug("writeFunction: accepting %d bytes", size);
			in += QByteArray(p, size);
			newlyReadOrEof = true;
			update();
		}

		return size;
	}

	size_t readFunction(char *p, size_t size)
	{
		QByteArray buf;

		if(outPos >= 0 && out.size() > REQUEST_BODY_BUFFER_MAX)
		{
			// exceeded buffer max, switch to unbuffered
			QByteArray remaining = out.mid(outPos);
			out.clear();
			out += remaining;
			outPos = -1;
		}

		if(outPos >= 0)
		{
			buf = out.mid(outPos, size);
			outPos += buf.size();
		}
		else
		{
			buf = out.take(size);
		}

		if(!buf.isEmpty())
		{
			bodyReadFrom = true;
			memcpy(p, buf.data(), buf.size());
			newlyWritten += buf.size();
			log_debug("readFunction: providing %d bytes", buf.size());
			update();
			return buf.size();
		}
		else
		{
			if(outFinished)
			{
				log_debug("readFunction: eof");
				return 0;
			}
			else
			{
				log_debug("readFunction: pausing");
				pauseBits |= CURLPAUSE_SEND;
				return CURL_READFUNC_PAUSE;
			}
		}
	}

	int seekFunction(curl_off_t offset, int origin)
	{
		if(outPos < 0)
		{
			log_debug("seekFunction: can't seek. input is unbuffered");
			return 1;
		}

		if(origin == SEEK_SET)
		{
			if(offset <= out.size())
			{
				outPos = offset;
				log_debug("seekFunction: seeking to position %ld", offset);
				return 0;
			}
			else
			{
				log_debug("seekFunction: %ld out of range (range: 0-%d)", offset, out.size());
				return 1;
			}
		}
		else
		{
			log_debug("seekFunction: unknown origin value: %d", origin);
			return 1;
		}
	}

	size_t headerFunction(char *p, size_t size)
	{
		assert(p[size - 1] == '\n');

		// curl doesn't protect us from \n vs \r\n
		int len;
		if(p[size - 2] == '\r')
			len = size - 2;
		else
			len = size - 1;

		QByteArray line(p, len);
		if(!line.isEmpty())
		{
			if(haveResponseHeaders)
			{
				// does it look like we're getting a status
				//   line again? (happens when redirecting)
				int at = line.indexOf(' ');
				if(at != -1 && !line.mid(0, at).contains(':'))
				{
					haveStatusLine = false;
					haveResponseHeaders = false;
					responseHeaders.clear();
				}
			}

			if(!haveResponseHeaders)
			{
				if(haveStatusLine)
				{
					int at = line.indexOf(": ");
					if(at == -1)
						return -1;

					log_debug("response header: %s", line.data());
					responseHeaders += HttpHeader(line.mid(0, at), line.mid(at + 2));
				}
				else
				{
					// status reason we have to parse ourselves
					int at = line.indexOf(' ');
					if(at == -1)
						return -1;
					at = line.indexOf(' ', at + 1);
					if(at == -1)
						return -1;
					responseReason = line.mid(at + 1);

					haveStatusLine = true;
				}
			}
		}
		else
		{
			haveResponseHeaders = true;

			// grab the status code
			long l;
			curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &l);
			responseCode = (int)l;

			if(responseCode == 100)
			{
				log_debug("got code 100, ignoring this header block");
				haveStatusLine = false;
				haveResponseHeaders = false;
				responseHeaders.clear();
				return size;
			}

			if(maxRedirects >= 0 && responseCode >= 300 && responseCode < 400 && responseHeaders.contains("Location"))
			{
				log_debug("got code 3xx and redirects enabled, ignoring this header block");
				haveStatusLine = false;
				haveResponseHeaders = false;
				responseHeaders.clear();
				return size;
			}

			// if a content-encoding was used, don't provide content-length
			QByteArray contentEncoding = responseHeaders.get("Content-Encoding");
			if(!contentEncoding.isEmpty() && contentEncoding != "identity")
				responseHeaders.removeAll("Content-Length");

			// tell the app we've got the header block
			newlyReadOrEof = true;
			update();
		}

		return size;
	}

	curl_socket_t openSocketFunction(curlsocktype purpose, struct curl_sockaddr *address)
	{
		Q_UNUSED(purpose);

		if(address->family == AF_INET6 || address->family == AF_INET)
		{
			QHostAddress addr;
			addr.setAddress(&address->addr);
			if(addr.isNull())
			{
				return CURL_SOCKET_BAD;
			}

			addressBlocked = false;

			emit nextAddress(addr);

			if(addressBlocked)
			{
				return CURL_SOCKET_BAD;
			}
		}

		return socket(address->family, address->socktype, address->protocol);
	}

#ifdef HAVE_OPENSSL
	CURLcode sslCtxFunction(CURL *_easy, SSL_CTX *context)
	{
		Q_UNUSED(_easy);

		SSL_CTX_set_cert_verify_callback(context, sslVerifyCallback_cb, this);
		return CURLE_OK;
	}

	int sslVerifyCallback(X509_STORE_CTX *x509StoreContext)
	{
#if OPENSSL_VERSION_NUMBER < 0x10100000L
		X509 *peerCert = x509StoreContext->cert;
#else
		X509 *peerCert = X509_STORE_CTX_get0_cert(x509StoreContext);
#endif

		foreach(const QString &host, checkHosts)
		{
			if(verifyhost(host.toUtf8().data(), peerCert) == CURLE_OK)
				return 1;
		}

		// TODO: later on, consider changing this to X509_V_ERR_HOSTNAME_MISMATCH
		X509_STORE_CTX_set_error(x509StoreContext, X509_V_ERR_SUBJECT_ISSUER_MISMATCH);
		return 0;
	}
#endif

	void done(CURLcode _result)
	{
		inFinished = true;
		result = _result;

		newlyReadOrEof = true;
		update();
	}

signals:
	// NOTE: not DOR-SS
	void nextAddress(const QHostAddress &addr);

	void updated();

public slots:
	void doUpdate()
	{
		pendingUpdate = false;

		emit updated();
	}
};

class CurlConnectionManager : public QObject
{
	Q_OBJECT

public:
	class SocketInfo
	{
	public:
		QSocketNotifier *snRead;
		QSocketNotifier *snWrite;

		SocketInfo() :
			snRead(0),
			snWrite(0)
		{
		}

		~SocketInfo()
		{
			delete snRead;
			delete snWrite;
		}
	};

	CURLM *multi;
	QHash<QSocketNotifier*, SocketInfo*> snMap;
	QTimer *timer;
	bool pendingUpdate;
	QSet<CurlConnection*> connections;

	CurlConnectionManager(QObject *parent = 0) :
		QObject(parent),
		timer(0),
		pendingUpdate(false)
	{
		timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, &CurlConnectionManager::timer_timeout);
		timer->setSingleShot(true);

		multi = curl_multi_init();
		curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, socketFunction_cb);
		curl_multi_setopt(multi, CURLMOPT_SOCKETDATA, this);
		curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, timerFunction_cb);
		curl_multi_setopt(multi, CURLMOPT_TIMERDATA, this);
	}

	~CurlConnectionManager()
	{
		assert(connections.isEmpty());

		curl_multi_cleanup(multi);
	}

	void update()
	{
		if(!pendingUpdate)
		{
			pendingUpdate = true;
			QMetaObject::invokeMethod(this, "doUpdate", Qt::QueuedConnection);
		}
	}

	static int socketFunction_cb(CURL *easy, curl_socket_t s, int action, void *userp, void *socketp)
	{
		CurlConnectionManager *self = (CurlConnectionManager *)userp;
		return self->socketFunction(easy, s, action, socketp);
	}

	static void timerFunction_cb(CURLM *multi, long timeout_ms, void *userp)
	{
		CurlConnectionManager *self = (CurlConnectionManager *)userp;
		self->timerFunction(multi, timeout_ms);
	}

	int socketFunction(CURL *easy, curl_socket_t s, int action, void *socketp)
	{
		Q_UNUSED(easy);

		const char *str = socketActionToString(action);
		if(!str)
		{
			log_debug("socketFunction: unknown action: %d fd=%d", action, s);
			return 0;
		}

		log_debug("socketFunction: %s %d", str, s);

		if(action == CURL_POLL_REMOVE)
		{
			SocketInfo *si = (SocketInfo *)socketp;
			snMap.remove(si->snRead);
			snMap.remove(si->snWrite);
			delete si;
			return 0;
		}

		SocketInfo *si = (SocketInfo *)socketp;
		if(!si)
		{
			si = new SocketInfo;

			si->snRead = new QSocketNotifier(s, QSocketNotifier::Read, this);
			si->snRead->setEnabled(false);
			connect(si->snRead, &QSocketNotifier::activated, this, &CurlConnectionManager::snRead_activated);
			snMap.insert(si->snRead, si);

			si->snWrite = new QSocketNotifier(s, QSocketNotifier::Write, this);
			si->snWrite->setEnabled(false);
			connect(si->snWrite, &QSocketNotifier::activated, this, &CurlConnectionManager::snWrite_activated);
			snMap.insert(si->snWrite, si);

			curl_multi_assign(multi, s, si);
		}

		if(action == CURL_POLL_IN || action == CURL_POLL_INOUT)
			si->snRead->setEnabled(true);
		else
			si->snRead->setEnabled(false);

		if(action == CURL_POLL_OUT || action == CURL_POLL_INOUT)
			si->snWrite->setEnabled(true);
		else
			si->snWrite->setEnabled(false);

		return 0;
	}

	void timerFunction(CURLM *multi, long timeout_ms)
	{
		Q_UNUSED(multi);

		if(timeout_ms >= 0)
		{
			log_debug("timerFunction: wake up in %dms", (int)timeout_ms);
			timer->start((int)timeout_ms);
		}
		else
		{
			log_debug("timerFunction: cancel timer");
			timer->stop();
		}
	}

	void doSocketAction(bool all, int sockfd, int ev_bitmask)
	{
		int running;
		if(!all)
			curl_multi_socket_action(multi, sockfd, ev_bitmask, &running);
		else
			curl_multi_socket_all(multi, &running);

		processMessages();
	}

	void processMessages()
	{
		while(true)
		{
			int pending;
			CURLMsg *m = curl_multi_info_read(multi, &pending);
			if(!m || !m->msg)
				break;

			const char *str = msgToString(m->msg);
			if(str)
				log_debug("message: %s", str);
			else
				log_debug("unknown message: %d", m->msg);

			if(m->msg == CURLMSG_DONE)
			{
				CurlConnection *conn;
				curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, &conn);
				conn->done(m->data.result);
			}
		}
	}

public slots:
	void snRead_activated(int socket)
	{
		QSocketNotifier *sn = (QSocketNotifier *)sender();
		SocketInfo *si = snMap.value(sn);
		assert(si);

		doSocketAction(false, socket, CURL_CSELECT_IN);
	}

	void snWrite_activated(int socket)
	{
		QSocketNotifier *sn = (QSocketNotifier *)sender();
		SocketInfo *si = snMap.value(sn);
		assert(si);

		doSocketAction(false, socket, CURL_CSELECT_OUT);
	}

	void timer_timeout()
	{
		doSocketAction(false, CURL_SOCKET_TIMEOUT, 0);
	}

	void doUpdate()
	{
		pendingUpdate = false;

#ifdef UNPAUSE_WORKAROUND
		doSocketAction(true, 0, 0);
#else
		doSocketAction(false, CURL_SOCKET_TIMEOUT, 0);
#endif
	}
};

class CurlConnectionManagerManager : public QObject
{
	Q_OBJECT

public:
	class Item
	{
	public:
		CurlConnectionManager *manager;
		int refs;

		Item() :
			manager(0),
			refs(0)
		{
		}

		~Item()
		{
			delete manager;
		}
	};

	Item *current;
	QHash<CurlConnectionManager*, Item*> old;
	QTimer *timer;
	int persistentConnectionMaxTime;

	CurlConnectionManagerManager(QObject *parent = 0) :
		QObject(parent),
		current(0),
		persistentConnectionMaxTime(-1)
	{
		curl_global_init(CURL_GLOBAL_ALL);

		timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, &CurlConnectionManagerManager::rotate);
		timer->setSingleShot(true);
	}

	// NOTE: not DOR-SS
	~CurlConnectionManagerManager()
	{
		qDeleteAll(old);
		delete current;

		curl_global_cleanup();
	}

	CurlConnectionManager *retainCurrent()
	{
		if(!current)
		{
			current = new Item;
			current->manager = new CurlConnectionManager(this);

			if(persistentConnectionMaxTime > 0)
				timer->start(persistentConnectionMaxTime * 1000);
		}

		++(current->refs);

		return current->manager;
	}

	void release(CurlConnectionManager *manager)
	{
		if(current && manager == current->manager)
		{
			assert(current->refs > 0);
			--(current->refs);
		}
		else
		{
			Item *i = old.value(manager);
			assert(i);
			assert(i->refs > 0);
			--(i->refs);
			if(i->refs == 0)
			{
				old.remove(manager);
				delete i;
				log_debug("removed connection manager (old=%d)", old.count());
			}
		}
	}

	void setPersistentConnectionMaxTime(int secs)
	{
		persistentConnectionMaxTime = secs;

		if(persistentConnectionMaxTime > 0 && current)
			timer->start(persistentConnectionMaxTime * 1000);
	}

private slots:
	void rotate()
	{
		if(current->refs > 0)
			old.insert(current->manager, current);
		else
			delete current;

		current = 0;

		log_debug("rotated connection managers (old=%d)", old.count());
	}
};

static CurlConnectionManagerManager *_g_ccmm = 0;

static CurlConnectionManagerManager *g_ccmm()
{
	if(!_g_ccmm)
		_g_ccmm = new CurlConnectionManagerManager(QCoreApplication::instance());
	return _g_ccmm;
}

class HttpRequest::Private : public QObject
{
	Q_OBJECT

public:
	HttpRequest *q;
	QString connectHost;
	int connectPort;
	bool trustConnectHost;
	bool allowIPv6;
	bool ignoreTlsErrors;
	int maxRedirects;
	int addressesAttempted;
	int addressesBlocked;
	HttpRequest::ErrorCondition errorCondition;
	QString method;
	QUrl uri;
	HttpHeaders headers;
	bool willWriteBody;
	bool bodyNotAllowed;
	bool ignoreBody;
	CurlConnection *conn;
	CurlConnectionManager *manager;

	Private(HttpRequest *_q) :
		QObject(_q),
		q(_q),
		connectPort(-1),
		trustConnectHost(false),
		allowIPv6(false),
		ignoreTlsErrors(false),
		maxRedirects(-1),
		addressesAttempted(0),
		addressesBlocked(0),
		errorCondition(HttpRequest::ErrorNone),
		willWriteBody(false),
		bodyNotAllowed(false),
		ignoreBody(false),
		conn(0),
		manager(0)
	{
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		if(conn)
		{
			if(manager)
			{
				curl_multi_remove_handle(manager->multi, conn->easy);
				manager->connections -= conn;
				g_ccmm()->release(manager);
				manager = 0;
			}

			delete conn;
			conn = 0;
		}
	}

	void start(const QString &_method, const QUrl &_uri, const HttpHeaders &_headers, bool _willWriteBody)
	{
		addressesAttempted = 0;
		addressesBlocked = 0;

		if(_method.isEmpty() || (_uri.scheme() != "https" && _uri.scheme() != "http"))
		{
			ignoreBody = true;
			errorCondition = HttpRequest::ErrorGeneric;
			QMetaObject::invokeMethod(q, "error", Qt::QueuedConnection);
			return;
		}

		method = _method;
		uri = _uri;
		headers = _headers;
		willWriteBody = _willWriteBody;

		// we'd prefer not to send chunked encoding header if we don't
		//   have to for certain methods, so wait and see if the user
		//   actually tries to send a body before we start the request
		if(willWriteBody && (method == "OPTIONS" || method == "GET" || method == "DELETE"))
			return;

		// if the user might provide a body but we don't expect one
		//   for the method type, then we'll wait and see what is
		//   provided before starting the request.
		if(willWriteBody && method == "HEAD")
		{
			bodyNotAllowed = true;
			return;
		}

		startConnect();
	}

	void writeBody(const QByteArray &body)
	{
		assert(willWriteBody);

		if(body.isEmpty() || ignoreBody)
			return;

		if(bodyNotAllowed)
		{
			ignoreBody = true;
			errorCondition = HttpRequest::ErrorBodyNotAllowed;
			QMetaObject::invokeMethod(q, "error", Qt::QueuedConnection);
			return;
		}

		if(!conn)
			startConnect();

		assert(conn);

		conn->out += body;

		if(conn->pauseBits & CURLPAUSE_SEND)
		{
			log_debug("send unpausing");
			conn->pauseBits &= ~CURLPAUSE_SEND;
			curl_easy_pause(conn->easy, conn->pauseBits);
			manager->update();
		}
	}

	void endBody()
	{
		assert(willWriteBody);

		if(ignoreBody)
			return;

		if(!conn)
		{
			// if the user called endBody without actually writing
			//   a body then we can set this flag off
			willWriteBody = false;

			startConnect();
		}

		assert(conn);

		conn->outFinished = true;

		if(conn->pauseBits & CURLPAUSE_SEND)
		{
			log_debug("send unpausing");
			conn->pauseBits &= ~CURLPAUSE_SEND;
			curl_easy_pause(conn->easy, conn->pauseBits);
			manager->update();
		}
	}

	QByteArray readResponseBody(int size)
	{
		if(conn)
		{
			QByteArray out = conn->in.take(size);
			if(out.isEmpty())
				return out;

			if(conn->pauseBits & CURLPAUSE_RECV)
			{
				log_debug("recv unpausing");
				conn->pauseBits &= ~CURLPAUSE_RECV;
				curl_easy_pause(conn->easy, conn->pauseBits);
				manager->update();
			}

			return out;
		}
		else
			return QByteArray();
	}

	void startConnect()
	{
		assert(!conn);

		conn = new CurlConnection;
		connect(conn, &CurlConnection::nextAddress, this, &Private::conn_nextAddress);
		connect(conn, &CurlConnection::updated, this, &Private::conn_updated);

		// eat any transport headers as they'd likely break things
		headers.removeAll("Connection");
		headers.removeAll("Keep-Alive");
		headers.removeAll("Accept-Encoding");
		headers.removeAll("Content-Encoding");
		headers.removeAll("Transfer-Encoding");
		headers.removeAll("Expect");

		conn->setupMethod(method, willWriteBody);

		conn->setup(uri, headers, connectHost, connectPort, maxRedirects, trustConnectHost, allowIPv6);

		if(ignoreTlsErrors)
		{
			curl_easy_setopt(conn->easy, CURLOPT_SSL_VERIFYPEER, 0L);

#ifndef HAVE_OPENSSL
			// if openssl used then this will already be disabled,
			curl_easy_setopt(conn->easy, CURLOPT_SSL_VERIFYHOST, 0L);
#endif
		}

		manager = g_ccmm()->retainCurrent();
		manager->connections += conn;
		curl_multi_add_handle(manager->multi, conn->easy);

		// kick the engine
		manager->doSocketAction(false, CURL_SOCKET_TIMEOUT, 0);
	}

private slots:
	void conn_nextAddress(const QHostAddress &addr)
	{
		++addressesAttempted;

		log_debug("trying %s", qPrintable(addr.toString()));

		if(!allowIPv6 && addr.protocol() != QAbstractSocket::IPv4Protocol)
		{
			conn->blockAddress();
			return;
		}

		emit q->nextAddress(addr);
	}

	void conn_updated()
	{
		if(conn->inFinished && conn->result != CURLE_OK)
		{
			log_debug("curl result: %d", conn->result);

			HttpRequest::ErrorCondition curError;
			switch(conn->result)
			{
				case CURLE_COULDNT_RESOLVE_HOST:
				case CURLE_COULDNT_CONNECT:
					if(addressesAttempted > 0 && addressesBlocked >= addressesAttempted)
					{
						curError = HttpRequest::ErrorPolicy;
					}
					else
					{
						curError = HttpRequest::ErrorConnect;
					}
					break;
#if LIBCURL_VERSION_NUM < 0x073e00
				case CURLE_SSL_CACERT:
#endif
				case CURLE_PEER_FAILED_VERIFICATION:
					curError = HttpRequest::ErrorTls;
					break;
				case CURLE_OPERATION_TIMEDOUT:
					curError = HttpRequest::ErrorTimeout;
					break;
				case CURLE_TOO_MANY_REDIRECTS:
					curError = HttpRequest::ErrorTooManyRedirects;
					break;
				default:
					curError = HttpRequest::ErrorGeneric;
			}

			errorCondition = curError;
			emit q->error();
		}
		else
		{
			QPointer<QObject> self = this;

			if(conn->newlyReadOrEof)
			{
				conn->newlyReadOrEof = false;
				emit q->readyRead();
				if(!self)
					return;
			}

			if(conn->newlyWritten > 0)
			{
				int x = conn->newlyWritten;
				conn->newlyWritten = 0;
				emit q->bytesWritten(x);
			}
		}
	}
};

HttpRequest::HttpRequest(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

HttpRequest::~HttpRequest()
{
	delete d;
}

void HttpRequest::setConnectHostPort(const QString &host, int port)
{
	d->connectHost = host;
	d->connectPort = port;
}

void HttpRequest::setTrustConnectHost(bool on)
{
	d->trustConnectHost = on;
}

void HttpRequest::setIgnoreTlsErrors(bool on)
{
	d->ignoreTlsErrors = on;
}

void HttpRequest::setFollowRedirects(int maxRedirects)
{
	d->maxRedirects = maxRedirects;
}

void HttpRequest::setAllowIPv6(bool on)
{
	d->allowIPv6 = on;
}

void HttpRequest::start(const QString &method, const QUrl &uri, const HttpHeaders &headers, bool willWriteBody)
{
	d->start(method, uri, headers, willWriteBody);
}

void HttpRequest::writeBody(const QByteArray &body)
{
	d->writeBody(body);
}

void HttpRequest::endBody()
{
	d->endBody();
}

int HttpRequest::bytesAvailable() const
{
	if(d->conn)
		return d->conn->in.size();
	else
		return 0;
}

bool HttpRequest::isFinished() const
{
	if(d->errorCondition != ErrorNone || (d->conn && d->conn->inFinished))
		return true;
	else
		return false;
}

HttpRequest::ErrorCondition HttpRequest::errorCondition() const
{
	return d->errorCondition;
}

int HttpRequest::responseCode() const
{
	if(d->conn)
		return d->conn->responseCode;
	else
		return -1;
}

QByteArray HttpRequest::responseReason() const
{
	if(d->conn)
		return d->conn->responseReason;
	else
		return QByteArray();
}

HttpHeaders HttpRequest::responseHeaders() const
{
	if(d->conn)
		return d->conn->responseHeaders;
	else
		return HttpHeaders();
}

QByteArray HttpRequest::readResponseBody(int size)
{
	return d->readResponseBody(size);
}

void HttpRequest::blockAddress()
{
	if(d->conn)
	{
		++(d->addressesBlocked);
		d->conn->blockAddress();
	}
}

void HttpRequest::setPersistentConnectionMaxTime(int secs)
{
	g_ccmm()->setPersistentConnectionMaxTime(secs);
}

#include "httprequest.moc"
