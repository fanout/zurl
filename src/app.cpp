#include "app.h"

#include <stdio.h>
#include <assert.h>
#include <QHash>
#include <QSettings>
#include <QHostAddress>
#include "jdnsshared.h"
#include "qzmqsocket.h"
#include "qzmqreprouter.h"
#include "qzmqreqmessage.h"
#include "request.h"
#include "requestpacket.h"
#include "responsepacket.h"
#include "tnetstring.h"

class App::Private : public QObject
{
	Q_OBJECT

public:
	class RequestState
	{
	public:
		QByteArray receiver;
		bool reqSource;
		QList<QByteArray> reqHeaders; // if reqSource is true
		QByteArray id;
		bool streaming;
		bool sentHeader;

		RequestState() :
			reqSource(false),
			streaming(false),
			sentHeader(false)
		{
		}
	};

	App *q;
	JDnsShared *dns;
	QZmq::RepRouter *in_req_sock;
	QZmq::Socket *in_sock;
	QZmq::Socket *out_sock;
	QString defaultPolicy;
	QStringList allowExps, denyExps;
	QHash<Request*, RequestState*> requestStateByRequest;

	Private(App *_q) :
		QObject(_q),
		q(_q)
	{
	}

	void start()
	{
		QStringList args = QCoreApplication::instance()->arguments();
		args.removeFirst();

		// options
		QHash<QString, QString> options;
		for(int n = 0; n < args.count(); ++n)
		{
			if(args[n].startsWith("--"))
			{
				QString opt = args[n].mid(2);
				QString var, val;

				int at = opt.indexOf("=");
				if(at != -1)
				{
					var = opt.mid(0, at);
					val = opt.mid(at + 1);
				}
				else
					var = opt;

				options[var] = val;

				args.removeAt(n);
				--n; // adjust position
			}
		}

		QString configFile = options["config"];
		if(configFile.isEmpty())
			configFile = "/etc/yurl.conf";

		// QSettings doesn't inform us if the config file doesn't exist, so do that ourselves
		{
			QFile file(configFile);
			if(!file.open(QIODevice::ReadOnly))
			{
				fprintf(stderr, "error: failed to open %s\n", qPrintable(configFile));
				emit q->quit();
				return;
			}
		}

		QSettings settings(configFile, QSettings::IniFormat);

		QString in_url = settings.value("in_spec").toString();
		QString out_url = settings.value("out_spec").toString();
		QString in_req_url = settings.value("in_req_spec").toString();

		if(!in_url.isEmpty() && out_url.isEmpty())
		{
			fprintf(stderr, "error: in_spec set but not out_spec\n");
			emit q->quit();
			return;
		}

		if(in_url.isEmpty() && !out_url.isEmpty())
		{
			fprintf(stderr, "error: out_spec set but not in_spec\n");
			emit q->quit();
			return;
		}

		if(in_url.isEmpty() && in_req_url.isEmpty())
		{
			fprintf(stderr, "error: must set at least in_spec+out_spec or in_req_spec\n");
			emit q->quit();
			return;
		}

		defaultPolicy = settings.value("defpolicy").toString();
		if(defaultPolicy != "allow" && defaultPolicy != "deny")
		{
			fprintf(stderr, "error: must set defpolicy to either \"allow\" or \"deny\"\n");
			emit q->quit();
			return;
		}

		allowExps = settings.value("allow").toStringList();
		denyExps = settings.value("deny").toStringList();

		dns = new JDnsShared(JDnsShared::UnicastInternet, this);
		dns->addInterface(QHostAddress::Any);
		dns->addInterface(QHostAddress::AnyIPv6);

		in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);
		connect(in_sock, SIGNAL(readyRead()), SLOT(in_readyRead()));
		in_sock->bind(in_url);

		out_sock = new QZmq::Socket(QZmq::Socket::Pub, this);
		connect(out_sock, SIGNAL(messagesWritten(int)), SLOT(out_messagesWritten(int)));
		out_sock->bind(out_url);

		in_req_sock = new QZmq::RepRouter(this);
		connect(in_req_sock, SIGNAL(readyRead()), SLOT(in_req_readyRead()));
		connect(in_req_sock, SIGNAL(messagesWritten(int)), SLOT(in_req_messagesWritten(int)));
		in_req_sock->bind(in_req_url);

		fprintf(stderr, "yurl: initialized and ready to serve\n");
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
		foreach(const QString &exp, allowExps)
		{
			if(matchExp(exp, in))
				return true;
		}

