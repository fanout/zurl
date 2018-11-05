/*
 * Copyright (C) 2012-2018 Fanout, Inc.
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

#ifndef HTTPREQUEST_H
#define HTTPREQUEST_H

#include <QObject>
#include "httpheaders.h"

class QHostAddress;
class QUrl;

class HttpRequest : public QObject
{
	Q_OBJECT

public:
	enum ErrorCondition
	{
		ErrorNone,
		ErrorGeneric,
		ErrorPolicy,
		ErrorConnect,
		ErrorTls,
		ErrorTimeout,
		ErrorBodyNotAllowed,
		ErrorTooManyRedirects
	};

	HttpRequest(QObject *parent = 0);
	~HttpRequest();

	void setConnectHostPort(const QString &host, int port);
	void setTrustConnectHost(bool on);
	void setIgnoreTlsErrors(bool on);
	void setFollowRedirects(int maxRedirects); // -1 to disable
	void setAllowIPv6(bool on);

	void start(const QString &method, const QUrl &uri, const HttpHeaders &headers = HttpHeaders(), bool willWriteBody = true);

	// may call this multiple times
	void writeBody(const QByteArray &body);

	void endBody();

	int bytesAvailable() const;
	bool isFinished() const;
	ErrorCondition errorCondition() const;

	int responseCode() const;
	QByteArray responseReason() const;
	HttpHeaders responseHeaders() const;

	QByteArray readResponseBody(int size = -1); // takes from the buffer

	// call in response to nextAddress() signal
	void blockAddress();

	static void setPersistentConnectionMaxTime(int secs);

signals:
	// NOTE: not DOR-SS
	void nextAddress(const QHostAddress &addr);

	void readyRead();
	void bytesWritten(int count);
	void error();

private:
	class ReqBodyDevice;

	class Private;
	friend class Private;
	Private *d;
};

#endif
