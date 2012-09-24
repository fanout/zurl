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
#include "qzmqreqmessage.h"
#include "qzmqvalve.h"
#include "tnetstring.h"
#include "appconfig.h"
#include "worker.h"

#define VERSION "1.0"

class App::Private : public QObject
{
	Q_OBJECT

public:
	App *q;
	bool verbose;
	QTime qtime;
	JDnsShared *dns;
	QZmq::Socket *in_sock;
	QZmq::Socket *in_stream_sock;
	QZmq::Socket *out_sock;
	QZmq::Socket *in_req_sock;
	QZmq::Valve *in_valve;
	QZmq::Valve *in_req_valve;
	AppConfig config;
	QSet<Worker*> workers;
	QHash<QByteArray, Worker*> streamWorkersById;
	QHash<Worker*, QList<QByteArray> > reqHeadersByWorker;

	Private(App *_q) :
		QObject(_q),
		q(_q),
		verbose(false),
		in_sock(0),
		in_stream_sock(0),
		out_sock(0),
		in_req_sock(0),
		in_valve(0),
		in_req_valve(0)
	{
		qtime.start();
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

			QTime t(0, 0);
			t = t.addMSecs(qtime.elapsed());
			fprintf(stderr, "[%s] %s %s\n", lstr, qPrintable(t.toString("HH:mm:ss.zzz")), qPrintable(str));
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
		QString in_stream_url = settings.value("in_stream_spec").toString();
		QString out_url = settings.value("out_spec").toString();
		QString in_req_url = settings.value("in_req_spec").toString();
		config.maxWorkers = settings.value("max_open_requests", -1).toInt();

		if((!in_url.isEmpty() || !in_stream_url.isEmpty() || !out_url.isEmpty()) && (in_url.isEmpty() || in_stream_url.isEmpty() || out_url.isEmpty()))
		{
			log_error("if any of in_spec, in_stream_spec, or out_spec are set then all of them must be set");
			emit q->quit();
			return;
		}

		if(in_url.isEmpty() && in_req_url.isEmpty())
		{
			log_error("must set at least in_spec+in_stream_spec+out_spec or in_req_spec");
			emit q->quit();
			return;
		}

		config.defaultPolicy = settings.value("defpolicy").toString();
		if(config.defaultPolicy != "allow" && config.defaultPolicy != "deny")
		{
			log_error("must set defpolicy to either \"allow\" or \"deny\"");
			emit q->quit();
			return;
		}

		config.allowExps = settings.value("allow").toStringList();
		config.denyExps = settings.value("deny").toStringList();

		dns = new JDnsShared(JDnsShared::UnicastInternet, this);
		dns->addInterface(QHostAddress::Any);
		dns->addInterface(QHostAddress::AnyIPv6);

		if(!in_url.isEmpty())
		{
			in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);
			if(!in_sock->bind(in_url))
			{
				log_error("unable to bind to in_spec: %s", qPrintable(in_url));
				emit q->quit();
				return;
			}

			in_valve = new QZmq::Valve(in_sock, this);
			connect(in_valve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(in_readyRead(const QList<QByteArray> &)));
		}

		if(!in_stream_url.isEmpty())
		{
			in_stream_sock = new QZmq::Socket(QZmq::Socket::Router, this);
			connect(in_stream_sock, SIGNAL(readyRead()), SLOT(in_stream_readyRead()));
			if(!in_stream_sock->bind(in_stream_url))
			{
				log_error("unable to bind to in_stream_spec: %s", qPrintable(in_stream_url));
				emit q->quit();
				return;
			}
		}

		if(!out_url.isEmpty())
		{
			out_sock = new QZmq::Socket(QZmq::Socket::Pub, this);
			if(!out_sock->bind(out_url))
			{
				log_error("unable to bind to out_spec: %s", qPrintable(out_url));
				emit q->quit();
				return;
			}
		}

