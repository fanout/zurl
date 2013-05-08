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

#ifndef WORKER_H
#define WORKER_H

#include <QObject>

class QVariant;
class JDnsShared;
class AppConfig;

class Worker : public QObject
{
	Q_OBJECT

public:
	enum Mode
	{
		Single, // for REQ/REP
		Stream  // for PUSH/PUB
	};

	Worker(JDnsShared *dns, AppConfig *config, QObject *parent = 0);
	~Worker();

	QByteArray rid() const;

	void start(const QVariant &request, Mode mode);
	void write(const QVariant &request);

signals:
	void readyRead(const QByteArray &receiver, const QVariant &response);
	void finished();

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
