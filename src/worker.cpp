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

#include "worker.h"

#include <assert.h>
#include <QVariant>
#include <QTimer>
#include <QPointer>
#include "jdnsshared.h"
#include "httprequest.h"
#include "zhttprequestpacket.h"
#include "zhttpresponsepacket.h"
#include "appconfig.h"
#include "log.h"

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
	bool ignorePolicies;
	HttpRequest *hreq;
	bool quiet;
	bool sentHeader;
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
		outSeq = 0;

		ZhttpRequestPacket request;
		if(!request.fromVariant(vrequest))
		{
			log_warning("failed to parse zurl request");

			QVariantHash vhash = vrequest.toHash();
			rid = vhash.value("id").toByteArray();
			assert(!rid.isEmpty()); // app layer ensures this
			receiver = vhash.value("from").toByteArray();
			bool cancel = (vhash.value("type").toByteArray() == "cancel");
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
		receiver = request.from;
		outCredits = 0;
		userData = request.userData;
		quiet = false;
		sentHeader = false;
		stuffToRead = false;
		bytesReceived = 0;

		ignorePolicies = request.ignorePolicies;

		// streaming only allowed on streaming interface
		if(mode == Worker::Stream)
			outStream = request.stream;
		else
			outStream = false;

		// some required fields
		if(request.method.isEmpty() || request.uri.isEmpty())
		{
			log_warning("missing request method or missing uri");

			QMetaObject::invokeMethod(this, "respondError", Qt::QueuedConnection, Q_ARG(QByteArray, "bad-request"));
			return;
		}

		log_info("IN id=%s, %s %s", request.id.data(), qPrintable(request.method), request.uri.toEncoded().data());

		// inbound streaming must start with sequence number of 0
		if(mode == Worker::Stream && request.more && request.seq != 0)
		{
			log_warning("streamed input must start with seq 0");

			QMetaObject::invokeMethod(this, "respondError", Qt::QueuedConnection, Q_ARG(QByteArray, "bad-request"));
			return;
		}

		// fire and forget
		if(mode == Worker::Stream && receiver.isEmpty())
			quiet = true;

		// can't use these two together
		if(mode == Worker::Single && request.more)
		{
			log_warning("cannot use streamed input on router interface");

			QMetaObject::invokeMethod(this, "respondError", Qt::QueuedConnection, Q_ARG(QByteArray, "bad-request"));
			return;
		}

		bodySent = false;

		inSeq = request.seq;

		if(!isAllowed(request.uri.host()) || (!request.connectHost.isEmpty() && !isAllowed(request.connectHost)))
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

		hreq->setIgnoreTlsErrors(request.ignoreTlsErrors);

		if(request.credits != -1)
			outCredits += request.credits;

		HttpHeaders headers = request.headers;
		// ensure content-length (or overwrite it, if not streaming input)
		if((request.method == "POST" || request.method == "PUT") && (!headers.contains("content-length") || !request.more))
			headers += HttpHeader("Content-Length", QByteArray::number(request.body.size()));

		timer = new QTimer(this);
		connect(timer, SIGNAL(timeout()), SLOT(timer_timeout()));
		timer->setSingleShot(true);
		timer->start(config->sessionTimeout * 1000);

		hreq->start(request.method, request.uri, headers);

		// note: unlike follow-up requests, the initial request is assumed to have a body.
		//   if no body field is present, we act as if it is present but empty.

		if(!request.body.isEmpty())
		{
			if(request.more && !request.headers.contains("content-length"))
			{
				log_warning("streamed input requires content-length");
				QMetaObject::invokeMethod(this, "respondError", Qt::QueuedConnection, Q_ARG(QByteArray, "length-required"));
				return;
			}

			hreq->writeBody(request.body);
		}

		if(!request.more)
		{
			bodySent = true;
			hreq->endBody();
		}
		else
		{
			// send cts
			ZhttpResponsePacket resp;
			resp.type = ZhttpResponsePacket::Credit;
			resp.credits = config->sessionBufferSize;
			writeResponse(resp);
		}
	}

	void write(const QVariant &vrequest)
	{
		ZhttpRequestPacket request;
		if(!request.fromVariant(vrequest))
		{
			QVariantHash vhash = vrequest.toHash();
			if(vhash["type"].toByteArray() != "cancel")
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
		if(inSeq == -1 || request.seq == -1 || request.seq != inSeq + 1)
		{
			if(request.type != ZhttpRequestPacket::Cancel)
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

		if(request.type == ZhttpRequestPacket::Cancel)
		{
			cleanup();
			QMetaObject::invokeMethod(q, "finished", Qt::QueuedConnection);
			return;
		}

		refreshTimeout();

		inSeq = request.seq;

		// all we care about from follow-up writes are body and credits

		if(request.credits != -1)
			outCredits += request.credits;

		if(request.type == ZhttpRequestPacket::Data)
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
		out.from = config->clientId;
		out.id = rid;
		out.type = ZhttpResponsePacket::Data;
		out.seq = outSeq++;
		out.userData = userData;

		// only log if error or body packet. this way we don't log cts or credits-only packets

		if(out.type == ZhttpResponsePacket::Error)
		{
			log_info("OUT ERR id=%s condition=%s", out.id.data(), out.condition.data());
		}
		else if(out.type == ZhttpResponsePacket::Data)
		{
			if(resp.code != -1)
				log_info("OUT id=%s code=%d %d%s", out.id.data(), out.code, out.body.size(), out.more ? " M" : "");
			else
				log_debug("OUT id=%s %d%s", out.id.data(), out.body.size(), out.more ? " M" : "");
		}

		if(!quiet)
			emit q->readyRead(receiver, out.toVariant());
	}

	// emits signals, but safe to delete after
	void trySend()
	{
		QPointer<QObject> self = this;

		stuffToRead = false;

		ZhttpResponsePacket resp;
		resp.type = ZhttpResponsePacket::Data;

		if(!sentHeader)
		{
			resp.code = hreq->responseCode();
			resp.reason = hreq->responseStatus();
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

			if(!buf.isNull())
				resp.body = buf;
			else
				resp.body = "";

			bytesReceived += resp.body.size();

			if(!quiet)
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
		timer->start(config->sessionTimeout * 1000);
	}

private slots:
	void respondError(const QByteArray &condition)
	{
		QPointer<QObject> self = this;

		ZhttpResponsePacket resp;
		resp.type = ZhttpResponsePacket::Error;
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

		trySend();
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
			case HttpRequest::ErrorGeneric:
			default:
				condition = "undefined-condition";
				break;
		}

		respondError(condition);
	}

	void timer_timeout()
	{
		respondError("session-timeout");
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
