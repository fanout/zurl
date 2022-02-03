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

#include "app.h"

#include <stdio.h>
#include <assert.h>
#include <QHash>
#include <QUuid>
#include <QSettings>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include "qzmqsocket.h"
#include "qzmqreqmessage.h"
#include "qzmqvalve.h"
#include "processquit.h"
#include "tnetstring.h"
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "httprequest.h"
#include "appconfig.h"
#include "log.h"
#include "worker.h"

#define VERSION "1.11.1"

static void cleanStringList(QStringList *in)
{
	for(int n = 0; n < in->count(); ++n)
	{
		if(in->at(n).isEmpty())
		{
			in->removeAt(n);
			--n; // adjust position
		}
	}
}

// return true if item modified
static bool convertToJsonStyleInPlace(QVariant *in)
{
	// Hash -> Map
	// ByteArray (UTF-8) -> String

	bool changed = false;

	int type = in->type();
	if(type == QVariant::Hash)
	{
		QVariantMap vmap;
		QVariantHash vhash = in->toHash();
		QHashIterator<QString, QVariant> it(vhash);
		while(it.hasNext())
		{
			it.next();
			QVariant i = it.value();
			convertToJsonStyleInPlace(&i);
			vmap[it.key()] = i;
		}

		*in = vmap;
		changed = true;
	}
	else if(type == QVariant::List)
	{
		QVariantList vlist = in->toList();
		for(int n = 0; n < vlist.count(); ++n)
		{
			QVariant i = vlist.at(n);
			convertToJsonStyleInPlace(&i);
			vlist[n] = i;
		}

		*in = vlist;
		changed = true;
	}
	else if(type == QVariant::ByteArray)
	{
		*in = QVariant(QString::fromUtf8(in->toByteArray()));
		changed = true;
	}

	return changed;
}

// return true if item modified
static bool convertFromJsonStyleInPlace(QVariant *in)
{
	// Map -> Hash
	// String -> ByteArray (UTF-8)

	bool changed = false;

	int type = in->type();
	if(type == QVariant::Map)
	{
		QVariantHash vhash;
		QVariantMap vmap = in->toMap();
		QMapIterator<QString, QVariant> it(vmap);
		while(it.hasNext())
		{
			it.next();
			QVariant i = it.value();
			convertFromJsonStyleInPlace(&i);
			vhash[it.key()] = i;
		}

		*in = vhash;
		changed = true;
	}
	else if(type == QVariant::List)
	{
		QVariantList vlist = in->toList();
		for(int n = 0; n < vlist.count(); ++n)
		{
			QVariant i = vlist.at(n);
			convertFromJsonStyleInPlace(&i);
			vlist[n] = i;
		}

		*in = vlist;
		changed = true;
	}
	else if(type == QVariant::String)
	{
		*in = QVariant(in->toString().toUtf8());
		changed = true;
	}
	else if(type != QVariant::Bool && type != QVariant::Double && in->canConvert(QVariant::Int))
	{
		*in = in->toInt();
		changed = true;
	}

	return changed;
}

static QVariant convertToJsonStyle(const QVariant &in)
{
	QVariant v = in;
	convertToJsonStyleInPlace(&v);
	return v;
}

static QVariant convertFromJsonStyle(const QVariant &in)
{
	QVariant v = in;
	convertFromJsonStyleInPlace(&v);
	return v;
}

class App::Private : public QObject
{
	Q_OBJECT

public:
	enum InputType
	{
		InInit,
		InStream,
		InReq
	};

	App *q;
	QZmq::Socket *in_sock;
	QZmq::Socket *in_stream_sock;
	QZmq::Socket *out_sock;
	QZmq::Socket *in_req_sock;
	QZmq::Valve *in_valve;
	QZmq::Valve *in_req_valve;
	AppConfig config;
	QSet<Worker*> workers;
	QHash<QByteArray, Worker*> streamWorkersByRid;
	QHash<Worker*, QList<QByteArray> > reqHeadersByWorker;

