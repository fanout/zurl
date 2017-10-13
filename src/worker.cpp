/*
 * Copyright (C) 2012-2017 Fanout, Inc.
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

#include "worker.h"

#include <assert.h>
#include <QVariant>
#include <QTimer>
#include <QPointer>
#include "qjdnsshared.h"
#include "httprequest.h"
#include "websocket.h"
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "bufferlist.h"
#include "log.h"
#include "appconfig.h"

#define SESSION_EXPIRE 60000

class Worker::Private : public QObject
{
	Q_OBJECT

public:
	enum Transport
	{
		HttpTransport,
		WebSocketTransport
	};

	enum State
	{
		NotStarted,
		Started,
		Closing,
		PeerClosing,
		CloseWait,
		Finished,
		Cancel,
		Error,
		Stopped
	};

	Worker *q;
	QJDnsShared *dns;
	AppConfig *config;
	Transport transport;
	Worker::Format format;
	QByteArray toAddress;
	QByteArray rid;
	State state;
	QByteArray errorCondition;
	int inSeq, outSeq;
	int outCredits;
	bool outStream;
	QVariant userData;
	int maxResponseSize;
	bool ignorePolicies;
	int sessionTimeout;
	HttpRequest *hreq;
	WebSocket *ws;
	bool quiet;
	bool sentHeader;
	bool bodySent;
	bool stuffToRead;
	BufferList inbuf; // for single mode
	int bytesReceived;
	QTimer *expireTimer;
	QTimer *httpActivityTimer;
	QTimer *httpSessionTimer;
	QTimer *keepAliveTimer;
	QTimer *updateTimer;
	WebSocket::Frame::Type lastReceivedFrameType;
	bool wsSendingMessage;
	QList<int> wsPendingWrites;
	bool wsClosed;
	bool wsPendingPeerClose;

	Private(QJDnsShared *_dns, AppConfig *_config, Worker::Format _format, Worker *_q) :
		QObject(_q),
		q(_q),
		dns(_dns),
		config(_config),
		format(_format),
		state(NotStarted),
		hreq(0),
		ws(0),
		expireTimer(0),
		httpActivityTimer(0),
		httpSessionTimer(0),
		keepAliveTimer(0),
		lastReceivedFrameType(WebSocket::Frame::Text),
		wsSendingMessage(false),
		wsClosed(false),
		wsPendingPeerClose(false)
	{
		updateTimer = new QTimer(this);
		connect(updateTimer, &QTimer::timeout, this, &Private::doUpdate);
		updateTimer->setSingleShot(true);
	}

	~Private()
	{
		cleanup();

		updateTimer->disconnect(this);
		updateTimer->setParent(0);
		updateTimer->deleteLater();
	}

	void cleanup()
	{
		updateTimer->stop();

		delete hreq;
		hreq = 0;

		delete ws;
		ws = 0;

		if(expireTimer)
		{
			expireTimer->disconnect(this);
			expireTimer->setParent(0);
			expireTimer->deleteLater();
			expireTimer = 0;
		}

		if(httpActivityTimer)
		{
			httpActivityTimer->disconnect(this);
			httpActivityTimer->setParent(0);
			httpActivityTimer->deleteLater();
			httpActivityTimer = 0;
		}

		if(httpSessionTimer)
		{
			httpSessionTimer->disconnect(this);
			httpSessionTimer->setParent(0);
			httpSessionTimer->deleteLater();
			httpSessionTimer = 0;
		}

		if(keepAliveTimer)
		{
			keepAliveTimer->disconnect(this);
			keepAliveTimer->setParent(0);
			keepAliveTimer->deleteLater();
			keepAliveTimer = 0;
		}

		state = Stopped;
	}

	void start(const QVariant &vrequest, Mode mode)
	{
		outSeq = 0;
		outCredits = 0;
		quiet = false;

		state = Started;

		ZhttpRequestPacket request;
		if(!request.fromVariant(vrequest))
		{
			log_warning("failed to parse zurl request");

			QVariantHash vhash = vrequest.toHash();
			rid = vhash.value("id").toByteArray();
			toAddress = vhash.value("from").toByteArray();
			QByteArray type = vhash.value("type").toByteArray();
			if(!toAddress.isEmpty() && type != "error" && type != "cancel")
				deferError("bad-request");
			else
				deferFinished();

			return;
		}

		int seq = -1;
		if(!request.ids.isEmpty())
		{
			rid = request.ids.first().id;
			seq = request.ids.first().seq;
		}

		toAddress = request.from;
		userData = request.userData;
		sentHeader = false;
		stuffToRead = false;
		bytesReceived = 0;

		ignorePolicies = request.ignorePolicies;
		sessionTimeout = -1;

		if(request.uri.isEmpty())
		{
			log_warning("missing request uri");

			deferError("bad-request");
			return;
		}

		QString scheme = request.uri.scheme();
		if(scheme == "https" || scheme == "http")
		{
			transport = HttpTransport;
		}
		else if(scheme == "wss" || scheme == "ws")
		{
			transport = WebSocketTransport;
		}
		else
		{
			log_warning("unsupported scheme");

			deferError("bad-request");
			return;
		}

		if(transport == WebSocketTransport && mode != Worker::Stream)
		{
			log_warning("websocket must be used from stream interface");

			deferError("bad-request");
			return;
		}

		int defaultPort;
		if(scheme == "https" || scheme == "wss")
			defaultPort = 443;
		else // http || wss
			defaultPort = 80;

		HttpHeaders headers = request.headers;

		if(transport == HttpTransport)
		{
			// fire and forget
			if(mode == Worker::Stream && (rid.isEmpty() || toAddress.isEmpty()))
				quiet = true;

			// streaming only allowed on streaming interface
			if(mode == Worker::Stream)
				outStream = request.stream;
			else
				outStream = false;

			if(request.method.isEmpty())
			{
				log_warning("missing request method");

				deferError("bad-request");
				return;
			}

			log_info("IN id=%s, %s %s", rid.data(), qPrintable(request.method), request.uri.toEncoded().data());

			// inbound streaming must start with sequence number of 0
			if(mode == Worker::Stream && request.more && seq != 0)
			{
				log_warning("streamed input must start with seq 0");

				deferError("bad-request");
				return;
			}

			// can't use these two together
			if(mode == Worker::Single && request.more)
			{
				log_warning("cannot use streamed input on router interface");

				deferError("bad-request");
				return;
			}

			bodySent = false;

			inSeq = seq;

			if(!isAllowed(request.uri.host()) || (!request.connectHost.isEmpty() && !isAllowed(request.connectHost)))
			{
				deferError("policy-violation");
				return;
			}

			QByteArray hostHeader = request.uri.host().toUtf8();

			int port = request.uri.port(defaultPort);
			if(port != defaultPort)
				hostHeader += ':' + QByteArray::number(port);

			if(headers.get("Host") != hostHeader)
			{
				headers.removeAll("Host");
				headers += HttpHeader("Host", hostHeader);
			}

			hreq = new HttpRequest(dns, this);
			connect(hreq, &HttpRequest::nextAddress, this, &Private::req_nextAddress);
			connect(hreq, &HttpRequest::readyRead, this, &Private::req_readyRead);
			connect(hreq, &HttpRequest::bytesWritten, this, &Private::req_bytesWritten);
			connect(hreq, &HttpRequest::error, this, &Private::req_error);

			maxResponseSize = request.maxSize;
			sessionTimeout = request.timeout;

			if(!request.connectHost.isEmpty())
				hreq->setConnectHost(request.connectHost);
			if(request.connectPort != -1)
				request.uri.setPort(request.connectPort);

			hreq->setTrustConnectHost(request.trustConnectHost);
			hreq->setIgnoreTlsErrors(request.ignoreTlsErrors);
			if(request.followRedirects)
				hreq->setFollowRedirects(8);

			if(request.credits != -1)
				outCredits += request.credits;
		}
		else // WebSocketTransport
		{
			log_info("IN id=%s, %s", rid.data(), request.uri.toEncoded().data());

			// inbound streaming must start with sequence number of 0
			if(seq != 0)
			{
				log_warning("websocket input must start with seq 0");

				deferError("bad-request");
				return;
			}

			if(toAddress.isEmpty())
			{
				log_warning("websocket input must provide from address");

				deferError("bad-request");
				return;
			}

			inSeq = seq;

			if(!isAllowed(request.uri.host()) || (!request.connectHost.isEmpty() && !isAllowed(request.connectHost)))
			{
				deferError("policy-violation");
				return;
			}

			QByteArray hostHeader = request.uri.host().toUtf8();

			int port = request.uri.port(defaultPort);
			if(port != defaultPort)
				hostHeader += ":" + QByteArray::number(port);

			if(headers.get("Host") != hostHeader)
			{
				headers.removeAll("Host");
				headers += HttpHeader("Host", hostHeader);
			}

			ws = new WebSocket(dns, this);
			connect(ws, &WebSocket::nextAddress, this, &Private::req_nextAddress);
			connect(ws, &WebSocket::connected, this, &Private::ws_connected);
			connect(ws, &WebSocket::readyRead, this, &Private::ws_readyRead);
			connect(ws, &WebSocket::framesWritten, this, &Private::ws_framesWritten);
			connect(ws, &WebSocket::peerClosing, this, &Private::ws_peerClosing);
			connect(ws, &WebSocket::closed, this, &Private::ws_closed);
			connect(ws, &WebSocket::error, this, &Private::ws_error);

			if(!request.connectHost.isEmpty())
				ws->setConnectHost(request.connectHost);
			if(request.connectPort != -1)
				request.uri.setPort(request.connectPort);

			ws->setTrustConnectHost(request.trustConnectHost);
			ws->setIgnoreTlsErrors(request.ignoreTlsErrors);
			if(request.followRedirects)
				ws->setFollowRedirects(8);
			ws->setMaxFrameSize(config->sessionBufferSize);

			if(request.credits != -1)
				outCredits += request.credits;
		}

		httpActivityTimer = new QTimer(this);
		connect(httpActivityTimer, &QTimer::timeout, this, &Private::httpActivity_timeout);
		httpActivityTimer->setSingleShot(true);
		httpActivityTimer->start(config->activityTimeout * 1000);

		if(sessionTimeout != -1)
		{
			httpSessionTimer = new QTimer(this);
			connect(httpSessionTimer, &QTimer::timeout, this, &Private::httpSession_timeout);
			httpSessionTimer->setSingleShot(true);
			httpSessionTimer->start(sessionTimeout);
		}

		if(transport == WebSocketTransport || (transport == HttpTransport && mode == Worker::Stream))
		{
			expireTimer = new QTimer(this);
			connect(expireTimer, &QTimer::timeout, this, &Private::expire_timeout);
			expireTimer->setSingleShot(true);
			expireTimer->start(SESSION_EXPIRE);

			keepAliveTimer = new QTimer(this);
			connect(keepAliveTimer, &QTimer::timeout, this, &Private::keepAlive_timeout);
			keepAliveTimer->start(SESSION_EXPIRE / 2);
		}

		if(transport == HttpTransport)
		{
			if(!request.body.isEmpty() && !request.more && !headers.contains("Content-Length"))
				headers += HttpHeader("Content-Length", QByteArray::number(request.body.size()));

			bool hasOrMightHaveBody = (!request.body.isEmpty() || request.more);

			hreq->start(request.method, request.uri, headers, hasOrMightHaveBody);

			if(hasOrMightHaveBody)
			{
				if(!request.body.isEmpty())
					hreq->writeBody(request.body);

				if(!request.more)
				{
					bodySent = true;
					hreq->endBody();
				}
			}
			else
				bodySent = true;

			if(mode == Stream)
			{
				if(request.more)
				{
					// send cts
					ZhttpResponsePacket resp;
					resp.type = ZhttpResponsePacket::Credit;
					resp.credits = config->sessionBufferSize;
					writeResponse(resp);
				}
				else
				{
					// send ack
					ZhttpResponsePacket resp;
					resp.type = ZhttpResponsePacket::KeepAlive;
					writeResponse(resp);
				}
			}
		}
		else // WebSocketTransport
		{
			ws->start(request.uri, headers);
		}
	}

	void write(const QVariant &vrequest)
	{
		if(!(state == Started || state == Closing || state == PeerClosing))
			return;

		ZhttpRequestPacket request;
		if(!request.fromVariant(vrequest))
		{
			QVariantHash vhash = vrequest.toHash();
			if(vhash["type"].toByteArray() != "cancel")
				deferError("bad-request");
			else
				deferFinished();

			return;
		}

		int seq = -1;
		if(!request.ids.isEmpty())
			seq = request.ids.first().seq;

		// cancel session if a wrong sequenced packet is received
		if(inSeq == -1 || seq == -1 || seq != inSeq + 1)
		{
			if(request.type != ZhttpRequestPacket::Cancel)
				deferCancel();
			else
				deferFinished();

			return;
		}

		if(request.type == ZhttpRequestPacket::Cancel)
		{
			deferFinished();
			return;
		}

		inSeq = seq;

		refreshTimeout();

		// all we care about from follow-up writes are body and credits

		if(request.credits != -1)
			outCredits += request.credits;

		if(transport == HttpTransport)
		{
			if(request.type == ZhttpRequestPacket::Data)
			{
				if(bodySent)
				{
					deferError("bad-request");
					return;
				}

				refreshActivityTimeout();

				if(!request.body.isEmpty())
					hreq->writeBody(request.body);

				// the 'more' flag only has significance if body field present
				if(!request.more)
				{
					bodySent = true;
					hreq->endBody();
				}
			}
		}
		else // WebSocketTransport
		{
			if(request.type == ZhttpRequestPacket::Data || request.type == ZhttpRequestPacket::Close || request.type == ZhttpRequestPacket::Ping || request.type == ZhttpRequestPacket::Pong)
			{
				if(wsClosed)
				{
					deferError("bad-request");
					return;
				}

				if(request.type == ZhttpRequestPacket::Data || request.type == ZhttpRequestPacket::Ping || request.type == ZhttpRequestPacket::Pong)
				{
					refreshActivityTimeout();

					if(request.type == ZhttpRequestPacket::Data)
					{
						WebSocket::Frame::Type ftype;
						if(wsSendingMessage)
							ftype = WebSocket::Frame::Continuation;
						else if(request.contentType == "binary")
							ftype = WebSocket::Frame::Binary;
						else
							ftype = WebSocket::Frame::Text;

						wsSendingMessage = request.more;

						wsPendingWrites += request.body.size();
						ws->writeFrame(WebSocket::Frame(ftype, request.body, request.more));
					}
					else if(request.type == ZhttpRequestPacket::Ping)
					{
						wsPendingWrites += 0;
						ws->writeFrame(WebSocket::Frame(WebSocket::Frame::Ping, QByteArray(), false));
					}
					else if(request.type == ZhttpRequestPacket::Pong)
					{
						wsPendingWrites += 0;
						ws->writeFrame(WebSocket::Frame(WebSocket::Frame::Pong, QByteArray(), false));
					}
				}
				else if(request.type == ZhttpRequestPacket::Close)
				{
					ws->close(request.code);

					wsClosed = true;

					if(state == Started)
						state = Closing;
					else // PeerClosing
						state = CloseWait;
				}
			}
		}

		// if we needed credits to send something, take care of that now
		if(request.credits != -1 && stuffToRead)
			update();
	}

	static bool matchExp(const QString &exp, const QString &s)
	{
		int at = exp.indexOf('*');
		if(at != -1)
		{
			QString start = exp.mid(0, at);
			QString end = exp.mid(at + 1);
			return (s.startsWith(start, Qt::CaseInsensitive) && s.endsWith(end, Qt::CaseInsensitive));
		}
		else
			return s.compare(exp, Qt::CaseInsensitive);
	}

	bool checkAllow(const QString &in) const
	{
		foreach(const QString &exp, config->allowExps)
		{
			if(matchExp(exp, in))
				return true;
		}

		return false;
	}

	bool checkDeny(const QString &in) const
	{
		foreach(const QString &exp, config->denyExps)
		{
			if(matchExp(exp, in))
				return true;
		}

		return false;
	}

	bool isAllowed(const QString &in) const
	{
		if(ignorePolicies)
			return true;

		if(config->defaultPolicy == "allow")
			return !checkDeny(in) || checkAllow(in);
		else
			return checkAllow(in) && !checkDeny(in);
	}

	// emits signals, but safe to delete after
	void writeResponse(const ZhttpResponsePacket &resp)
	{
		ZhttpResponsePacket out = resp;

		if(!toAddress.isEmpty())
			out.from = config->clientId;

		QByteArray outRid;
		if(!rid.isEmpty())
		{
			out.ids.clear();
			out.ids += ZhttpResponsePacket::Id(rid, outSeq++);

			outRid = rid;
		}

		out.userData = userData;

		// only log if error or body packet. this way we don't log cts or credits-only packets

		if(out.type == ZhttpResponsePacket::Error)
		{
			log_info("OUT ERR id=%s condition=%s", outRid.data(), out.condition.data());
		}
		else if(out.type == ZhttpResponsePacket::Data)
		{
			if(resp.code != -1)
				log_info("OUT id=%s code=%d %d%s", outRid.data(), out.code, out.body.size(), out.more ? " M" : "");
			else
				log_debug("OUT id=%s %d%s", outRid.data(), out.body.size(), out.more ? " M" : "");
		}

		if(!quiet)
			emit q->readyRead(toAddress, out.toVariant());
	}

	void update()
	{
		if(!updateTimer->isActive())
			updateTimer->start();
	}

	void deferFinished()
	{
		cleanup();
		state = Finished;
		update();
	}

	void deferCancel()
	{
		cleanup();
		state = Cancel;
		update();
	}

	void deferError(const QByteArray &condition)
	{
		cleanup();
		state = Error;
		errorCondition = condition;
		update();
	}

	void refreshTimeout()
	{
		expireTimer->start(SESSION_EXPIRE);
	}

	void refreshActivityTimeout()
	{
		httpActivityTimer->start(config->activityTimeout * 1000);
	}

	void respondError(const QByteArray &condition)
	{
		respondError(condition, -1, QByteArray(), HttpHeaders(), QByteArray());
	}

	void respondError(const QByteArray &condition, int code, const QByteArray &reason, const HttpHeaders &headers, const QByteArray &body)
	{
		QPointer<QObject> self = this;

		ZhttpResponsePacket resp;
		resp.type = ZhttpResponsePacket::Error;
		resp.condition = condition;

		if(code != -1)
		{
			resp.code = code;
			resp.reason = reason;
			resp.headers = headers;
			resp.body = body;
		}

		writeResponse(resp);
		if(!self)
			return;

		cleanup();
		emit q->finished();
	}

	void respondCancel()
	{
		QPointer<QObject> self = this;

		ZhttpResponsePacket resp;
		resp.type = ZhttpResponsePacket::Cancel;

		writeResponse(resp);
		if(!self)
			return;

		cleanup();
		emit q->finished();
	}

private slots:
	// emits signals, but safe to delete after
	void doUpdate()
	{
		QPointer<QObject> self = this;

		// if we had a pending update, we can cancel since we're updating now
		if(updateTimer->isActive())
			updateTimer->stop();

		if(transport == HttpTransport)
		{
			if(state == Started)
			{
				if(!stuffToRead)
					return;

				stuffToRead = false;

				ZhttpResponsePacket resp;
				resp.type = ZhttpResponsePacket::Data;

				if(!sentHeader)
				{
					resp.code = hreq->responseCode();
					resp.reason = hreq->responseReason();
					resp.headers = hreq->responseHeaders();
					sentHeader = true;
				}

				// note: we always set body, even if empty

				if(outStream)
				{
					// note: we skip credits handling if quiet mode

					QByteArray buf;

					if(!quiet)
						buf = hreq->readResponseBody(outCredits);
					else
						buf = hreq->readResponseBody(); // all

					if(!buf.isEmpty())
					{
						if(maxResponseSize != -1 && bytesReceived + buf.size() > maxResponseSize)
						{
							respondError("max-size-exceeded");
							return;
						}
					}

					resp.body = buf;

					bytesReceived += resp.body.size();

					if(!quiet)
						outCredits -= resp.body.size();

					resp.more = (hreq->bytesAvailable() > 0 || !hreq->isFinished());

					if(hreq->bytesAvailable() > 0)
						stuffToRead = true;
				}
				else
				{
					resp.body = inbuf.take();
				}

				writeResponse(resp);
				if(!self)
					return;

				if(!resp.more)
				{
					cleanup();
					emit q->finished();
				}
			}
			else if(state == Finished)
			{
				cleanup();
				emit q->finished();
			}
			else if(state == Cancel)
			{
				respondCancel();
			}
			else if(state == Error)
			{
				respondError(errorCondition);
			}
		}
		else // WebSocketTransport
		{
			if(state == Started || state || Closing || state == PeerClosing)
			{
				if(stuffToRead)
				{
					stuffToRead = false;

					while(ws->framesAvailable() > 0 && outCredits >= ws->nextFrameSize())
					{
						WebSocket::Frame frame = ws->readFrame();
						outCredits -= frame.data.size();

						if(frame.type == WebSocket::Frame::Continuation || frame.type == WebSocket::Frame::Text || frame.type == WebSocket::Frame::Binary)
						{
							if(frame.type == WebSocket::Frame::Continuation)
								frame.type = lastReceivedFrameType;
							else
								lastReceivedFrameType = frame.type;

							ZhttpResponsePacket resp;
							resp.type = ZhttpResponsePacket::Data;
							if(frame.type == WebSocket::Frame::Binary)
								resp.contentType = "binary";
							resp.body = frame.data;
							resp.more = frame.more;
							writeResponse(resp);
							if(!self)
								return;
						}
						else if(frame.type == WebSocket::Frame::Ping)
						{
							ZhttpResponsePacket resp;
							resp.type = ZhttpResponsePacket::Ping;
							writeResponse(resp);
							if(!self)
								return;
						}
						else if(frame.type == WebSocket::Frame::Pong)
						{
							ZhttpResponsePacket resp;
							resp.type = ZhttpResponsePacket::Pong;
							writeResponse(resp);
							if(!self)
								return;
						}
					}

					if(ws->framesAvailable() > 0)
					{
						stuffToRead = true;
					}
				}

				if(wsPendingPeerClose && !stuffToRead)
				{
					wsPendingPeerClose = false;

					ZhttpResponsePacket resp;
					resp.type = ZhttpResponsePacket::Close;
					resp.code = ws->peerCloseCode();
					writeResponse(resp);
					if(!self)
						return;

					if(state == Closing)
					{
						cleanup();
						emit q->finished();
						return;
					}

					state = PeerClosing;
				}
			}
			else if(state == Finished)
			{
				cleanup();
				emit q->finished();
			}
			else if(state == Cancel)
			{
				respondCancel();
			}
			else if(state == Error)
			{
				respondError(errorCondition);
			}
		}
	}

	void req_nextAddress(const QHostAddress &addr)
	{
		if(!isAllowed(addr.toString()))
			respondError("policy-violation");
	}

	void req_readyRead()
	{
		refreshActivityTimeout();

		stuffToRead = true;

		if(outStream)
		{
			// only wait for credits if not quiet
			if(!quiet && outCredits < 1)
				return;
		}
		else
		{
			// for non-streaming, collect the response
			QByteArray buf = hreq->readResponseBody();
			if(!buf.isEmpty())
			{
				if(maxResponseSize != -1 && bytesReceived + buf.size() > maxResponseSize)
				{
					respondError("max-size-exceeded");
					return;
				}

				inbuf += buf;
				bytesReceived += buf.size();
			}

			if(!hreq->isFinished())
				return;
		}

		// this will defer for a cycle, to pick up a potential disconnect as well
		update();
	}

	void req_bytesWritten(int count)
	{
		if(!bodySent)
		{
			ZhttpResponsePacket resp;
			resp.type = ZhttpResponsePacket::Credit;
			resp.credits = count;
			writeResponse(resp);
		}
	}

	void req_error()
	{
		QByteArray condition;
		switch(hreq->errorCondition())
		{
			case HttpRequest::ErrorPolicy:
				condition = "policy-violation"; break;
			case HttpRequest::ErrorConnect:
				condition = "remote-connection-failed"; break;
			case HttpRequest::ErrorTls:
				condition = "tls-error"; break;
			case HttpRequest::ErrorTimeout:
				condition = "connection-timeout"; break;
			case HttpRequest::ErrorBodyNotAllowed:
				condition = "content-not-allowed"; break;
			case HttpRequest::ErrorTooManyRedirects:
				condition = "too-many-redirects"; break;
			case HttpRequest::ErrorGeneric:
			default:
				condition = "undefined-condition";
				break;
		}

		respondError(condition);
	}

	void ws_connected()
	{
		refreshActivityTimeout();

		ZhttpResponsePacket resp;
		resp.type = ZhttpResponsePacket::Data;
		resp.code = ws->responseCode();
		resp.reason = ws->responseReason();
		resp.headers = ws->responseHeaders();
		resp.credits = config->sessionBufferSize;
		writeResponse(resp);
	}

	void ws_readyRead()
	{
		refreshActivityTimeout();

		stuffToRead = true;

		if(outCredits < 1)
			return;

		doUpdate();
	}

	void ws_framesWritten(int count)
	{
		int credits = 0;
		for(int n = 0; n < count; ++n)
			credits += wsPendingWrites.takeFirst();

		ZhttpResponsePacket resp;
		resp.type = ZhttpResponsePacket::Credit;
		resp.credits = credits;
		writeResponse(resp);
	}

	void ws_peerClosing()
	{
		assert(state == Started);

		// flag to peer close once all queued data is sent
		wsPendingPeerClose = true;

		doUpdate();
	}

	void ws_closed()
	{
		if(state == Closing)
		{
			// flag to peer close once all queued data is sent
			wsPendingPeerClose = true;

			doUpdate();
		}
		else if(state == CloseWait)
		{
			// we were acking the peer's close, so we're done
			cleanup();
			emit q->finished();
		}
	}

	void ws_error()
	{
		QByteArray condition;
		switch(ws->errorCondition())
		{
			case WebSocket::ErrorPolicy:
				condition = "policy-violation"; break;
			case WebSocket::ErrorConnect:
				condition = "remote-connection-failed"; break;
			case WebSocket::ErrorTls:
				condition = "tls-error"; break;
			case WebSocket::ErrorRejected:
				condition = "rejected"; break;
			case WebSocket::ErrorFrameTooLarge:
				condition = "frame-too-large"; break;
			case WebSocket::ErrorTimeout:
				condition = "connection-timeout"; break;
			case HttpRequest::ErrorGeneric:
			default:
				condition = "undefined-condition";
				break;
		}

		if(condition == "rejected")
			respondError(condition, ws->responseCode(), ws->responseReason(), ws->responseHeaders(), ws->readResponseBody());
		else
			respondError(condition);
	}

	void expire_timeout()
	{
		cleanup();
		emit q->finished();
	}

	void httpActivity_timeout()
	{
		respondError("session-timeout");
	}

	void httpSession_timeout()
	{
		respondError("session-timeout");
	}

	void keepAlive_timeout()
	{
		ZhttpResponsePacket resp;
		resp.type = ZhttpResponsePacket::KeepAlive;
		writeResponse(resp);
	}
};

Worker::Worker(QJDnsShared *dns, AppConfig *config, Format format, QObject *parent) :
	QObject(parent)
{
	d = new Private(dns, config, format, this);
}

Worker::~Worker()
{
	delete d;
}

QByteArray Worker::rid() const
{
	return d->rid;
}

Worker::Format Worker::format() const
{
	return d->format;
}

void Worker::start(const QVariant &request, Mode mode)
{
	d->start(request, mode);
}

void Worker::write(const QVariant &request)
{
	d->write(request);
}

#include "worker.moc"
