#include "worker.h"

#include <assert.h>
#include <QVariant>
#include <QTimer>
#include <QPointer>
#include "jdnsshared.h"
#include "httprequest.h"
#include "yurlrequestpacket.h"
#include "yurlresponsepacket.h"
#include "appconfig.h"
#include "log.h"

#define SESSION_TIMEOUT 600
#define IDEAL_CREDITS 200000

class Worker::Private : public QObject
{
	Q_OBJECT

public:
	Worker *q;
	JDnsShared *dns;
	AppConfig *config;
	QByteArray receiver;
	QByteArray rid;
	int inSeq, outSeq;
	int outCredits;
	bool outStream;
	QVariant userData;
	int maxResponseSize;
	HttpRequest *hreq;
	bool sentHeader;
	bool methodHasBody;
	bool bodySent;
	bool stuffToRead;
	QByteArray inbuf; // for single mode
	int bytesReceived;
	QTimer *timer;

	Private(JDnsShared *_dns, AppConfig *_config, Worker *_q) :
		QObject(_q),
		q(_q),
		dns(_dns),
		config(_config),
		hreq(0),
		timer(0)
	{
	}

	~Private()
	{
	}

	void cleanup()
	{
		delete hreq;
		hreq = 0;

		if(timer)
		{
			timer->disconnect(this);
			timer->setParent(0);
			timer->deleteLater();
			timer = 0;
		}
	}

	void start(const QVariant &vrequest, Mode mode)
	{
		inSeq = 0;
		outSeq = 0;

		YurlRequestPacket request;
		if(!request.fromVariant(vrequest))
		{
			QVariantHash vhash = vrequest.toHash();
			rid = vhash.value("id").toByteArray();
			assert(!rid.isEmpty()); // app layer ensures this
			receiver = vhash.value("sender").toByteArray();
			bool cancel = vhash.value("cancel").toBool();
			if(!receiver.isEmpty() && !cancel)
			{
				QMetaObject::invokeMethod(this, "respondError", Qt::QueuedConnection, Q_ARG(QByteArray, "bad-request"));
			}
			else
			{
				cleanup();
				QMetaObject::invokeMethod(q, "finished", Qt::QueuedConnection);
			}

			return;
		}

		rid = request.id;
		receiver = request.sender;
		outCredits = 0;
		userData = request.userData;
		sentHeader = false;
		stuffToRead = false;
		bytesReceived = 0;

		// streaming only allowed on streaming interface
		if(mode == Worker::Stream)
			outStream = request.stream;
		else
			outStream = false;

		// some required fields
		if(request.method.isEmpty() || request.url.isEmpty())
		{
			QMetaObject::invokeMethod(this, "respondError", Qt::QueuedConnection, Q_ARG(QByteArray, "bad-request"));
			return;
		}

		log_info("IN id=%s, %s %s", request.id.data(), qPrintable(request.method), qPrintable(request.url.toString()));

		// stream mode requires sender subscriber id and sequence number
		if(mode == Worker::Stream && (receiver.isEmpty() || request.seq != 0))
		{
			QMetaObject::invokeMethod(this, "respondError", Qt::QueuedConnection, Q_ARG(QByteArray, "bad-request"));
			return;
		}

		// can't use these two together
		if(mode == Worker::Single && request.more)
		{
			QMetaObject::invokeMethod(this, "respondError", Qt::QueuedConnection, Q_ARG(QByteArray, "bad-request"));
			return;
		}

		methodHasBody = (request.method == "POST" || request.method == "PUT");
		bodySent = false;

		// can't use an inbound stream for non-POST/PUT
		if(!methodHasBody && request.more)
		{
			QMetaObject::invokeMethod(this, "respondError", Qt::QueuedConnection, Q_ARG(QByteArray, "bad-request"));
			return;
		}

		// must provide content-length if planning on sending more packets
		if(request.more && !request.headers.contains("content-length"))
		{
			QMetaObject::invokeMethod(this, "respondError", Qt::QueuedConnection, Q_ARG(QByteArray, "bad-request"));
			return;
		}

		inSeq = request.seq;

		if(!isAllowed(request.url.host()) || (!request.connectHost.isEmpty() && !isAllowed(request.connectHost)))
		{
			QMetaObject::invokeMethod(this, "respondError", Qt::QueuedConnection, Q_ARG(QByteArray, "policy-violation"));
			return;
		}

		hreq = new HttpRequest(dns, this);
		connect(hreq, SIGNAL(nextAddress(const QHostAddress &)), SLOT(req_nextAddress(const QHostAddress &)));
		connect(hreq, SIGNAL(readyRead()), SLOT(req_readyRead()));
		connect(hreq, SIGNAL(bytesWritten(int)), SLOT(req_bytesWritten(int)));
		connect(hreq, SIGNAL(error()), SLOT(req_error()));
		maxResponseSize = request.maxSize;
		if(!request.connectHost.isEmpty())
			hreq->setConnectHost(request.connectHost);

		if(request.credits != -1)
			outCredits += request.credits;

		HttpHeaders headers = request.headers;
		// ensure content-length (or overwrite it, if not streaming input)
		if((request.method == "POST" || request.method == "PUT") && (!headers.contains("content-length") || !request.more))
			headers += HttpHeader("Content-Length", QByteArray::number(request.body.size()));

		timer = new QTimer(this);
		connect(timer, SIGNAL(timeout()), SLOT(timer_timeout()));
		timer->setSingleShot(true);
		timer->start(SESSION_TIMEOUT * 1000);

		hreq->start(request.method, request.url, headers);

		// note: unlike follow-up requests, the initial request is assumed to have a body.
		//   if no body field is present, we act as if it is present but empty.

		if(methodHasBody)
		{
			if(!request.body.isEmpty())
				hreq->writeBody(request.body);

			if(!request.more)
			{
				bodySent = true;
				hreq->endBody();
			}
			else
			{
				// send cts
				YurlResponsePacket resp;
				resp.credits = IDEAL_CREDITS;
				writeResponse(resp);
			}
		}
	}