		return false;
	}

	bool checkDeny(const QString &in) const
	{
		foreach(const QString &exp, denyExps)
		{
			if(matchExp(exp, in))
				return true;
		}

		return false;
	}

	bool isAllowed(const QString &in) const
	{
		if(defaultPolicy == "allow")
			return !checkDeny(in) || checkAllow(in);
		else
			return checkAllow(in) && !checkDeny(in);
	}

	void setupRequest(const RequestPacket &p, RequestState *state)
	{
		printf("request: id=[%s], method=[%s], url=[%s]\n", p.id.data(), qPrintable(p.method), qPrintable(p.url.toString()));

		if(!isAllowed(p.url.host()))
		{
			ResponsePacket resp;
			resp.id = p.id;
			resp.isError = true;
			resp.condition = "policy-violation";
			state->id = p.id;
			writeResponse(resp, state);
			delete state;
			return;
		}

		Request *req = new Request(dns, this);
		connect(req, SIGNAL(nextAddress(const QHostAddress &)), SLOT(req_nextAddress(const QHostAddress &)));
		connect(req, SIGNAL(readyRead()), SLOT(req_readyRead()));
		connect(req, SIGNAL(error()), SLOT(req_error()));
		if(p.maxResponseSize != -1)
			req->setMaximumResponseSize(p.maxResponseSize);

		state->id = p.id;
		requestStateByRequest.insert(req, state);

		req->start(p.method, p.url, p.headers, p.body);
	}

	void writeResponse(const ResponsePacket &p, RequestState *state)
	{
		QByteArray msg = p.toByteArray();

		if(state->reqSource)
			in_req_sock->write(QZmq::ReqMessage(state->reqHeaders, QList<QByteArray>() << msg).toRawMessage());
		else
			out_sock->write(QList<QByteArray>() << (state->receiver + ' ' + msg));
	}

private slots:
	void in_req_readyRead()
	{
		QZmq::ReqMessage reqMessage(in_req_sock->read());
		if(reqMessage.content().count() != 1)
		{
			fprintf(stderr, "warning: received message with bad format (0), dropping\n");
			return;
		}

		bool ok;
		QVariant data = TnetString::toVariant(reqMessage.content()[0], 0, &ok);
		if(!ok)
		{
			fprintf(stderr, "warning: received message with bad format (3), dropping\n");
			return;
		}

		fprintf(stderr, "IN REQ: %s\n", qPrintable(TnetString::variantToString(data, -1)));

		RequestPacket p;
		if(!p.fromVariant(data))
		{
			fprintf(stderr, "bad format\n");
			return;
		}

		RequestState *state = new RequestState;
		state->reqSource = true;
		state->reqHeaders = reqMessage.headers();
		setupRequest(p, state);
	}

	void in_req_messagesWritten(int count)
	{
		// TODO
		Q_UNUSED(count);
	}

	void in_readyRead()
	{
		QList<QByteArray> msg = in_sock->read();
		if(msg.count() != 1)
		{
			fprintf(stderr, "warning: received message with bad format (0), dropping\n");
			return;
		}

		int at = msg[0].indexOf(' ');
		if(at == -1)
		{
			fprintf(stderr, "warning: received message with bad format (1), dropping\n");
			return;
		}

		QByteArray receiver = msg[0].mid(0, at);
		if(receiver.isEmpty())
		{
			fprintf(stderr, "warning: received message with bad format (2), dropping\n");
			return;
		}

		bool ok;
		QVariant data = TnetString::toVariant(msg[0], at + 1, &ok);
		if(!ok)
		{
			fprintf(stderr, "warning: received message with bad format (3), dropping\n");
			return;
		}

		fprintf(stderr, "IN: %s %s\n", receiver.data(), qPrintable(TnetString::variantToString(data, -1)));

		RequestPacket p;
		if(!p.fromVariant(data))
		{
			fprintf(stderr, "bad format\n");
			return;
		}

		RequestState *state = new RequestState;
		state->receiver = receiver;
		state->streaming = p.stream;
		setupRequest(p, state);
	}

	void out_messagesWritten(int count)
	{
		// TODO
		Q_UNUSED(count);
	}

	void req_nextAddress(const QHostAddress &addr)
	{
		Request *req = (Request *)sender();
		RequestState *state = requestStateByRequest[req];
		assert(state);

		if(!isAllowed(addr.toString()))
		{
			ResponsePacket resp;
			resp.id = state->id;
			resp.isError = true;
			resp.condition = "policy-violation";
			writeResponse(resp, state);

			requestStateByRequest.remove(req);
			delete state;
			req->disconnect(this);
			req->deleteLater();
		}
	}

	void req_readyRead()
	{
		Request *req = (Request *)sender();
		RequestState *state = requestStateByRequest[req];
		assert(state);

		if(!state->streaming && !req->isFinished())
			return;

		ResponsePacket p;

		p.id = state->id;
		p.isLast = req->isFinished();

		if(state->reqSource)
			assert(p.isLast);

		if(!state->sentHeader)
		{
			p.code = req->responseCode();
			p.status = req->responseStatus();
			p.headers = req->responseHeaders();
			state->sentHeader = true;
		}

		p.body = req->readResponseBody();

		writeResponse(p, state);

		requestStateByRequest.remove(req);
		delete state;
		req->disconnect(this);
		req->deleteLater();
	}

	void req_error()
	{
		Request *req = (Request *)sender();
		RequestState *state = requestStateByRequest[req];
		assert(state);

		ResponsePacket p;
		p.id = state->id;
		p.isError = true;
		switch(req->errorCondition())
		{
			case Request::ErrorPolicy:      p.condition = "policy-violation"; break;
			case Request::ErrorConnect:     p.condition = "remote-connection-failed"; break;
			case Request::ErrorTimeout:     p.condition = "connection-timeout"; break;
			case Request::ErrorGeneric:
			default:
				p.condition = "undefined-condition";
				break;
		}

		writeResponse(p, state);

		requestStateByRequest.remove(req);
		delete state;
		req->disconnect(this);
		req->deleteLater();
	}
};

App::App(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

App::~App()
{
	delete d;
}

void App::start()
{
	d->start();
}

#include "app.moc"
