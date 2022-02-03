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

#ifndef ADDRESSRESOLVER_H
#define ADDRESSRESOLVER_H

#include <QObject>
#include <QHostAddress>

class AddressResolver : public QObject
{
	Q_OBJECT

public:
	AddressResolver(QObject *parent = 0);
	~AddressResolver();

	void start(const QString &hostName);

signals:
	void resultsReady(const QList<QHostAddress> &results);
	void error();

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
