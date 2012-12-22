/*
 * Copyright (C) 2012 Fan Out Networks, Inc.
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
#include <QPointer>
#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QtCrypto>
#include "jdnsshared.h"
#include "log.h"

class HttpRequest::ReqBodyDevice : public QIODevice
{
	Q_OBJECT

public:
	QByteArray buf;
	bool finished;
	bool accessed;

	ReqBodyDevice(QObject *parent = 0) :
		QIODevice(parent),
		finished(false),
		accessed(false)
	{
	}

	bool wasAccessed() const
	{
		return accessed;
	}

	void append(const QByteArray &in)
	{
		buf += in;

		QMetaObject::invokeMethod(this, "readyRead", Qt::QueuedConnection);
	}

	void end()
	{
		finished = true;

		QMetaObject::invokeMethod(this, "readChannelFinished", Qt::QueuedConnection);
	}

	// reimplemented
	virtual bool open(OpenMode mode)
	{
		return QIODevice::open(mode);
	}

	// reimplemented
	virtual bool isSequential() const
	{
		return true;
	}

	// reimplemented
	virtual bool atEnd() const
	{
		return (finished && buf.isEmpty());
	}

	// reimplemented
	virtual qint64 	bytesAvailable() const
	{
		return buf.size();
	}

protected:
	// reimplemented
	virtual qint64 readData(char *data, qint64 maxSize)
	{
		accessed = true;

		qint64 size = qMin((qint64)buf.size(), maxSize);
		memcpy(data, buf.data(), size);
		buf = buf.mid(size);
		if(size > 0)
			emit bytesTaken((int)size);
		return size;
	}

	// reimplemented
	virtual qint64 writeData(const char *data, qint64 maxSize)
	{
		Q_UNUSED(data);
		Q_UNUSED(maxSize);

		return -1;
	}

signals:
	void bytesTaken(int count);
};

class HttpRequest::Private : public QObject
{
	Q_OBJECT

public:
	HttpRequest *q;
	JDnsShared *dns;
	QString connectHost;
	HttpRequest::ErrorCondition errorCondition;
	QNetworkAccessManager *nam;
	QString method;
	QUrl url;
	HttpHeaders headers;
	ReqBodyDevice *outdev;
	QString host;
	QList<QHostAddress> addrs;
	QNetworkReply *reply;
	int bytesReceived;
	HttpRequest::ErrorCondition mostSignificantError;
	bool ignoreEnd;

	Private(HttpRequest *_q, JDnsShared *_dns) :
		QObject(_q),
		q(_q),
		dns(_dns),
		outdev(0),
		reply(0),
		bytesReceived(0),
		mostSignificantError(HttpRequest::ErrorGeneric),
		ignoreEnd(false)
	{
		nam = new QNetworkAccessManager(this);
	}

	~Private()
	{
		if(reply)
		{
			reply->disconnect(this);
			reply->setParent(0);
			reply->deleteLater();
		}

		if(outdev)
		{
			outdev->disconnect(this);
			outdev->setParent(0);
			outdev->deleteLater();
		}

		nam->setParent(0);
		nam->deleteLater();
	}

	void start(const QString &_method, const QUrl &_url, const HttpHeaders &_headers)
	{
		method = _method;
		url = _url;
		headers = _headers;

		if(method == "POST" || method == "PUT")
		{
			outdev = new ReqBodyDevice(this);
			connect(outdev, SIGNAL(bytesTaken(int)), SLOT(outdev_bytesTaken(int)));
			outdev->open(QIODevice::ReadOnly);

			startConnect();
		}

		// for all other method types, outdev will be 0 and we'll
		//   start the request once endBody() is called. this unifies
		//   the class usage regardless of method type.
	}

	void writeBody(const QByteArray &body)
	{
		if(outdev)
		{
			outdev->append(body);
		}
		else
		{
			// set this so a follow-up endBody() call does nothing
			ignoreEnd = true;

			errorCondition = HttpRequest::ErrorBodyNotAllowed;
			QMetaObject::invokeMethod(q, "error", Qt::QueuedConnection);
		}
	}

	void endBody()
	{
		if(outdev)
		{
			outdev->end();
		}
		else
		{
			if(!ignoreEnd)
				startConnect();
		}
	}

	void startConnect()
	{
		if(!connectHost.isEmpty())
			host = connectHost;
		else
			host = url.host();

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
	//
	// note that ErrorTimeout is kind of an odd one, as it might get
	//   occur either during a connect or after a request is already in
	//   progress. this means a request could actually get performed and
	//   timeout mid-receive, retried to another server that fails by
	//   connection refused, and the ErrorConnect would be considered more
	//   significant even though it is actually further from success in
	//   this context. but, this seems to be the best we can do with
	//   QNetworkReply.
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

		emit q->nextAddress(addr);
		if(!self)
			return;

		QNetworkRequest request;
		QUrl tmpUrl = url;
		tmpUrl.setHost(addr.toString());
		request.setUrl(tmpUrl);
		request.setRawHeader("Host", url.host().toUtf8());

		bool haveContentType = false;
		bool haveContentLength = false;
		foreach(const HttpHeader &h, headers)
		{
			QByteArray lname = h.first.toLower();
			if(lname == "content-type")
				haveContentType = true;
			else if(lname == "content-length")
				haveContentLength = true;

			request.setRawHeader(h.first, h.second);
		}

		if(outdev)
		{
			if(!haveContentType)
				request.setRawHeader("Content-Type", "application/octet-stream");

			assert(haveContentLength);
		}

		request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);

		if(method == "HEAD")
			reply = nam->head(request);
		else if(method == "GET")
			reply = nam->get(request);
		else if(method == "POST")
			reply = nam->post(request, outdev);
		else if(method == "PUT")
			reply = nam->put(request, outdev);
		else if(method == "DELETE")
			reply = nam->deleteResource(request);
		else if(method == "OPTIONS")
			reply = nam->sendCustomRequest(request, "OPTIONS");
		else
		{
			errorCondition = HttpRequest::ErrorGeneric;
			emit q->error();
			return;
		}

		reply->setParent(this);
		connect(reply, SIGNAL(readyRead()), SLOT(reply_readyRead()));
		connect(reply, SIGNAL(finished()), SLOT(reply_finished()));
		connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), SLOT(reply_error(QNetworkReply::NetworkError)));
		connect(reply, SIGNAL(sslErrors(const QList<QSslError> &)), SLOT(reply_sslErrors(const QList<QSslError> &)));
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

	void reply_readyRead()
	{
		emit q->readyRead();
	}

	void reply_finished()
	{
		emit q->readyRead();
	}

	void reply_error(QNetworkReply::NetworkError code)
	{
		log_debug("HttpRequest::reply_error: %d", (int)code);

		QVariant v = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
		if(v.isValid())
		{
			// qnetworkreply will signal error even if we got a response. so, let's
			//   ignore the error if it looks like we got a response.
			return;
		}

		HttpRequest::ErrorCondition curError;
		if(code == QNetworkReply::ConnectionRefusedError)
			curError = HttpRequest::ErrorConnect;
		else if(code == QNetworkReply::SslHandshakeFailedError)
			curError = HttpRequest::ErrorTls;
		else if(code == QNetworkReply::TimeoutError)
			curError = HttpRequest::ErrorTimeout;
		else
			curError = HttpRequest::ErrorGeneric;

		if(errorPriority(curError) > errorPriority(mostSignificantError))
			mostSignificantError = curError;

		// if we started submitting an input body, then we can't retry
		//   the request
		if(outdev && outdev->wasAccessed())
		{
			emit q->error();
			return;
		}

		tryNextAddress();
	}

	void reply_sslErrors(const QList<QSslError> &errors)
	{
		log_debug("HttpRequest::reply_sslErrors, count: %d", errors.count());

		// we'll almost always get a host mismatch error since we replace the host with ip address
		if(errors.count() == 1 && errors[0].error() == QSslError::HostNameMismatch)
		{
			// in that case, do our own host matching using qca
			QSslCertificate qtCert = reply->sslConfiguration().peerCertificate();
			QCA::Certificate qcaCert = QCA::Certificate::fromDER(qtCert.toDer());
			if(qcaCert.matchesHostName(url.host()))
				reply->ignoreSslErrors();
		}
	}

	void outdev_bytesTaken(int count)
	{
		emit q->bytesWritten(count);
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

void HttpRequest::start(const QString &method, const QUrl &url, const HttpHeaders &headers)
{
	d->start(method, url, headers);
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
	if(d->reply)
		return d->reply->bytesAvailable();
	else
		return 0;
}

bool HttpRequest::isFinished() const
{
	if(d->reply)
		return d->reply->isFinished();
	else
		return false;
}

HttpRequest::ErrorCondition HttpRequest::errorCondition() const
{
	return d->errorCondition;
}

int HttpRequest::responseCode() const
{
	if(d->reply)
		return d->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	else
		return -1;
}

QByteArray HttpRequest::responseStatus() const
{
	if(d->reply)
		return d->reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toByteArray();
	else
		return QByteArray();
}

HttpHeaders HttpRequest::responseHeaders() const
{
	if(d->reply)
	{
		HttpHeaders out;
		QList< QPair<QByteArray, QByteArray> > headerPairs = d->reply->rawHeaderPairs();
		for(int n = 0; n < headerPairs.count(); ++n)
			out += HttpHeader(headerPairs[n].first, headerPairs[n].second);
		return out;
	}
	else
		return HttpHeaders();
}

QByteArray HttpRequest::readResponseBody(int size)
{
	if(d->reply)
	{
		if(size != -1)
			return d->reply->read(size);
		else
			return d->reply->readAll();
	}
	else
		return QByteArray();
}

#include "httprequest.moc"
