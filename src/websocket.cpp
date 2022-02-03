/*
 * Copyright (C) 2014-2022 Fanout, Inc.
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

#include "websocket.h"

#include <assert.h>
#ifdef HAVE_OPENSSL
#include <openssl/x509.h>
#endif
#include <QUrl>
#include <QPointer>
#include <QSslSocket>
#include "log.h"
#include "bufferlist.h"
#include "addressresolver.h"
#include "verifyhost.h"

#define RESPONSE_BODY_MAX 100000
#define MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static quint16 read16(const quint8 *in)
{
	quint16 out = in[0];
	out <<= 8;
	out += in[1];
	return out;
}

static quint64 read64(const quint8 *in)
{
	quint64 out = in[0];
	out <<= 8;
	out += in[1];
	out <<= 8;
	out += in[2];
	out <<= 8;
	out += in[3];
	out <<= 8;
	out += in[4];
	out <<= 8;
	out += in[5];
	out <<= 8;
	out += in[6];
	out <<= 8;
	out += in[7];
	return out;
}

static void write16(quint8 *out, quint16 i)
{
	out[0] = (i >> 8) & 0xff;
	out[1] = i & 0xff;
}

static void write64(quint8 *out, quint64 i)
{
	out[0] = (i >> 56) & 0xff;
	out[1] = (i >> 48) & 0xff;
	out[2] = (i >> 40) & 0xff;
	out[3] = (i >> 32) & 0xff;
	out[4] = (i >> 24) & 0xff;
	out[5] = (i >> 16) & 0xff;
	out[6] = (i >> 8) & 0xff;
	out[7] = i & 0xff;
}

static QByteArray createFrame(bool fin, int opcode, const QByteArray &payload, const QByteArray &mask)
{
	int payloadSize = payload.size();

	int frameSize;
	if(payloadSize < 126)
		frameSize = 2;
	else if(payloadSize < 65536)
		frameSize = 4;
	else
		frameSize = 10;

	if(!mask.isEmpty())
		frameSize += 4;

	frameSize += payloadSize;

	QByteArray out;
	out.resize(frameSize);

	quint8 *p = (quint8 *)out.data();

	quint8 b1 = 0;
	if(fin)
		b1 |= 0x80;
	b1 |= (opcode & 0x0f);

	*(p++) = b1;

	if(payloadSize < 126)
	{
		*(p++) = payloadSize;
	}
	else if(payloadSize < 65536)
	{
		*(p++) = 126;
		write16(p, payloadSize);
		p += 2;
	}
	else
	{
		*(p++) = 127;
		write64(p, payloadSize);
		p += 8;
	}

	if(!mask.isEmpty())
	{
		quint8 b2 = out[1];
		b2 |= 0x80;
		out[1] = b2;
		memcpy(p, mask.data(), 4);
		p += 4;
		for(int n = 0; n < payloadSize; ++n)
			*(p++) = (quint8)payload[n] ^ (quint8)mask[n % 4];
	}
	else
	{
		memcpy(p, payload.data(), payloadSize);
		p += payloadSize;
	}

	return out;
}

// ret: 0 = need more data (size unknown), 1 = need more data (size known), 2 = ready to read
static int checkFrame(const quint8 *data, quint64 size, quint64 *payloadSize)
{
	if(size < 2)
		return 0;

	int headerSize;

	quint8 b2 = data[1] & 0x7f;

	if(b2 < 126)
	{
		headerSize = 2;
		*payloadSize = b2;
	}
	else if(b2 == 126)
	{
		if(size < 2 + 2)
			return 0;

		headerSize = 4;
		*payloadSize = read16(data + 2);
	}
	else
	{
		if(size < 2 + 8)
			return 0;

		headerSize = 10;
		*payloadSize = read64(data + 2);
	}

	if(data[1] & 0x80)
		headerSize += 4;

	if(size < (quint64)headerSize + *payloadSize)
		return 1;

	return 2;
}

// this method assumes checkFrame has passed
static QByteArray parseFrame(const quint8 *data, bool *fin, int *opcode, int *bytesRead)
{
	int headerSize;
	int payloadSize;

	quint8 b1 = data[0];
	quint8 b2 = data[1] & 0x7f;

	if(b2 < 126)
	{
		headerSize = 2;
		payloadSize = b2;
	}
	else if(b2 == 126)
	{
		headerSize = 4;
		payloadSize = read16(data + 2);
	}
	else
	{
		headerSize = 10;
		payloadSize = read64(data + 2);
	}

	QByteArray payload;
	payload.resize(payloadSize);

	if(data[1] & 0x80)
	{
		const quint8 *maskp = data + headerSize;
		headerSize += 4;
		const quint8 *p = data + headerSize;
		for(int n = 0; n < payloadSize; ++n)
			payload[n] = *(p++) ^ maskp[n % 4];
	}
	else
	{
		const quint8 *p = data + headerSize;
		memcpy(payload.data(), p, payloadSize);
	}

	if(b1 & 0x80)
		*fin = true;
	else
		*fin = false;

	*opcode = b1 & 0x0f;
	*bytesRead = headerSize + payloadSize;

	return payload;
}

static int findLinebreak(const quint8 *data, int size)
{
	for(int n = 0; n < size - 1; ++n)
	{
		if(data[n] == '\r' && data[n + 1] == '\n')
			return n;
	}

	return -1;
}

// ret: 0 = need more data (size unknown), 1 = need more data (size known), 2 = ready to read, -1 = error
static int checkChunk(const quint8 *data, quint64 size, quint64 *payloadSize)
{
	int at = findLinebreak(data, (int)size);
	if(at == -1)
		return 0;

	bool ok;
	int x = QByteArray((const char *)data, at).toInt(&ok, 16);
	if(!ok)
		return -1;

	*payloadSize = (quint64)x;

	at += 2 + x;
	if((quint64)at + 2 > size)
		return 1;

	if(data[at] != '\r' || data[at + 1] != '\n')
		return -1;

	return 2;
}

// this method assumes checkChunk has passed
static QByteArray parseChunk(const quint8 *data, quint64 size, int *bytesRead)
{
	int at = findLinebreak(data, (int)size);

	bool ok;
	int x = QByteArray((const char *)data, at).toInt(&ok, 16);

	at += 2;
	*bytesRead = at + x + 2;
	return QByteArray((const char *)data + at, x);
}

class WebSocket::Private : public QObject
{
	Q_OBJECT

public:
	class WriteItem
	{
	public:
		enum Type
		{
			Handshake,
			Frame
		};

		Type type;
		int opcode;
		int size;

		WriteItem(int _size) :
			type(Handshake),
			opcode(-1),
			size(_size)
		{
		}

		WriteItem(int _opcode, int _size) :
			type(Frame),
			opcode(_opcode),
			size(_size)
		{
		}
	};

	WebSocket *q;
	AddressResolver *resolver;
	State state;
	QString connectHost;
	bool trustConnectHost;
	bool ignoreTlsErrors;
	int maxRedirects;
	int maxFrameSize;
	QSslSocket *sock;
	QUrl requestUri;
	HttpHeaders requestHeaders;
	QByteArray requestKey;
	int responseCode;
	QByteArray responseReason;
	HttpHeaders responseHeaders;
	BufferList responseBody;
	int responseContentLength;
	bool readingResponseBody;
	bool chunked;
	bool peerClosing;
	int peerCloseCode;
	QString peerCloseReason;
	QList<QHostAddress> addrs;
	ErrorCondition errorCondition;
	ErrorCondition mostSignificantError;
	QString host;
	QByteArray inbuf;
	bool inStatusLine;
	QList<Frame> in;
	int inBytes;
	bool pendingRead;
	QList<WriteItem> pendingWrites;
	int followedRedirects;

	Private(WebSocket *_q) :
		QObject(_q),
		q(_q),
		state(Idle),
		trustConnectHost(false),
		ignoreTlsErrors(false),
		maxRedirects(-1),
		maxFrameSize(-1),
		sock(0),
		responseCode(-1),
		responseContentLength(-1),
		readingResponseBody(false),
		chunked(false),
		peerClosing(false),
		peerCloseCode(-1),
		errorCondition(ErrorNone),
		mostSignificantError(ErrorGeneric),
		inStatusLine(true),
		inBytes(0),
		pendingRead(false),
		followedRedirects(0)
	{
		resolver = new AddressResolver(this);
		connect(resolver, &AddressResolver::resultsReady, this, &Private::resolver_resultsReady);
		connect(resolver, &AddressResolver::error, this, &Private::resolver_error);
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		if(sock)
		{
			sock->disconnect(this);
			sock->setParent(0);
			sock->deleteLater();
			sock = 0;
		}
	}

	void start(const QUrl &uri, const HttpHeaders &headers)
	{
		requestUri = uri;
		requestHeaders = headers;

		tryConnect();
	}

	void tryConnect()
	{
		responseCode = -1;
		responseContentLength = -1;
		readingResponseBody = false;
		chunked = false;
		peerClosing = false;
		peerCloseCode = -1;
		peerCloseReason.clear();
		errorCondition = ErrorNone;
		mostSignificantError = ErrorGeneric;
		inbuf.clear();
		inStatusLine = true;
		pendingRead = false;

		if(!connectHost.isEmpty())
			host = connectHost;
		else
			host = requestUri.host();

		state = Connecting;
		resolver->start(host);
	}

	void writeFrame(const Frame &frame)
	{
		assert(state != Idle);

		if(state == Closing)
			return;

		int opcode;
		if(frame.type == Frame::Continuation)
			opcode = 0;
		else if(frame.type == Frame::Text)
			opcode = 1;
		else if(frame.type == Frame::Binary)
			opcode = 2;
		else if(frame.type == Frame::Ping)
			opcode = 9;
		else if(frame.type == Frame::Pong)
			opcode = 10;
		else
		{
			// ignore unsupported frame type
			return;
		}

		log_debug("ws: writing frame type=%d, size=%d", opcode, frame.data.size());

		QByteArray buf = createFrame(!frame.more, opcode, frame.data, generateMask());
		pendingWrites += WriteItem(opcode, buf.size());
		sock->write(buf);
	}

	Frame readFrame()
	{
		Frame f = in.takeFirst();
		inBytes -= f.data.size();

		if(!pendingRead && (maxFrameSize == -1 || inBytes < maxFrameSize) && sock && sock->bytesAvailable() > 0)
		{
			pendingRead = true;
			QMetaObject::invokeMethod(this, "tryRead", Qt::QueuedConnection);
		}

		return f;
	}

	void close(int code, const QString &reason)
	{
		log_debug("ws: closing");

		state = Closing;

		QByteArray buf;
		if(code != -1)
		{
			QByteArray rawReason = reason.toUtf8();

			QByteArray data(2 + rawReason.size(), 0);
			write16((quint8 *)data.data(), code);
			memcpy(data.data() + 2, rawReason.data(), rawReason.size());
			buf = createFrame(true, 8, data, generateMask());
		}
		else
			buf = createFrame(true, 8, QByteArray(), generateMask());

		pendingWrites += WriteItem(8, buf.size());
		sock->write(buf);

		if(peerClosing)
			sock->disconnectFromHost();
	}

	static QByteArray generateKey()
	{
		QByteArray out(16, 0);
		for(int n = 0; n < out.size(); ++n)
			out[n] = qrand() % 256;

		return out;
	}

	static QByteArray generateMask()
	{
		QByteArray out(4, 0);
		for(int n = 0; n < out.size(); ++n)
			out[n] = qrand() % 256;

		return out;
	}

	// the idea with the priorities here is that an error is considered
	//   more significant the closer the request was to succeeding. e.g.
	//   ErrorTls means the server was actually reached. ErrorPolicy means
	//   we didn't even attempt to try connecting.
	// note: only pre-connected-state errors are concerned with priority
	static int errorPriority(ErrorCondition e)
	{
		if(e == ErrorTls)
			return 100;
		else if(e == ErrorConnect)
			return 99;
		else if(e == ErrorTimeout)
			return 98;
		else if(e == ErrorPolicy)
			return 97;
		else
			return 0;
	}

	bool parseStatusLine(const QByteArray &line, int *code, QByteArray *reason)
	{
		int end = line.indexOf(' ');
		if(end == -1)
			return false;

		int start = end + 1;
		end = line.indexOf(' ', start);
		if(end == -1)
			return false;

		bool ok;
		*code = line.mid(start, end - start).toInt(&ok);
		if(!ok)
			return false;

		*reason = line.mid(end + 1);
		return true;
	}

	bool handleResponseLine(const QByteArray &line)
	{
		if(inStatusLine)
		{
			if(!parseStatusLine(line, &responseCode, &responseReason))
			{
				cleanup();
				state = Idle;
				errorCondition = ErrorGeneric;
				emit q->error();
				return false;
			}

			inStatusLine = false;
		}
		else
		{
			if(line.isEmpty())
			{
				if(responseCode == 101)
				{
					// TODO: confirm Sec-WebSocket-Accept == base64(sha1(requestKey + MAGIC_STRING))

					state = Connected;
					emit q->connected();
				}
				else
				{
					// we'll read the response body before acting
					if(responseHeaders.contains("Content-Length"))
					{
						bool ok;
						responseContentLength = responseHeaders.get("Content-Length").toInt(&ok);
						if(!ok)
						{
							cleanup();
							state = Idle;
							errorCondition = ErrorGeneric;
							emit q->error();
							return false;
						}
					}
					else if(responseHeaders.get("Transfer-Encoding") == "chunked")
					{
						chunked = true;
					}

					// remove these headers as we'll rewrite Content-Length based on read limit
					responseHeaders.removeAll("Content-Length");
					responseHeaders.removeAll("Transfer-Encoding");

					readingResponseBody = true;
				}
			}
			else
			{
				int at = line.indexOf(": ");
				if(at == -1)
				{
					cleanup();
					state = Idle;
					errorCondition = ErrorGeneric;
					emit q->error();
					return false;
				}

				responseHeaders += HttpHeader(line.mid(0, at), line.mid(at + 2));
			}
		}

		return true;
	}

	void handleResponse()
	{
		if(maxRedirects > 0
			&& (responseCode == 301 || responseCode == 302 || responseCode == 303 || responseCode == 307 || responseCode == 308)
			&& responseHeaders.contains("Location"))
		{
			QByteArray location = responseHeaders.get("Location");

			log_debug("ws: received redirect response, code=%d location=[%s]", responseCode, location.data());

			if(followedRedirects >= maxRedirects)
			{
				log_debug("ws: too many redirects");

				cleanup();
				state = Idle;
				errorCondition = ErrorGeneric;
				emit q->error();
				return;
			}

			++followedRedirects;

			requestUri = QUrl::fromEncoded(location);

			cleanup();
			tryConnect();
		}
		else
		{
			// force content-length on rejections
			responseHeaders += HttpHeader("Content-Length", QByteArray::number(responseBody.size()));

			cleanup();
			state = Idle;
			errorCondition = ErrorRejected;
			emit q->error();
		}
	}

	// return true if new frame to read
	bool handleIncomingFrame(bool fin, int opcode, const QByteArray &data)
	{
		// skip any frames after close frame
		if(peerClosing)
			return false;

		// close message?
		if(opcode == 8)
		{
			peerClosing = true;

			if(data.count() >= 2)
			{
				peerCloseCode = read16((const quint8 *)data.data());
				peerCloseReason = QString::fromUtf8(data.mid(2));
				log_debug("ws: received peer close: %d [%s]", peerCloseCode, qPrintable(peerCloseReason));
			}
			else
				log_debug("ws: received peer close");

			if(state == Closing)
				sock->disconnectFromHost();
			else
				emit q->peerClosing();

			return false;
		}

		log_debug("ws: received frame type=%d, size=%d", opcode, data.size());

		Frame::Type ftype;
		if(opcode == 0)
			ftype = Frame::Continuation;
		else if(opcode == 1)
			ftype = Frame::Text;
		else if(opcode == 2)
			ftype = Frame::Binary;
		else if(opcode == 9)
			ftype = Frame::Ping;
		else if(opcode == 10)
			ftype = Frame::Pong;
		else
		{
			// ignore unknown frame type
			return false;
		}

		in += Frame(ftype, data, !fin);
		inBytes += data.size();
		return true;
	}

	void tryProcessFrames()
	{
		QPointer<QObject> self = this;

		bool newFrames = false;
		bool ok = true;
		while(ok)
		{
			ok = tryProcessFrame();
			if(!self)
				return;

			if(ok)
				newFrames = true;
		}

		if(newFrames)
			emit q->readyRead();
	}

	// return true if new frame to read, and that we should try again
	bool tryProcessFrame()
	{
		quint64 size;
		int ret = checkFrame((const quint8 *)inbuf.data(), inbuf.size(), &size);
		if(ret >= 1 && (maxFrameSize == -1 || size > (quint64)maxFrameSize))
		{
			cleanup();
			state = Idle;
			errorCondition = ErrorFrameTooLarge;
			emit q->error();
			return false;
		}

		if(ret == 2)
		{
			bool fin;
			int opcode;
			int bytesRead;
			QByteArray data = parseFrame((const quint8 *)inbuf.data(), &fin, &opcode, &bytesRead);
			inbuf = inbuf.mid(bytesRead);

			return handleIncomingFrame(fin, opcode, data);
		}

		return false;
	}

	void tryProcessBody()
	{
		bool eof = false;

		if(chunked)
		{
			while(!eof)
			{
				quint64 size;
				int ret = checkChunk((const quint8 *)inbuf.data(), inbuf.size(), &size);
				if(ret < 0)
				{
					cleanup();
					state = Idle;
					errorCondition = ErrorGeneric;
					emit q->error();
					return;
				}

				if(ret < 1)
				{
					// no data and size unknown
					break;
				}

				if(responseBody.size() + size > RESPONSE_BODY_MAX)
				{
					// can't fit the next chunk. we'll stop now
					eof = true;
				}
				else if(ret == 2)
				{
					int bytesRead;
					QByteArray chunk = parseChunk((const quint8 *)inbuf.data(), inbuf.size(), &bytesRead);
					inbuf = inbuf.mid(bytesRead);

					if(!chunk.isEmpty())
						responseBody += chunk;
					else
						eof = true;
				}
			}
		}
		else
		{
			if(!inbuf.isEmpty())
			{
				int avail = RESPONSE_BODY_MAX - responseBody.size();

				// don't read more than Content-Length
				if(responseContentLength != -1)
					avail = qMin(avail, responseContentLength - responseBody.size());

				int size = qMin(inbuf.size(), avail);
				responseBody += inbuf.mid(0, size);
				inbuf = inbuf.mid(size);

				assert(responseBody.size() <= RESPONSE_BODY_MAX);
			}

			if(responseContentLength != -1)
			{
				assert(responseBody.size() <= responseContentLength);

				if(responseBody.size() == responseContentLength || responseBody.size() == RESPONSE_BODY_MAX)
					eof = true;
			}
			else
			{
				if(responseBody.size() == RESPONSE_BODY_MAX)
					eof = true;
			}

			// if there are any bytes left we must be at the end
			assert(inbuf.isEmpty() || eof);
		}

		if(eof)
			handleResponse();
	}

private slots:
	void tryNextAddress()
	{
		QPointer<QObject> self = this;

		if(addrs.isEmpty())
		{
			state = Idle;
			errorCondition = mostSignificantError;
			emit q->error();
			return;
		}

		QHostAddress addr = addrs.takeFirst();

		log_debug("ws: trying %s", qPrintable(addr.toString()));

		emit q->nextAddress(addr);
		if(!self)
			return;

		sock = new QSslSocket(this);
		connect(sock, &QSslSocket::connected, this, &Private::sock_connected);
		connect(sock, &QSslSocket::readyRead, this, &Private::sock_readyRead);
		connect(sock, &QSslSocket::bytesWritten, this, &Private::sock_bytesWritten);
		connect(sock, &QSslSocket::disconnected, this, &Private::sock_disconnected);
		connect(sock, static_cast<void (QSslSocket::*)(QAbstractSocket::SocketError)>(&QSslSocket::error), this, &Private::sock_error);
		connect(sock, static_cast<void (QSslSocket::*)(const QList<QSslError> &)>(&QSslSocket::sslErrors), this, &Private::sock_sslErrors);

		bool useSsl = (requestUri.scheme() == "wss");
		int port = requestUri.port(useSsl ? 443 : 80);

		log_debug("ws: connecting to %s:%d%s", qPrintable(addr.toString()), port, useSsl ? " (ssl)" : "");

		if(useSsl)
			sock->connectToHostEncrypted(addr.toString(), port, requestUri.host());
		else
			sock->connectToHost(host, port);
	}

	void resolver_resultsReady(const QList<QHostAddress> &results)
	{
		addrs += results;
		tryNextAddress();
	}

	void resolver_error()
	{
		state = Idle;
		errorCondition = ErrorConnect;
		emit q->error();
	}

	void tryRead()
	{
		pendingRead = false;

		// don't read if we're at limit
		if(maxFrameSize != -1 && inBytes >= maxFrameSize)
			return;

		QByteArray buf = sock->readAll();
		if(buf.isEmpty())
			return;

		log_debug("ws: read: %d", buf.size());
		inbuf += buf;

		tryProcessFrames();
	}

	void sock_connected()
	{
		log_debug("ws: connected");

		QByteArray path = requestUri.path(QUrl::FullyEncoded).toUtf8();
		if(path.isEmpty())
			path = "/";

		if(requestUri.hasQuery())
			path += '?' + requestUri.query(QUrl::FullyEncoded).toUtf8();

		requestKey = generateKey();

		requestHeaders.removeAll("Upgrade");
		requestHeaders.removeAll("Connection");
		requestHeaders.removeAll("Sec-WebSocket-Version");
		requestHeaders.removeAll("Sec-WebSocket-Key");
		requestHeaders.removeAll("Accept-Encoding"); // we only support unencoded responses

		// note: we let Sec-WebSocket-Extensions and
		//   Sec-WebSocket-Protocol go through. clients should take
		//   care to not send connection-level extensions, as we won't
		//   be able to understand them

		if(!requestHeaders.contains("Host"))
		{
			QByteArray hostHeader = requestUri.host().toUtf8();
			if(requestUri.port() != -1)
				hostHeader += ':' + QByteArray::number(requestUri.port());
			requestHeaders += HttpHeader("Host", hostHeader);
		}

		requestHeaders += HttpHeader("Upgrade", "websocket");
		requestHeaders += HttpHeader("Connection", "Upgrade");
		requestHeaders += HttpHeader("Sec-WebSocket-Version", "13");
		requestHeaders += HttpHeader("Sec-WebSocket-Key", requestKey.toBase64());

		QByteArray buf = "GET " + path + " HTTP/1.1\r\n";
		foreach(const HttpHeader &h, requestHeaders)
			buf += h.first + ": " + h.second + "\r\n";
		buf += "\r\n";

		log_debug("ws: sending handshake: [%s]", buf.data());
		pendingWrites += WriteItem(buf.size());
		sock->write(buf);
	}

	void sock_readyRead()
	{
		log_debug("ws: readyRead");

		if(state == Connecting)
		{
			QByteArray buf = sock->readAll();
			log_debug("ws: read: %d", buf.size());
			inbuf += buf;

			if(!readingResponseBody)
			{
				QPointer<QObject> self = this;
				bool ok = true;
				while(state == Connecting && !readingResponseBody && ok)
				{
					int at = inbuf.indexOf('\n');
					if(at == -1)
						return;

					QByteArray line;
					if(at > 0 && inbuf[at - 1] == '\r')
					{
						--at;
						line = inbuf.mid(0, at);
						inbuf = inbuf.mid(at + 2);
					}
					else
					{
						line = inbuf.mid(0, at);
						inbuf = inbuf.mid(at + 1);
					}

					ok = handleResponseLine(line);
					if(!self)
						return;
				}

				if(!ok)
					return;
			}

			if(state == Connected)
				tryProcessFrames();
			else
				tryProcessBody();
		}
		else
		{
			tryRead();
		}
	}

	void sock_bytesWritten(qint64 bytes)
	{
		int written = 0;
		int left = bytes;

		log_debug("ws: bytesWritten: %d", left);

		while(left > 0)
		{
			assert(!pendingWrites.isEmpty());

			WriteItem &wi = pendingWrites.first();
			int take = qMin(wi.size, left);
			wi.size -= take;
			left -= take;
			if(wi.size == 0)
			{
				if(wi.type == WriteItem::Frame && wi.opcode != 8)
					++written;

				pendingWrites.removeFirst();
			}
		}

		if(written > 0)
			emit q->framesWritten(written);
	}

	void sock_disconnected()
	{
		log_debug("ws: disconnected");

		cleanup();
		state = Idle;
		emit q->closed();
	}

	void sock_error(QAbstractSocket::SocketError socketError)
	{
		log_debug("ws: sock_error: %d", (int)socketError);

		bool tryAgain = true;

		ErrorCondition curError;
		switch(socketError)
		{
			case QAbstractSocket::ConnectionRefusedError:
				curError = ErrorConnect;
				break;
			case QAbstractSocket::RemoteHostClosedError:
				if(state == Closing && peerClosing)
				{
					// we should not get this error in
					// this state, but if we do, ignore
					// it. the disconnected signal will
					// come soon after
					return;
				}

				if(readingResponseBody && responseContentLength == -1 && !chunked)
				{
					handleResponse();
					return;
				}
				curError = ErrorGeneric;
				break;
			case QAbstractSocket::SslHandshakeFailedError:
				curError = ErrorTls;
				tryAgain = false;
				break;
			default:
				curError = ErrorGeneric;
		}

		if(!tryAgain || state == Connected)
		{
			cleanup();
			state = Idle;
			errorCondition = curError;
			emit q->error();
			return;
		}

		if(errorPriority(curError) > errorPriority(mostSignificantError))
			mostSignificantError = curError;

		cleanup();
		tryNextAddress();
	}

	void sock_sslErrors(const QList<QSslError> &errors)
	{
		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
		{
			QStringList strs;
			foreach(const QSslError &e, errors)
				strs += e.errorString();
			log_debug("ws: sslErrors: %s", qPrintable(strs.join(", ")));
		}

		bool hostMismatchOk = false;
		if(errors.count() == 1 && errors[0].error() == QSslError::HostNameMismatch)
		{
			// if hostname doesn't match, check against connect host if trusted
			if(!connectHost.isEmpty() && trustConnectHost)
			{
#ifdef HAVE_OPENSSL
				QSslCertificate cert = sock->peerCertificate();
				QByteArray der = cert.toDer();
				const unsigned char *p = (const unsigned char *)der.data();
				X509 *opensslCert = d2i_X509(NULL, &p, der.size());
				if(opensslCert)
				{
					if(verifyhost(connectHost.toUtf8().data(), opensslCert) == CURLE_OK)
						hostMismatchOk = true;

					X509_free(opensslCert);
				}
#endif
			}
		}

		if(ignoreTlsErrors || hostMismatchOk)
			sock->ignoreSslErrors();
	}
};

WebSocket::WebSocket(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

WebSocket::~WebSocket()
{
	delete d;
}

void WebSocket::setConnectHost(const QString &host)
{
	d->connectHost = host;
}

void WebSocket::setTrustConnectHost(bool on)
{
	d->trustConnectHost = on;
}

void WebSocket::setIgnoreTlsErrors(bool on)
{
	d->ignoreTlsErrors = on;
}

void WebSocket::setFollowRedirects(int maxRedirects)
{
	d->maxRedirects = maxRedirects;
}

void WebSocket::setMaxFrameSize(int size)
{
	d->maxFrameSize = size;
}

void WebSocket::start(const QUrl &uri, const HttpHeaders &headers)
{
	d->start(uri, headers);
}

WebSocket::State WebSocket::state() const
{
	return d->state;
}

int WebSocket::responseCode() const
{
	return d->responseCode;
}

QByteArray WebSocket::responseReason() const
{
	return d->responseReason;
}

HttpHeaders WebSocket::responseHeaders() const
{
	return d->responseHeaders;
}

int WebSocket::framesAvailable() const
{
	return d->in.count();
}

int WebSocket::nextFrameSize() const
{
	return d->in.first().data.size();
}

int WebSocket::peerCloseCode() const
{
	return d->peerCloseCode;
}

QString WebSocket::peerCloseReason() const
{
	return d->peerCloseReason;
}

WebSocket::ErrorCondition WebSocket::errorCondition() const
{
	return d->errorCondition;
}

QByteArray WebSocket::readResponseBody()
{
	return d->responseBody.take();
}

void WebSocket::writeFrame(const Frame &frame)
{
	d->writeFrame(frame);
}

WebSocket::Frame WebSocket::readFrame()
{
	return d->readFrame();
}

void WebSocket::close(int code, const QString &reason)
{
	d->close(code, reason);
}

#include "websocket.moc"
