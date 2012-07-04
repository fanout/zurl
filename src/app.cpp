#include "app.h"

#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#include <QHash>
#include <QSettings>
#include <QHostAddress>
#include <QtCrypto>
#include "jdnsshared.h"
#include "qzmqsocket.h"
#include "qzmqreprouter.h"
#include "qzmqreqmessage.h"
#include "request.h"
#include "requestpacket.h"
#include "responsepacket.h"
#include "tnetstring.h"

#define VERSION "1.0"

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
		int seq;
		bool streaming;
		bool sentHeader;

		RequestState() :
			reqSource(false),
			seq(0),
			streaming(false),
			sentHeader(false)
		{
		}
	};

	App *q;
	bool verbose;
	JDnsShared *dns;
	QZmq::RepRouter *in_req_sock;
	QZmq::Socket *in_sock;
	QZmq::Socket *out_sock;
	QString defaultPolicy;
	QStringList allowExps, denyExps;
	QHash<Request*, RequestState*> requestStateByRequest;

	Private(App *_q) :
		QObject(_q),
		q(_q),
		verbose(false)
	{
	}

	void log(int level, const char *fmt, va_list ap) const
	{
		if(level <= 1 || verbose)
		{
			QString str;
			str.vsprintf(fmt, ap);

			const char *lstr;
			switch(level)
			{
				case 0: lstr = "ERR"; break;
				case 1: lstr = "WARN"; break;
				case 2:
				default:
					lstr = "INFO"; break;
			}

			fprintf(stderr, "[%s] %s\n", lstr, qPrintable(str));
		}
	}

	void log_info(const char *fmt, ...) const
	{
		va_list ap;
		va_start(ap, fmt);
		log(2, fmt, ap);
		va_end(ap);
	}

	void log_warning(const char *fmt, ...) const
	{
		va_list ap;
		va_start(ap, fmt);
		log(1, fmt, ap);
		va_end(ap);
	}

	void log_error(const char *fmt, ...) const
	{
		va_list ap;
		va_start(ap, fmt);
		log(0, fmt, ap);
		va_end(ap);
	}

	void start()
	{
		if(!QCA::isSupported("cert"))
		{
			log_error("missing qca \"cert\" feature. install qca-ossl");
			emit q->quit();
			return;
		}

		QStringList args = QCoreApplication::instance()->arguments();
		args.removeFirst();

		// options
		QHash<QString, QString> options;
		for(int n = 0; n < args.count(); ++n)
		{
			if(args[n] == "--")
			{
				break;
			}
			else if(args[n].startsWith("--"))
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

		if(options.contains("version"))
		{
			printf("Yurl %s\n", VERSION);
			emit q->quit();
			return;
		}

		if(options.contains("verbose"))
			verbose = true;

		QString configFile = options["config"];
		if(configFile.isEmpty())
			configFile = "/etc/yurl.conf";

		// QSettings doesn't inform us if the config file doesn't exist, so do that ourselves
		{
			QFile file(configFile);
			if(!file.open(QIODevice::ReadOnly))
			{
				log_error("failed to open %s, and --config not passed", qPrintable(configFile));
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
			log_error("in_spec set but not out_spec");
			emit q->quit();
			return;
		}

		if(in_url.isEmpty() && !out_url.isEmpty())
		{
			log_error("out_spec set but not in_spec");
			emit q->quit();
			return;
		}

		if(in_url.isEmpty() && in_req_url.isEmpty())
		{
			log_error("must set at least in_spec+out_spec or in_req_spec");
			emit q->quit();
			return;
		}

		defaultPolicy = settings.value("defpolicy").toString();
		if(defaultPolicy != "allow" && defaultPolicy != "deny")
		{
			log_error("must set defpolicy to either \"allow\" or \"deny\"");
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

		log_info("started");
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

	// receiver non-empty on push interface, empty on req interface
	void setupRequest(const QZmq::ReqMessage &msg, const QByteArray &receiver, const QVariant &data)
	{
		RequestPacket p;
		if(!p.fromVariant(data))
		{
			log_warning("received message with invalid format (bad/missing fields), skipping");
			return;
		}

		RequestState *state = new RequestState;

		state->id = p.id;
		state->seq = 0;

		if(!receiver.isEmpty())
		{
			state->receiver = receiver;
			state->streaming = p.stream; // streaming only allowed on push interface
		}
		else
		{
			state->reqSource = true;
			state->reqHeaders = msg.headers();
		}

		log_info("IN id=%s, %s %s", p.id.data(), qPrintable(p.method), qPrintable(p.url.toString()));

		if(!isAllowed(p.url.host()))
		{
			ResponsePacket resp;
			resp.id = state->id;
			resp.seq = (state->seq)++;
			resp.isError = true;
			resp.condition = "policy-violation";
			writeResponse(state, resp);
			delete state;
			return;
		}

		Request *req = new Request(dns, this);
		connect(req, SIGNAL(nextAddress(const QHostAddress &)), SLOT(req_nextAddress(const QHostAddress &)));
		connect(req, SIGNAL(readyRead()), SLOT(req_readyRead()));
		connect(req, SIGNAL(error()), SLOT(req_error()));
		if(p.maxSize != -1)
			req->setMaximumResponseSize(p.maxSize);

		requestStateByRequest.insert(req, state);
		req->start(p.method, p.url, p.headers, p.body);
	}

	void writeResponse(RequestState *state, const ResponsePacket &p)
	{
		if(p.isError)
		{
			log_info("OUT ERR id=%s condition=%s", p.id.data(), p.condition.data());
		}
		else
		{
			if(p.code != -1)
				log_info("OUT id=%s code=%d %d%s", p.id.data(), p.code, p.body.size(), p.isLast ? " L" : "");
			else
				log_info("OUT id=%s %d%s", p.id.data(), p.body.size(), p.isLast ? " L" : "");
		}

		QByteArray msg = p.toByteArray();

		if(state->reqSource)
			in_req_sock->write(QZmq::ReqMessage(state->reqHeaders, QList<QByteArray>() << msg).toRawMessage());
		else
			out_sock->write(QList<QByteArray>() << (state->receiver + ' ' + msg));
	}

	void destroyReq(Request *req)
	{
		req->disconnect(this);
		req->setParent(0);
		req->stop();
		req->deleteLater();
	}

private slots:
	void in_req_readyRead()
	{
		QZmq::ReqMessage reqMessage(in_req_sock->read());
		if(reqMessage.content().count() != 1)
		{
			log_warning("received message with parts != 1, skipping");
			return;
		}

		bool ok;
		QVariant data = TnetString::toVariant(reqMessage.content()[0], 0, &ok);
		if(!ok)
		{
			log_warning("received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		setupRequest(reqMessage, QByteArray(), data);
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
			log_warning("received message with parts != 1, skipping");
			return;
		}

		int at = msg[0].indexOf(' ');
		if(at == -1)
		{
			log_warning("received message with invalid format (missing receiver), skipping");
			return;
		}

		QByteArray receiver = msg[0].mid(0, at);
		if(receiver.isEmpty())
		{
			log_warning("received message with invalid format (receiver empty), skipping");
			return;
		}

		bool ok;
		QVariant data = TnetString::toVariant(msg[0], at + 1, &ok);
		if(!ok)
		{
			log_warning("received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		setupRequest(QZmq::ReqMessage(), receiver, data);
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
			resp.seq = (state->seq)++;
			resp.isError = true;
			resp.condition = "policy-violation";
			writeResponse(state, resp);

			requestStateByRequest.remove(req);
			delete state;

			destroyReq(req);
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
		p.seq = (state->seq)++;
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

		writeResponse(state, p);

		if(req->isFinished())
		{
			requestStateByRequest.remove(req);
			delete state;

			destroyReq(req);
		}
	}

	void req_error()
	{
		Request *req = (Request *)sender();
		RequestState *state = requestStateByRequest[req];
		assert(state);

		ResponsePacket p;
		p.id = state->id;
		p.seq = (state->seq)++;
		p.isError = true;
		switch(req->errorCondition())
		{
			case Request::ErrorPolicy:          p.condition = "policy-violation"; break;
			case Request::ErrorConnect:         p.condition = "remote-connection-failed"; break;
			case Request::ErrorTls:             p.condition = "tls-error"; break;
			case Request::ErrorTimeout:         p.condition = "connection-timeout"; break;
			case Request::ErrorMaxSizeExceeded: p.condition = "max-size-exceeded"; break;
			case Request::ErrorGeneric:
			default:
				p.condition = "undefined-condition";
				break;
		}

		writeResponse(state, p);

		requestStateByRequest.remove(req);
		delete state;

		destroyReq(req);
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