	Private(App *_q) :
		QObject(_q),
		q(_q),
		in_sock(0),
		in_stream_sock(0),
		out_sock(0),
		in_req_sock(0),
		in_valve(0),
		in_req_valve(0)
	{
		connect(ProcessQuit::instance(), &ProcessQuit::quit, this, &Private::doQuit);
		connect(ProcessQuit::instance(), &ProcessQuit::hup, this, &Private::reload);
	}

	void start()
	{
		qsrand(time(NULL));

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
			printf("Zurl %s\n", VERSION);
			emit q->quit();
			return;
		}

		int logLevel = LOG_LEVEL_INFO;
		QString logLevelString = options.value("loglevel");
		if(!logLevelString.isEmpty())
		{
			bool ok;
			logLevel = logLevelString.toInt(&ok);
			if(!ok || logLevel < LOG_LEVEL_ERROR)
			{
				log_error("parameter to --loglevel invalid: %s", qPrintable(logLevelString));
				emit q->quit();
				return;
			}
		}

		if(options.contains("verbose"))
			logLevel = LOG_LEVEL_DEBUG;

		log_setOutputLevel(logLevel);

		QString logFile = options.value("logfile");
		if(!logFile.isEmpty())
		{
			if(!log_setFile(logFile))
			{
				log_error("failed to open log file: %s", qPrintable(logFile));
				emit q->quit();
				return;
			}
		}

		log_info("starting...");

		if(options.contains("config") && options.value("config").isEmpty())
		{
			log_error("parameter to --config missing");
			emit q->quit();
			return;
		}

		QString configFile = options.value("config");
		if(configFile.isEmpty())
			configFile = "/etc/zurl.conf";

		// QSettings doesn't inform us if the config file doesn't exist, so do that ourselves
		{
			QFile file(configFile);
			if(!file.open(QIODevice::ReadOnly))
			{
				if(options.contains("config"))
					log_error("failed to open %s", qPrintable(configFile));
				else
					log_error("failed to open %s, and --config not passed", qPrintable(configFile));
				emit q->quit();
				return;
			}
		}

		QSettings settings(configFile, QSettings::IniFormat);

		config.clientId = settings.value("instance_id").toString().toUtf8();
		if(config.clientId.isEmpty())
			config.clientId = QUuid::createUuid().toString().toLatin1();

		QString in_spec = settings.value("in_spec").toString();
		QString in_stream_spec = settings.value("in_stream_spec").toString();
		QString out_spec = settings.value("out_spec").toString();
		QString in_req_spec = settings.value("in_req_spec").toString();
		QString ipcFileModeStr = settings.value("ipc_file_mode").toString();
		config.allowIPv6 = settings.value("allow_ipv6", false).toBool();
		config.maxWorkers = settings.value("max_open_requests", -1).toInt();
		config.sessionBufferSize = settings.value("buffer_size", 200000).toInt();
		config.activityTimeout = settings.value("timeout", 600).toInt();
		config.persistentConnectionMaxTime = settings.value("connection_max_time", 60 * 60 * 2).toInt();
		int inHwm = settings.value("in_hwm", 1000).toInt();
		int outHwm = settings.value("out_hwm", 1000).toInt();

		if((!in_spec.isEmpty() || !in_stream_spec.isEmpty() || !out_spec.isEmpty()) && (in_spec.isEmpty() || in_stream_spec.isEmpty() || out_spec.isEmpty()))
		{
			log_error("if any of in_spec, in_stream_spec, or out_spec are set then all of them must be set");
			emit q->quit();
			return;
		}

		if(in_spec.isEmpty() && in_req_spec.isEmpty())
		{
			log_error("must set at least in_spec+in_stream_spec+out_spec or in_req_spec");
			emit q->quit();
			return;
		}

		int ipcFileMode = -1;
		if(!ipcFileModeStr.isEmpty())
		{
			// parse mode as octal
			bool ok;
			ipcFileMode = ipcFileModeStr.toInt(&ok, 8);
			if(!ok)
			{
				log_error("invalid ipc_file_mode: %s", qPrintable(ipcFileModeStr));
				emit q->quit();
				return;
			}
		}