	void write(const QVariant &vrequest)
	{
		YurlRequestPacket request;
		if(!request.fromVariant(vrequest))
		{
			QVariantHash vhash = vrequest.toHash();
			if(!vhash["cancel"].toBool())
			{
				QMetaObject::invokeMethod(this, "respondError", Qt::QueuedConnection, Q_ARG(QByteArray, "bad-request"));
			}
			else
			{
				cleanup();
				QMetaObject::invokeMethod(q, "finished", Qt::QueuedConnection);
			}

			return;
		}

		// cancel session if a wrong sequenced packet is received
		if(request.seq == -1 || request.seq != inSeq + 1)
		{
			if(!request.cancel)
			{
				QMetaObject::invokeMethod(this, "respondError", Qt::QueuedConnection, Q_ARG(QByteArray, "cancel"));
			}
			else
			{
				cleanup();
				QMetaObject::invokeMethod(q, "finished", Qt::QueuedConnection);
			}

			return;
		}

		refreshTimeout();

		inSeq = request.seq;

		// all we care about from follow-up writes are body and credits

		if(request.credits != -1)
			outCredits += request.credits;

		if(methodHasBody)
		{
			if(!request.body.isNull())
			{
				if(bodySent)
				{
					QMetaObject::invokeMethod(this, "respondError", Qt::QueuedConnection, Q_ARG(QByteArray, "bad-request"));
					return;
				}

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

		// if we needed credits to send something, take care of that now
		if(request.credits != -1 && stuffToRead)
			trySend();
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
		if(config->defaultPolicy == "allow")
			return !checkDeny(in) || checkAllow(in);
		else
			return checkAllow(in) && !checkDeny(in);
	}

	// emits signals, but safe to delete after
	void writeResponse(const YurlResponsePacket &resp)
	{
		YurlResponsePacket out = resp;
		out.id = rid;
		if(!out.isError)
			out.seq = outSeq++;
		out.replyAddress = config->clientId;
		out.userData = userData;

		// only log if error or body packet. this way we don't log cts or credits-only packets

		if(out.isError)
		{
			log_info("OUT ERR id=%s condition=%s", out.id.data(), out.condition.data());
		}
		else if(!out.body.isNull())
		{
			if(resp.code != -1)
				log_info("OUT id=%s code=%d %d%s", out.id.data(), out.code, out.body.size(), out.more ? " M" : "");
			else
				log_info("OUT id=%s %d%s", out.id.data(), out.body.size(), out.more ? " M" : "");
		}

		emit q->readyRead(receiver, out.toVariant());
	}

	// emits signals, but safe to delete after
	void trySend()
	{
		QPointer<QObject> self = this;

		stuffToRead = false;

		YurlResponsePacket resp;

		if(!sentHeader)
		{
			resp.code = hreq->responseCode();
			resp.status = hreq->responseStatus();
			resp.headers = hreq->responseHeaders();
			sentHeader = true;
		}

		// note: we always set body, even if empty

		if(outStream)
		{
			QByteArray buf = hreq->readResponseBody(outCredits);

			if(!buf.isEmpty())
			{
				if(maxResponseSize != -1 && bytesReceived + buf.size() > maxResponseSize)
				{
					respondError("max-size-exceeded");
					return;
				}
			}

			if(!buf.isNull())
				resp.body = buf;
			else
				resp.body = "";

			bytesReceived += resp.body.size();
			outCredits -= resp.body.size();

			resp.more = (hreq->bytesAvailable() > 0 || !hreq->isFinished());

			if(hreq->bytesAvailable() > 0)
				stuffToRead = true;
		}
		else
		{
			if(!inbuf.isNull())
			{
				resp.body = inbuf;
				inbuf.clear();
			}
			else
				resp.body = "";
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

	void refreshTimeout()
	{
		timer->start(SESSION_TIMEOUT * 1000);
	}

private slots:
	void respondError(const QByteArray &condition)
	{
		QPointer<QObject> self = this;

		YurlResponsePacket resp;
		resp.isError = true;
		resp.condition = condition;

		writeResponse(resp);
		if(!self)
			return;

		cleanup();
		emit q->finished();
	}

	void req_nextAddress(const QHostAddress &addr)
	{
		if(!isAllowed(addr.toString()))
			respondError("policy-violation");
	}

	void req_readyRead()
	{
		refreshTimeout();

		stuffToRead = true;

		if(outStream)
		{
			if(outCredits < 1)
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

		trySend();
	}

	void req_bytesWritten(int count)
	{
		YurlResponsePacket resp;
		resp.credits = count;
		writeResponse(resp);
	}

	void req_error()
	{
		QByteArray condition;
		switch(hreq->errorCondition())
		{
			case HttpRequest::ErrorPolicy:  condition = "policy-violation"; break;
			case HttpRequest::ErrorConnect: condition = "remote-connection-failed"; break;
			case HttpRequest::ErrorTls:     condition = "tls-error"; break;
			case HttpRequest::ErrorTimeout: condition = "connection-timeout"; break;
			case HttpRequest::ErrorGeneric:
			default:
				condition = "undefined-condition";
				break;
		}

		respondError(condition);
	}

	void timer_timeout()
	{
		respondError("connection-timeout");
	}
};

Worker::Worker(JDnsShared *dns, AppConfig *config, QObject *parent) :
	QObject(parent)
{
	d = new Private(dns, config, this);
}

Worker::~Worker()
{
	delete d;
}

QByteArray Worker::rid() const
{
	return d->rid;
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