		if(!in_req_url.isEmpty())
		{
			in_req_sock = new QZmq::Socket(QZmq::Socket::Router, this);
			if(!in_req_sock->bind(in_req_url))
			{
				log_error("unable to bind to in_req_spec: %s", qPrintable(in_req_url));
				emit q->quit();
				return;
			}

			in_req_valve = new QZmq::Valve(in_req_sock, this);
			connect(in_req_valve, SIGNAL(readyRead(const QList<QByteArray> &)), SLOT(in_req_readyRead(const QList<QByteArray> &)));
		}

		if(in_valve)
			in_valve->open();
		if(in_req_valve)
			in_req_valve->open();

		log_info("started");
	}

private slots:
	void in_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 1)
		{
			log_warning("received message with parts != 1, skipping");
			return;
		}

		int at = message[0].indexOf(' ');
		if(at == -1)
		{
			log_warning("received message with invalid format (missing receiver), skipping");
			return;
		}

		QByteArray receiver = message[0].mid(0, at);
		if(receiver.isEmpty())
		{
			log_warning("received message with invalid format (receiver empty), skipping");
			return;
		}

		bool ok;
		QVariant data = TnetString::toVariant(message[0], at + 1, &ok);
		if(!ok)
		{
			log_warning("received message with invalid format (tnetstring parse failed), skipping");
			return;
		}

		Worker *w = new Worker(dns, &config, this);
		connect(w, SIGNAL(readyRead(const QByteArray &, const QVariant &)), SLOT(worker_readyRead(const QByteArray &, const QVariant &)));
		connect(w, SIGNAL(finished()), SLOT(worker_finished()));

		workers += w;
		streamWorkersById[w->id()] = w;

		if(workers.count() >= config.maxWorkers)
		{
			in_valve->close();

			// also close in_req_valve, if we have it
			if(in_req_valve)
				in_req_valve->close();
		}

		w->start(receiver, data, Worker::Stream);
	}

	void in_stream_readyRead()
	{
		QZmq::ReqMessage reqMessage(in_stream_sock->read());
		if(reqMessage.headers().isEmpty())
		{
			log_warning("received message with no routing id, skipping");
			return;
		}

		Worker *w = streamWorkersById.value(reqMessage.headers()[0]);
		if(!w)
		{
			log_warning("received message with unknown routing id, skipping");
			return;
		}

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

		w->write(data);
	}

	void in_req_readyRead(const QList<QByteArray> &message)
	{
		QZmq::ReqMessage reqMessage(message);
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

		Worker *w = new Worker(dns, &config, this);
		connect(w, SIGNAL(readyRead(const QByteArray &, const QVariant &)), SLOT(worker_readyRead(const QByteArray &, const QVariant &)));
		connect(w, SIGNAL(finished()), SLOT(worker_finished()));

		workers += w;
		reqHeadersByWorker[w] = reqMessage.headers();

		if(workers.count() >= config.maxWorkers)
		{
			in_req_valve->close();

			// also close in_valve, if we have it
			if(in_valve)
				in_valve->close();
		}

		w->start(QByteArray(), data, Worker::Single);
	}

	void worker_readyRead(const QByteArray &receiver, const QVariant &response)
	{
		Worker *w = (Worker *)sender();

		QByteArray part = TnetString::fromVariant(response);
		if(!receiver.isEmpty())
		{
			out_sock->write(QList<QByteArray>() << (receiver + ' ' + part));
		}
		else
		{
			assert(reqHeadersByWorker.contains(w));
			out_sock->write(QZmq::ReqMessage(reqHeadersByWorker.value(w), QList<QByteArray>() << part).toRawMessage());
		}
	}

	void worker_finished()
	{
		Worker *w = (Worker *)sender();

		streamWorkersById.remove(w->id());
		reqHeadersByWorker.remove(w);
		workers.remove(w);

		delete w;

		// ensure the valves are open
		if(in_valve)
			in_valve->open();
		if(in_req_valve)
			in_req_valve->open();
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
