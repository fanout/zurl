/*
 * Copyright (C) 2015 Fanout, Inc.
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

#include "addressresolver.h"

#include <assert.h>
#include "qjdnsshared.h"
#include "log.h"

class AddressResolver::Private : public QObject
{
	Q_OBJECT

public:
	AddressResolver *q;
	QJDnsShared *dns;
	QList<QByteArray> searchDomains;
	QString host;
	bool absoluteFirst;
	bool didAbsolute;
	QList<QHostAddress> results;

	Private(AddressResolver *_q, QJDnsShared *_dns) :
		QObject(_q),
		q(_q),
		dns(_dns),
		absoluteFirst(false),
		didAbsolute(false)
	{
		searchDomains = QJDnsShared::domains();
	}

	void start(const QString &hostName)
	{
		host = hostName;

		QHostAddress addr(host);
		if(!addr.isNull())
		{
			results += addr;
			QMetaObject::invokeMethod(this, "doFinish", Qt::QueuedConnection);
			return;
		}

		if(host.contains(".") || host == "localhost")
			absoluteFirst = true;

		nextQuery();
	}

private:
	void nextQuery()
	{
		QString h;

		if(!didAbsolute && (absoluteFirst || searchDomains.isEmpty()))
		{
			didAbsolute = true;
			h = host;
		}
		else
		{
			assert(!searchDomains.isEmpty());
			h = host + "." + QString::fromUtf8(searchDomains.takeFirst());
		}

		QByteArray rawHost = QUrl::toAce(h);
		log_debug("resolving: [%s]", rawHost.data());

		QJDnsSharedRequest *dreq = new QJDnsSharedRequest(dns, this);
		connect(dreq, SIGNAL(resultsReady()), SLOT(dreq_resultsReady()));
		dreq->query(rawHost, QJDns::A);
	}

private slots:
	void dreq_resultsReady()
	{
		QJDnsSharedRequest *dreq = (QJDnsSharedRequest *)sender();

		if(dreq->success())
		{
			foreach(const QJDns::Record &r, dreq->results())
			{
				if(r.type == QJDns::A)
					results += r.address;
			}

			delete dreq;
			emit q->resultsReady(results);
		}
		else
		{
			delete dreq;

			if(!didAbsolute || !searchDomains.isEmpty())
			{
				nextQuery();
				return;
			}

			emit q->error();
		}
	}

	void doFinish()
	{
		emit q->resultsReady(results);
	}
};

AddressResolver::AddressResolver(QJDnsShared *dns, QObject *parent) :
	QObject(parent)
{
	d = new Private(this, dns);
}

AddressResolver::~AddressResolver()
{
	delete d;
}

void AddressResolver::start(const QString &hostName)
{
	d->start(hostName);
}

#include "addressresolver.moc"