		if(settings.contains("defpolicy"))
		{
			QString defaultPolicy = settings.value("defpolicy").toString();
			if(defaultPolicy != "allow" && defaultPolicy != "deny")
			{
				log_error("defpolicy must be set to \"allow\" or \"deny\"");
				emit q->quit();
				return;
			}

			config.defaultPolicy = defaultPolicy;
		}
		else
		{
			config.defaultPolicy = "allow";
		}

		config.allowExps = settings.value("allow").toStringList();
		config.denyExps = settings.value("deny").toStringList();

		cleanStringList(&config.allowExps);
		cleanStringList(&config.denyExps);

		HttpRequest::setPersistentConnectionMaxTime(config.persistentConnectionMaxTime);

		if(!in_spec.isEmpty())
		{
			in_sock = new QZmq::Socket(QZmq::Socket::Pull, this);

			in_sock->setHwm(inHwm);

			if(!bindSpec(in_sock, "in_spec", in_spec, ipcFileMode))
				return;

			in_valve = new QZmq::Valve(in_sock, this);
			connect(in_valve, &QZmq::Valve::readyRead, this, &Private::in_readyRead);
		}

		if(!in_stream_spec.isEmpty())
		{
			in_stream_sock = new QZmq::Socket(QZmq::Socket::Dealer, this);

			in_stream_sock->setIdentity(config.clientId);
			in_stream_sock->setHwm(inHwm);

			connect(in_stream_sock, &QZmq::Socket::readyRead, this, &Private::in_stream_readyRead);

			if(!bindSpec(in_stream_sock, "in_stream_spec", in_stream_spec, ipcFileMode))
				return;
		}

		if(!out_spec.isEmpty())
		{
			out_sock = new QZmq::Socket(QZmq::Socket::Pub, this);

			out_sock->setWriteQueueEnabled(false);
			out_sock->setHwm(outHwm);
			out_sock->setShutdownWaitTime(0);

			if(!bindSpec(out_sock, "out_spec", out_spec, ipcFileMode))
				return;
		}

		if(!in_req_spec.isEmpty())
		{
			in_req_sock = new QZmq::Socket(QZmq::Socket::Router, this);

			in_req_sock->setHwm(inHwm);

			if(!bindSpec(in_req_sock, "in_req_spec", in_req_spec, ipcFileMode))
				return;

			in_req_valve = new QZmq::Valve(in_req_sock, this);
			connect(in_req_valve, &QZmq::Valve::readyRead, this, &Private::in_req_readyRead);
		}

		if(in_valve)
			in_valve->open();
		if(in_req_valve)
			in_req_valve->open();

