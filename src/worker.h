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

#ifndef WORKER_H
#define WORKER_H

#include <QObject>

class QVariant;
class ZhttpRequestPacket;
class ZhttpResponsePacket;
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

	enum Format
	{
		TnetStringFormat,
		JsonFormat
	};

	Worker(AppConfig *config, Format format, QObject *parent = 0);
	~Worker();

	QByteArray rid() const;
	Format format() const;

	void start(const QByteArray &id, int seq, const ZhttpRequestPacket &request, Mode mode);
	void write(int seq, const ZhttpRequestPacket &request);

signals:
	void readyRead(const QByteArray &receiver, const ZhttpResponsePacket &response);
	void finished();

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
