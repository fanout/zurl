/*
 * Copyright (C) 2015 Fanout, Inc.
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
	}

	void start(const QString &hostName)
	{
		results.clear();

		host = hostName;

		QHostAddress addr(host);
		if(!addr.isNull())
		{
			results += addr;
			QMetaObject::invokeMethod(this, "doFinish", Qt::QueuedConnection);
			return;
		}

		absoluteFirst = (host.contains(".") || host == "localhost");
		didAbsolute = false;

		searchDomains = QJDnsShared::domains();

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
		connect(dreq, &QJDnsSharedRequest::resultsReady, this, &Private::dreq_resultsReady);
		dreq->query(rawHost, QJDns::A);
	}

private slots:
	void dreq_resultsReady()
	{
		QJDnsSharedRequest *dreq = (QJDnsSharedRequest *)sender();

		if(dreq->success())
		{
			QList<QHostAddress> tmp;
			foreach(const QJDns::Record &r, dreq->results())
			{
				if(r.type == QJDns::A)
					tmp += r.address;
			}

			delete dreq;

			// randomize the results
			while(!tmp.isEmpty())
				results += tmp.takeAt(qrand() % tmp.count());

			doFinish();
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