		log_info("started");
	}

	bool bindSpec(QZmq::Socket *sock, const QString &specName, const QString &specValue, int ipcFileMode)
	{
		if(!sock->bind(specValue))
		{
			log_error("unable to bind to %s: %s", qPrintable(specName), qPrintable(specValue));
			emit q->quit();
			return false;
		}

		if(specValue.startsWith("ipc://") && ipcFileMode != -1)
		{
			QFile::Permissions perms;
			if(ipcFileMode & 0400)
				perms |= QFile::ReadUser;
			if(ipcFileMode & 0200)
				perms |= QFile::WriteUser;
			if(ipcFileMode & 0100)
				perms |= QFile::ExeUser;
			if(ipcFileMode & 0040)
				perms |= QFile::ReadGroup;
			if(ipcFileMode & 0020)
				perms |= QFile::WriteGroup;
			if(ipcFileMode & 0010)
				perms |= QFile::ExeGroup;
			if(ipcFileMode & 0004)
				perms |= QFile::ReadOther;
			if(ipcFileMode & 0002)
				perms |= QFile::WriteOther;
			if(ipcFileMode & 0001)
				perms |= QFile::ExeOther;
			QFile::setPermissions(specValue.mid(6), perms);
		}

		return true;
	}

	void handleIncoming(InputType type, const QByteArray &message, const QList<QByteArray> &reqHeaders = QList<QByteArray>())
	{
		if(message.length() < 1)
		{
			log_warning("received message with invalid format (empty), skipping");
			return;
		}

		Worker::Format format;
		if(message[0] == 'T')
		{
			format = Worker::TnetStringFormat;
		}
		else if(message[0] == 'J')
		{
			format = Worker::JsonFormat;
		}
		else
		{
			log_warning("received message with invalid format (unsupported type), skipping");
			return;
		}

		QVariant data;
		if(format == Worker::TnetStringFormat)
		{
			bool ok;
			data = TnetString::toVariant(message, 1, &ok);
			if(!ok)
			{
				log_warning("received message with invalid format (tnetstring parse failed), skipping");
				return;
			}
		}
		else // JsonFormat
		{
			QJsonParseError e;
			QJsonDocument doc = QJsonDocument::fromJson(message.mid(1), &e);
			if(e.error != QJsonParseError::NoError)
			{
				log_warning("received message with invalid format (json parse failed), skipping");
				return;
			}

			if(doc.isObject())
				data = doc.object().toVariantMap();
			else if(doc.isArray())
				data = doc.array().toVariantList();

			data = convertFromJsonStyle(data);
		}

		if(log_outputLevel() >= LOG_LEVEL_DEBUG)
		{
			if(type == InInit)
				log_debug("recv-init: %s", qPrintable(TnetString::variantToString(data, -1)));
			else if(type == InStream)
				log_debug("recv-stream: %s", qPrintable(TnetString::variantToString(data, -1)));
			else // InReq
				log_debug("recv-req: %s", qPrintable(TnetString::variantToString(data, -1)));
		}

		ZhttpRequestPacket p;
		if(!p.fromVariant(data))
		{
			log_warning("received message with invalid format (parse failed), skipping");

			if((p.type != ZhttpRequestPacket::Error && p.type != ZhttpRequestPacket::Cancel) && !p.from.isEmpty() && !p.ids.isEmpty())
			{
				respondError(p.from, p.ids.first().id, "bad-request");
			}

			return;
		}

		if(type == InStream)
		{
			if(p.ids.isEmpty())
			{
				log_warning("received stream message without request id, skipping");
				return;
			}

			foreach(const ZhttpRequestPacket::Id &id, p.ids)
			{
				Worker *w = streamWorkersByRid.value(id.id);
				if(!w)
				{
					if((p.type != ZhttpRequestPacket::Error && p.type != ZhttpRequestPacket::Cancel) && !p.from.isEmpty() && !p.ids.isEmpty())
					{
						respondCancel(p.from, id.id);
					}

					continue;
				}

				w->write(id.seq, p);
			}

			return;
		}

		if(p.ids.count() > 1)
		{
			log_warning("received initial message with multiple ids, skipping");
			return;
		}

		if(!p.ids.isEmpty() && streamWorkersByRid.contains(p.ids.first().id))
		{
			log_warning("received request for id already in use, skipping");
			return;
		}

		QByteArray rid;
		int seq = -1;
		if(!p.ids.isEmpty())
		{
			rid = p.ids.first().id;
			seq = p.ids.first().seq;
		}

		Worker *w = new Worker(&config, format, this);
		connect(w, &Worker::readyRead, this, &Private::worker_readyRead);
		connect(w, &Worker::finished, this, &Private::worker_finished);

		workers += w;

		if(type == InInit && !rid.isEmpty())
			streamWorkersByRid[rid] = w;
		else if(type == InReq)
			reqHeadersByWorker[w] = reqHeaders;

		if(config.maxWorkers != -1 && workers.count() >= config.maxWorkers)
		{
			if(in_valve)
				in_valve->close();

			if(in_req_valve)
				in_req_valve->close();
		}

		w->start(rid, seq, p, (type == InInit ? Worker::Stream : Worker::Single));
	}

	// normally responses are handled by Workers, but in some routing
	//   cases we need to be able to respond with an error at this layer

	void respondCancel(const QByteArray &receiver, const QByteArray &rid)
	{
		ZhttpResponsePacket out;
		out.ids += ZhttpResponsePacket::Id(rid);
		out.type = ZhttpResponsePacket::Cancel;
		QByteArray part = QByteArray("T") + TnetString::fromVariant(out.toVariant());
		out_sock->write(QList<QByteArray>() << (receiver + ' ' + part));
	}

	void respondError(const QByteArray &receiver, const QByteArray &rid, const QByteArray &condition)
	{
		ZhttpResponsePacket out;
		out.ids += ZhttpResponsePacket::Id(rid);
		out.type = ZhttpResponsePacket::Error;
		out.condition = condition;
		QByteArray part = QByteArray("T") + TnetString::fromVariant(out.toVariant());
		out_sock->write(QList<QByteArray>() << (receiver + ' ' + part));
	}

