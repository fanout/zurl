#include "request.h"

#include <stdio.h>
#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include "jdnsshared.h"

class Request::Private : public QObject
{
	Q_OBJECT

public:
	Request *q;
	JDnsShared *dns;
	int maxResponseSize;
	Request::ErrorCondition errorCondition;
	QNetworkAccessManager *nam;
	QString method;
	QUrl url;
	QList<Request::Header> headers;
	QByteArray outbuf;
	QString host;
	QList<QHostAddress> addrs;
	QNetworkReply *reply;
	QByteArray inbuf;

	Private(Request *_q, JDnsShared *_dns) :
		QObject(_q),
		q(_q),
		dns(_dns),
		maxResponseSize(-1),
		reply(0)
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

		nam->setParent(0);
		nam->deleteLater();
	}

	void start(const QString &_method, const QUrl &_url, const QList<Request::Header> &_headers, const QByteArray &body)
	{
		method = _method;
		url = _url;
		headers = _headers;
		outbuf = body;

		host = url.host();
		QHostAddress addr(host);
		if(!addr.isNull())
		{
			addrs += addr;
			tryNextAddress();
		}
		else
		{
			JDnsSharedRequest *dreq = new JDnsSharedRequest(dns);
			connect(dreq, SIGNAL(resultsReady()), SLOT(dreq_resultsReady()));
			dreq->query(QUrl::toAce(host), QJDns::A);
		}
	}

	void tryNextAddress()
	{
		if(addrs.isEmpty())
		{
			// note: this will never happen within start()
			errorCondition = Request::ErrorConnect;
			emit q->error();
			return;
		}

		QHostAddress addr = addrs.takeFirst();

		emit q->nextAddress(addr);

		QNetworkRequest request;
		QUrl tmpUrl = url;
		tmpUrl.setHost(addr.toString());
		request.setUrl(tmpUrl);
		request.setRawHeader("Host", host.toUtf8());

		foreach(const Request::Header &h, headers)
			request.setRawHeader(h.first, h.second);

		if(method == "HEAD")
			reply = nam->head(request);
		else if(method == "GET")
			reply = nam->get(request);
		else if(method == "POST")
			reply = nam->post(request, outbuf);
		else if(method == "PUT")
			reply = nam->put(request, outbuf);
		else if(method == "DELETE")
			reply = nam->deleteResource(request);
		else
		{
			errorCondition = Request::ErrorGeneric;
			QMetaObject::invokeMethod(q, "error", Qt::QueuedConnection);
			return;
		}

		reply->setParent(this);
		connect(reply, SIGNAL(readyRead()), SLOT(reply_readyRead()));
		connect(reply, SIGNAL(finished()), SLOT(reply_finished()));
		connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), SLOT(reply_error(QNetworkReply::NetworkError)));
		connect(reply, SIGNAL(sslErrors(const QList<QSslError> &)), SLOT(reply_sslErrors(const QList<QSslError> &)));
	}

private slots:
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
			errorCondition = Request::ErrorConnect;
			emit q->error();
		}
	}

	void reply_readyRead()
	{
		inbuf += reply->readAll();
		emit q->readyRead();
	}

	void reply_finished()
	{
		emit q->readyRead();
	}

	void reply_error(QNetworkReply::NetworkError code)
	{
		if(code == QNetworkReply::ConnectionRefusedError)
			errorCondition = Request::ErrorConnect;
		else if(code == QNetworkReply::TimeoutError)
			errorCondition = Request::ErrorTimeout;
		else
			errorCondition = Request::ErrorGeneric;

		emit q->error();
	}

	void reply_sslErrors(const QList<QSslError> &errors)
	{
		// we don't need to do anything here as error() still gets emitted either way
		Q_UNUSED(errors);
	}
};

Request::Request(JDnsShared *dns, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, dns);
}

Request::~Request()
{
	delete d;
}

void Request::setMaximumResponseSize(int size)
{
	d->maxResponseSize = size;
}

void Request::start(const QString &method, const QUrl &url, const QList<Header> &headers, const QByteArray &body)
{
	d->start(method, url, headers, body);
}

bool Request::isFinished() const
{
	if(d->reply)
		return d->reply->isFinished();
	else
		return false;
}

Request::ErrorCondition Request::errorCondition() const
{
	return d->errorCondition;
}

int Request::responseCode() const
{
	if(d->reply)
		return d->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	else
		return -1;
}

QByteArray Request::responseStatus() const
{
	if(d->reply)
		return d->reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toByteArray();
	else
		return QByteArray();
}

QList<Request::Header> Request::responseHeaders() const
{
	if(d->reply)
		return d->reply->rawHeaderPairs();
	else
		return QList<Header>();
}

QByteArray Request::readResponseBody()
{
	QByteArray out = d->inbuf;
	d->inbuf.clear();
	return out;
}

#include "request.moc"
