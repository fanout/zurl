/*
 * Copyright (C) 2015-2022 Fanout, Inc.
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
#include <QHostInfo>
#include "log.h"

class AddressResolver::Private : public QObject
{
	Q_OBJECT

public:
	AddressResolver *q;
	bool started;
	int lookupId;
	QList<QHostAddress> results;

	Private(AddressResolver *_q) :
		QObject(_q),
		q(_q),
		started(false)
	{
	}

	~Private()
	{
		cancel();
	}

	void cancel()
	{
		if(started)
		{
			started = false;
			QHostInfo::abortHostLookup(lookupId);
		}

		results.clear();
	}

	void start(const QString &hostName)
	{
		cancel();

		QHostAddress addr(hostName);
		if(!addr.isNull())
		{
			results += addr;
			QMetaObject::invokeMethod(this, "doFinish", Qt::QueuedConnection);
			return;
		}

		log_debug("resolving: [%s]", qPrintable(hostName));

		lookupId = QHostInfo::lookupHost(hostName, this, SLOT(lookupHost_finished(QHostInfo)));
		started = true;
	}

private slots:
	void lookupHost_finished(QHostInfo info)
	{
		started = false;

		if(info.error() != QHostInfo::NoError)
		{
			emit q->error();
			return;
		}

		results += info.addresses();

		doFinish();
	}

	void doFinish()
	{
		emit q->resultsReady(results);
	}
};

AddressResolver::AddressResolver(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
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