private slots:
	void in_readyRead(const QList<QByteArray> &message)
	{
		if(message.count() != 1)
		{
			log_warning("received message with parts != 1, skipping");
			return;
		}

		handleIncoming(InInit, message[0]);
	}

	void in_stream_readyRead()
	{
		// message from DEALER socket will have two parts, with first part empty
		QList<QByteArray> message = in_stream_sock->read();
		if(message.count() != 2)
		{
			log_warning("received message with parts != 2, skipping");
			return;
		}

		if(!message[0].isEmpty())
		{
			log_warning("received message with non-empty first part, skipping");
			return;
		}

		handleIncoming(InStream, message[1]);
	}

	void in_req_readyRead(const QList<QByteArray> &message)
	{
		QZmq::ReqMessage reqMessage(message);
		if(reqMessage.content().count() != 1)
		{
			log_warning("received message with parts != 1, skipping");
			return;
		}

		handleIncoming(InReq, reqMessage.content()[0], reqMessage.headers());
	}

	void worker_readyRead(const QByteArray &receiver, const ZhttpResponsePacket &response)
	{
		Worker *w = (Worker *)sender();

		QVariant vresponse = response.toVariant();

		QByteArray part;
		Worker::Format format = w->format();
		if(format == Worker::TnetStringFormat)
		{
			part = QByteArray("T") + TnetString::fromVariant(vresponse);
		}
		else // JsonFormat
		{
			QVariant data = convertToJsonStyle(vresponse);
			QJsonDocument doc;
			if(data.type() == QVariant::Map)
				doc = QJsonDocument(QJsonObject::fromVariantMap(data.toMap()));
			else if(data.type() == QVariant::List)
				doc = QJsonDocument(QJsonArray::fromVariantList(data.toList()));
			part = QByteArray("J") + doc.toJson(QJsonDocument::Compact);
		}

		if(!receiver.isEmpty())
		{
			if(log_outputLevel() >= LOG_LEVEL_DEBUG)
				log_debug("send: %s", qPrintable(TnetString::variantToString(vresponse, -1)));

			out_sock->write(QList<QByteArray>() << (receiver + ' ' + part));
		}
		else
		{
			if(log_outputLevel() >= LOG_LEVEL_DEBUG)
				log_debug("send-req: %s", qPrintable(TnetString::variantToString(vresponse, -1)));

			assert(reqHeadersByWorker.contains(w));
			QList<QByteArray> reqHeaders = reqHeadersByWorker.value(w);
			in_req_sock->write(QZmq::ReqMessage(reqHeaders, QList<QByteArray>() << part).toRawMessage());
		}
	}

	void worker_finished()
	{
		Worker *w = (Worker *)sender();

		if(!w->rid().isEmpty())
			streamWorkersByRid.remove(w->rid());
		reqHeadersByWorker.remove(w);
		workers.remove(w);

		delete w;

		// ensure the valves are open
		if(in_valve)
			in_valve->open();
		if(in_req_valve)
			in_req_valve->open();
	}

	void reload()
	{
		log_info("reloading");
		log_rotate();
	}

	void doQuit()
	{
		log_info("stopping...");

		// remove the handler, so if we get another signal then we crash out
		ProcessQuit::cleanup();

		log_info("stopped");
		emit q->quit();
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
