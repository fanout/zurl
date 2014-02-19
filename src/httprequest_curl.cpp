/*
 * Copyright (C) 2012-2013 Fanout, Inc.
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "httprequest.h"

#include <assert.h>
#include <sys/select.h>
#include <QPointer>
#include <QUrl>
#include <curl/curl.h>
#include "jdnsshared.h"
#include "bufferlist.h"
#include "log.h"

#define BUFFER_SIZE 200000

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
	CURLSH *share;
	CURL *easy;
	QString method;
	bool expectBody;
	bool bodyReadFrom;
	struct curl_slist *dnsCache;
	struct curl_slist *headersList;
	int pauseBits;
	BufferList in;
	BufferList out;
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

	CurlConnection() :
		expectBody(false),
		bodyReadFrom(false),
		dnsCache(NULL),
		headersList(NULL),
		pauseBits(0),
		inFinished(false),
		outFinished(false),
		haveStatusLine(false),
		haveResponseHeaders(false),
		newlyReadOrEof(false),
		newlyWritten(0),
		pendingUpdate(false)
	{
		share = curl_share_init();
		easy = curl_easy_init();

		// we use jdns for resolving and cache, so isolate curl's own
		//   caching to this request only
		curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);

		curl_easy_setopt(easy, CURLOPT_SHARE, share);
		curl_easy_setopt(easy, CURLOPT_PRIVATE, this);
		curl_easy_setopt(easy, CURLOPT_DEBUGFUNCTION, debugFunction_cb);
		curl_easy_setopt(easy, CURLOPT_DEBUGDATA, this);
		curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, writeFunction_cb);
		curl_easy_setopt(easy, CURLOPT_WRITEDATA, this);
		curl_easy_setopt(easy, CURLOPT_READFUNCTION, readFunction_cb);
		curl_easy_setopt(easy, CURLOPT_READDATA, this);
		curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, headerFunction_cb);
		curl_easy_setopt(easy, CURLOPT_HEADERDATA, this);

		curl_easy_setopt(easy, CURLOPT_BUFFERSIZE, BUFFER_SIZE);
		curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
		curl_easy_setopt(easy, CURLOPT_HTTP_CONTENT_DECODING, 1);

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
			curl_easy_setopt(easy, CURLOPT_VERBOSE, 1);
	}

	~CurlConnection()
	{
		curl_easy_cleanup(easy);
		curl_slist_free_all(dnsCache);
		curl_slist_free_all(headersList);
		curl_share_cleanup(share);
	}

	void setupMethod(const QString &_method)
	{
		method = _method;

		if(method == "OPTIONS")
		{
			expectBody = false;
			curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "OPTIONS");
		}
		else if(method == "HEAD")
		{
			expectBody = false;
			curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, NULL);
		}
		else if(method == "GET")
		{
			expectBody = false;
			curl_easy_setopt(easy, CURLOPT_HTTPGET, 1);
			curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, NULL);
		}
		else if(method == "POST")
		{
			expectBody = true;
			//curl_easy_setopt(easy, CURLOPT_POST, 1);
			//curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, NULL);
			curl_easy_setopt(easy, CURLOPT_UPLOAD, 1);
			curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "POST");
		}
		else if(method == "PUT")
		{
			expectBody = true;
			curl_easy_setopt(easy, CURLOPT_UPLOAD, 1);
			curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, NULL);
		}
		else if(method == "DELETE")
		{
			expectBody = false;
			curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, "DELETE");
		}
		else
		{
			expectBody = true;
			curl_easy_setopt(easy, CURLOPT_UPLOAD, 1);
			curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method.toLatin1().data());
		}
	}

	void setup(const QUrl &uri, const HttpHeaders &_headers, const QHostAddress &connectAddr = QHostAddress(), int connectPort = -1)
	{
		assert(!method.isEmpty());

		QUrl tmp = uri;
		if(connectPort != -1)
			tmp.setPort(connectPort);
		else if(tmp.port() == -1)
		{
			if(uri.scheme() == "https")
				tmp.setPort(443);
			else
				tmp.setPort(80);
		}

		curl_easy_setopt(easy, CURLOPT_URL, tmp.toEncoded().data());

		if(!connectAddr.isNull())
		{
			curl_slist_free_all(dnsCache);
			QByteArray cacheEntry = tmp.encodedHost() + ':' + QByteArray::number(tmp.port()) + ':' + connectAddr.toString().toUtf8();
			dnsCache = curl_slist_append(dnsCache, cacheEntry.data());
			curl_easy_setopt(easy, CURLOPT_RESOLVE, dnsCache);
		}

		HttpHeaders headers = _headers;

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
		else if(expectBody)
			chunked = true;

		curl_slist_free_all(headersList);
		foreach(const HttpHeader &h, headers)
		{
			QByteArray i = h.first + ": " + h.second;
			headersList = curl_slist_append(headersList, i.data());
		}
		if(chunked && !headers.contains("Transfer-Encoding"))
			headersList = curl_slist_append(headersList, "Transfer-Encoding: chunked");
		// disable expect usage as it appears to be buggy
		curl_slist_append(headersList, "Expect:");
		curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headersList);
	}

	void update()
	{
		if(!pendingUpdate)
		{
			pendingUpdate = true;

			QMetaObject::invokeMethod(this, "doUpdate", Qt::QueuedConnection);
		}
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

	static size_t headerFunction_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
	{
		CurlConnection *self = (CurlConnection *)userdata;
		return self->headerFunction((char *)ptr, size * nmemb);
	}

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
		QByteArray buf = out.take(size);
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
		if(line.isEmpty())
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
			}
			else
			{
				// if a content-encoding was used, don't provide content-length
				QByteArray contentEncoding = responseHeaders.get("Content-Encoding");
				if(!contentEncoding.isEmpty() && contentEncoding != "identity")
					responseHeaders.removeAll("Content-Length");

				// tell the app we've got the header block
				update();
			}
		}
		else if(!haveResponseHeaders)
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

		return size;
	}

	void done(CURLcode _result)
	{
		inFinished = true;
		result = _result;

		newlyReadOrEof = true;
		update();
	}

signals:
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

	CurlConnectionManager(QObject *parent = 0) :
		QObject(parent),
		timer(0),
		pendingUpdate(false)
	{
		timer = new QTimer(this);
		connect(timer, SIGNAL(timeout()), SLOT(timer_timeout()));
		timer->setSingleShot(true);

		curl_global_init(CURL_GLOBAL_ALL);
		multi = curl_multi_init();
		curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, socketFunction_cb);
		curl_multi_setopt(multi, CURLMOPT_SOCKETDATA, this);
		curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, timerFunction_cb);
		curl_multi_setopt(multi, CURLMOPT_TIMERDATA, this);
	}

	~CurlConnectionManager()
	{
		curl_multi_cleanup(multi);
		curl_global_cleanup();
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
			connect(si->snRead, SIGNAL(activated(int)), SLOT(snRead_activated(int)));
			snMap.insert(si->snRead, si);

			si->snWrite = new QSocketNotifier(s, QSocketNotifier::Write, this);
			si->snWrite->setEnabled(false);
			connect(si->snWrite, SIGNAL(activated(int)), SLOT(snWrite_activated(int)));
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
			log_debug("timerFunction: wake up in %dms", (int)timeout_ms);
		else
			log_debug("timerFunction: cancel timer");

		if(timeout_ms == -1)
		{
			if(timer)
				timer->stop();
		}
		else if(timeout_ms == 0)
		{
			timer_timeout();
		}
		else
		{
			timer->start((int)timeout_ms);
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

static CurlConnectionManager *g_man = 0;

class HttpRequest::Private : public QObject
{
	Q_OBJECT

public:
	HttpRequest *q;
	JDnsShared *dns;
	QString connectHost;
	bool ignoreTlsErrors;
	HttpRequest::ErrorCondition errorCondition;
	QString method;
	QUrl uri;
	HttpHeaders headers;
	QString host;
	QList<QHostAddress> addrs;
	HttpRequest::ErrorCondition mostSignificantError;
	bool ignoreBody;
	CurlConnection *conn;
	bool handleAdded;

	Private(HttpRequest *_q, JDnsShared *_dns) :
		QObject(_q),
		q(_q),
		dns(_dns),
		ignoreTlsErrors(false),
		errorCondition(HttpRequest::ErrorNone),
		mostSignificantError(HttpRequest::ErrorGeneric),
		ignoreBody(false),
		conn(0),
		handleAdded(false)
	{
		if(!g_man)
			g_man = new CurlConnectionManager(QCoreApplication::instance());
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		if(conn)
		{
			if(handleAdded)
				curl_multi_remove_handle(g_man->multi, conn->easy);

			delete conn;
			conn = 0;
			handleAdded = false;
		}
	}

	// remake the connection object for retrying
	void remakeConn()
	{
		if(conn)
		{
			// take the request body with us
			BufferList out = conn->out;
			bool outFinished = conn->outFinished;

			cleanup();

			conn = new CurlConnection;
			connect(conn, SIGNAL(updated()), SLOT(conn_updated()));

			conn->setupMethod(method);
			conn->out = out;
			conn->outFinished = outFinished;
		}
	}

	void start(const QString &_method, const QUrl &_uri, const HttpHeaders &_headers)
	{
		if(_method.isEmpty() || (_uri.scheme() != "https" && _uri.scheme() != "http"))
		{
			ignoreBody = true;
			errorCondition = HttpRequest::ErrorGeneric;
			QMetaObject::invokeMethod(q, "error", Qt::QueuedConnection);
			return;
		}

		conn = new CurlConnection;
		connect(conn, SIGNAL(updated()), SLOT(conn_updated()));

		method = _method;
		uri = _uri;
		headers = _headers;

		// eat any transport headers as they'd likely break things
		headers.removeAll("Connection");
		headers.removeAll("Keep-Alive");
		headers.removeAll("Accept-Encoding");
		headers.removeAll("Content-Encoding");
		headers.removeAll("Transfer-Encoding");
		headers.removeAll("Expect");

		conn->setupMethod(method);

		// if we aren't expecting a body, then we'll eat any input and start
		//   the connect after endBody() is called.
		if(conn->expectBody)
			startConnect();
	}

	void writeBody(const QByteArray &body)
	{
		if(body.isEmpty() || ignoreBody)
			return;

		assert(conn);

		if(conn->expectBody)
		{
			conn->out += body;

			if(conn->pauseBits & CURLPAUSE_SEND)
			{
				log_debug("send unpausing");
				conn->pauseBits &= ~CURLPAUSE_SEND;
				curl_easy_pause(conn->easy, conn->pauseBits);
				g_man->update();
			}
		}
		else
		{
			ignoreBody = true;
			errorCondition = HttpRequest::ErrorBodyNotAllowed;
			QMetaObject::invokeMethod(q, "error", Qt::QueuedConnection);
		}
	}

	void endBody()
	{
		if(ignoreBody)
			return;

		assert(conn);

		if(conn->expectBody)
		{
			if(conn->pauseBits & CURLPAUSE_SEND)
			{
				log_debug("send unpausing");
				conn->outFinished = true;
				conn->pauseBits &= ~CURLPAUSE_SEND;
				curl_easy_pause(conn->easy, conn->pauseBits);
				g_man->update();
			}
		}
		else
		{
			startConnect();
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
				g_man->update();
			}

			return out;
		}
		else
			return QByteArray();
	}

	void startConnect()
	{
		if(!connectHost.isEmpty())
			host = connectHost;
		else
			host = uri.host();

		QHostAddress addr(host);
		if(!addr.isNull())
		{
			addrs += addr;
			QMetaObject::invokeMethod(this, "tryNextAddress", Qt::QueuedConnection);
		}
		else
		{
			JDnsSharedRequest *dreq = new JDnsSharedRequest(dns);
			connect(dreq, SIGNAL(resultsReady()), SLOT(dreq_resultsReady()));
			dreq->query(QUrl::toAce(host), QJDns::A);
		}
	}

	// the idea with the priorities here is that an error is considered
	//   more significant the closer the request was to succeeding. e.g.
	//   ErrorTls means the server was actually reached. ErrorPolicy means
	//   we didn't even attempt to try connecting.
	static int errorPriority(HttpRequest::ErrorCondition e)
	{
		if(e == HttpRequest::ErrorTls)
			return 100;
		else if(e == HttpRequest::ErrorConnect)
			return 99;
		else if(e == HttpRequest::ErrorTimeout)
			return 98;
		else if(e == HttpRequest::ErrorPolicy)
			return 97;
		else
			return 0;
	}

private slots:
	// this method emits signals, so don't call directly from start()
	void tryNextAddress()
	{
		QPointer<QObject> self = this;

		if(addrs.isEmpty())
		{
			errorCondition = mostSignificantError;
			emit q->error();
			return;
		}

		QHostAddress addr = addrs.takeFirst();

		log_debug("trying %s", qPrintable(addr.toString()));

		emit q->nextAddress(addr);
		if(!self)
			return;

		conn->setup(uri, headers, addr);

		if(ignoreTlsErrors)
		{
			curl_easy_setopt(conn->easy, CURLOPT_SSL_VERIFYPEER, 0);
			curl_easy_setopt(conn->easy, CURLOPT_SSL_VERIFYHOST, 0);
		}

		handleAdded = true;
		curl_multi_add_handle(g_man->multi, conn->easy);

		// kick the engine
		g_man->doSocketAction(false, CURL_SOCKET_TIMEOUT, 0);
	}

	void dreq_resultsReady()
	{
		JDnsSharedRequest *dreq = (JDnsSharedRequest *)sender();

		if(dreq->success())
		{
			QList<QJDns::Record> results = dreq->results();
			foreach(const QJDns::Record &r, results)
			{
				if(r.type == QJDns::A)
					addrs += r.address;
			}

			delete dreq;
			tryNextAddress();
		}
		else
		{
			delete dreq;
			errorCondition = HttpRequest::ErrorConnect;
			emit q->error();
		}
	}

	void conn_updated()
	{
		if(conn->inFinished && conn->result != CURLE_OK)
		{
			log_debug("curl result: %d", conn->result);

			bool tryAgain = true;

			HttpRequest::ErrorCondition curError;
			switch(conn->result)
			{
				case CURLE_COULDNT_CONNECT:
					curError = HttpRequest::ErrorConnect;
					break;
				case CURLE_SSL_CACERT:
					curError = HttpRequest::ErrorTls;
					break;
				case CURLE_OPERATION_TIMEDOUT:
					// NOTE: if we get this then there may be a chance the request
					//   was actually sent off
					curError = HttpRequest::ErrorTimeout;
					break;
				default:
					tryAgain = false;
					curError = HttpRequest::ErrorGeneric;
			}

			if(errorPriority(curError) > errorPriority(mostSignificantError))
				mostSignificantError = curError;

			// don't try again if we know we sent off a request
			if(conn->bodyReadFrom || conn->haveResponseHeaders)
				tryAgain = false;

			if(tryAgain)
			{
				remakeConn();
				tryNextAddress();
			}
			else
			{
				errorCondition = mostSignificantError;
				emit q->error();
				return;
			}
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

HttpRequest::HttpRequest(JDnsShared *dns, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, dns);
}

HttpRequest::~HttpRequest()
{
	delete d;
}

void HttpRequest::setConnectHost(const QString &host)
{
	d->connectHost = host;
}

void HttpRequest::setIgnoreTlsErrors(bool on)
{
	d->ignoreTlsErrors = on;
}

void HttpRequest::start(const QString &method, const QUrl &uri, const HttpHeaders &headers)
{
	d->start(method, uri, headers);
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

#include "httprequest_curl.moc"
