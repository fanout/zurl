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

#include "qjdnsshared.h"
#include "log.h"

class AddressResolver::Private : public QObject
{
	Q_OBJECT

public:
	AddressResolver *q;
	QJDnsShared *dns;
	QList<QHostAddress> results;

	Private(AddressResolver *_q, QJDnsShared *_dns) :
		QObject(_q),
		q(_q),
		dns(_dns)
	{
		QList<QByteArray> domains = QJDnsShared::domains();
		log_debug("domains:");
		foreach(const QByteArray &domain, domains)
			log_debug("  [%s]", domain.data());
	}

	void start(const QString &hostName)
	{
		QHostAddress addr(hostName);
		if(!addr.isNull())
		{
			results += addr;
			QMetaObject::invokeMethod(this, "doFinish", Qt::QueuedConnection);
		}

		QJDnsSharedRequest *dreq = new QJDnsSharedRequest(dns, this);
		connect(dreq, SIGNAL(resultsReady()), SLOT(dreq_resultsReady()));
		dreq->query(QUrl::toAce(hostName), QJDns::A);
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
