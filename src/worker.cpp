#include "worker.h"

#include <assert.h>
#include <QSet>
#include <QVariant>
#include <QUuid>
#include "jdnsshared.h"
#include "httprequest.h"
#include "yurlrequestpacket.h"
#include "yurlresponsepacket.h"
#include "appconfig.h"

#define IDEAL_CREDITS 200000

static QSet<QByteArray> g_ids;

class Worker::Private : public QObject
{
	Q_OBJECT

public:
	Worker *q;
	JDnsShared *dns;
	AppConfig *config;
	QByteArray id;
	QByteArray receiver;
	QByteArray rid;
	int inSeq, outSeq;
	int outCredits;
	bool outStream;
	QVariant userData;
	HttpRequest *hreq;
	bool sentHeader;

	Private(JDnsShared *_dns, AppConfig *_config, Worker *_q) :
		QObject(_q),
		q(_q),
		dns(_dns),
		config(_config)
	{
		do
		{
			id = QUuid::createUuid().toByteArray();
		} while(g_ids.contains(id));

		g_ids += id;
	}

	~Private()
	{
		g_ids.remove(id);
	}

	void start(const QByteArray &_receiver, const QVariant &vrequest, Mode mode)
	{
		YurlRequestPacket request;
		if(!request.fromVariant(vrequest))
		{
			// TODO: log error
			emit q->finished();
			return;
		}

		receiver = _receiver;
		rid = request.id;
		inSeq = 0;
		outSeq = 0;
		outCredits = 0;
		userData = request.userData;
		sentHeader = false;

		// streaming only allowed on streaming interface
		if(mode == Worker::Stream)
			outStream = request.stream;
		else
			outStream = false;

		//log_info("IN wc=%d id=%s, %s %s", workers, p.id.data(), qPrintable(p.method), qPrintable(p.url.toString()));

		if(!isAllowed(request.url.host()) || (!request.connectHost.isEmpty() && !isAllowed(request.connectHost)))
		{
			YurlResponsePacket resp;
			resp.id = rid;
			resp.isError = true;
			resp.condition = "policy-violation";
			resp.userData = userData;
			emit q->readyRead(receiver, resp.toVariant());
			emit q->finished();
			return;
		}

		hreq = new HttpRequest(dns, this);
		connect(hreq, SIGNAL(nextAddress(const QHostAddress &)), SLOT(req_nextAddress(const QHostAddress &)));
		connect(hreq, SIGNAL(readyRead()), SLOT(req_readyRead()));
		connect(hreq, SIGNAL(bytesWritten(int)), SLOT(req_bytesWritten(int)));
		connect(hreq, SIGNAL(error()), SLOT(req_error()));
		if(request.maxSize != -1)
			hreq->setMaximumResponseSize(request.maxSize);
		if(!request.connectHost.isEmpty())
			hreq->setConnectHost(request.connectHost);

		++inSeq;
		if(request.credits != -1)
			outCredits += request.credits;

		hreq->start(request.method, request.url, request.headers);

		if(!request.body.isEmpty())
			hreq->writeBody(request.body);

		if(request.more)
		{
			// send cts
			YurlResponsePacket resp;
			resp.id = rid;
			resp.seq = outSeq++;
			resp.replyAddress = id;
			resp.credits = IDEAL_CREDITS;
			emit q->readyRead(receiver, resp.toVariant());
		}
		else
		{
			hreq->endBody();
		}
	}

	void write(const QVariant &vrequest)
	{
		YurlRequestPacket request;
		if(!request.fromVariant(vrequest))
		{
			// TODO: log error
			return;
		}

		// all we care about from follow-up writes are body and credits

		if(request.credits != -1)
			outCredits += request.credits;

		if(!request.body.isEmpty())
		{
			hreq->writeBody(request.body);

			if(!request.more)
				hreq->endBody();
		}
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

private slots:
	void req_nextAddress(const QHostAddress &addr)
	{
		if(!isAllowed(addr.toString()))
		{
			// TODO: cleanup

			YurlResponsePacket resp;
			resp.id = rid;
			resp.isError = true;
			resp.condition = "policy-violation";
			resp.userData = userData;
			emit q->readyRead(receiver, resp.toVariant());
			emit q->finished();
		}
	}

	void req_readyRead()
	{
		if(!outStream && !hreq->isFinished())
			return;

		YurlResponsePacket resp;
		resp.id = rid;
		resp.seq = outSeq++;
		resp.more = !hreq->isFinished();

		assert(!receiver.isEmpty() || !resp.more);

		if(!sentHeader)
		{
			resp.code = hreq->responseCode();
			resp.status = hreq->responseStatus();
			resp.headers = hreq->responseHeaders();
			sentHeader = true;
		}

		QByteArray body = hreq->readResponseBody();
		if(!body.isEmpty() || !resp.more)
			resp.body = hreq->readResponseBody();

		resp.userData = userData;

		emit q->readyRead(receiver, resp.toVariant());

		if(hreq->isFinished())
		{
			// TODO: cleanup
			emit q->finished();
		}
	}

	void req_bytesWritten(int count)
	{
		YurlResponsePacket resp;
		resp.id = rid;
		resp.seq = outSeq++;
		resp.credits = count;
		emit q->readyRead(receiver, resp.toVariant());
	}

	void req_error()
	{
		YurlResponsePacket resp;
		resp.id = rid;
		resp.isError = true;
		switch(hreq->errorCondition())
		{
			case HttpRequest::ErrorPolicy:          resp.condition = "policy-violation"; break;
			case HttpRequest::ErrorConnect:         resp.condition = "remote-connection-failed"; break;
			case HttpRequest::ErrorTls:             resp.condition = "tls-error"; break;
			case HttpRequest::ErrorTimeout:         resp.condition = "connection-timeout"; break;
			case HttpRequest::ErrorMaxSizeExceeded: resp.condition = "max-size-exceeded"; break;
			case HttpRequest::ErrorGeneric:
			default:
				resp.condition = "undefined-condition";
				break;
		}

		resp.userData = userData;

		// TODO: cleanup
		emit q->readyRead(receiver, resp.toVariant());
		emit q->finished();
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

QByteArray Worker::id() const
{
	return d->id;
}

void Worker::start(const QByteArray &receiver, const QVariant &request, Mode mode)
{
	d->start(receiver, request, mode);
}

void Worker::write(const QVariant &request)
{
	d->write(request);
}

#if 0
	void logResponse(const YurlResponsePacket &p)
	{
		if(p.isError)
		{
			log_info("OUT ERR id=%s condition=%s", p.id.data(), p.condition.data());
		}
		else
		{
			if(p.code != -1)
				log_info("OUT id=%s code=%d %d%s", p.id.data(), p.code, p.body.size(), p.more ? " M" : "");
			else
				log_info("OUT id=%s %d%s", p.id.data(), p.body.size(), p.more ? " M" : "");
		}
	}
#endif

#include "worker.moc"
