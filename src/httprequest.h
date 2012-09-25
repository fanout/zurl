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

#ifndef HTTPREQUEST_H
#define HTTPREQUEST_H

#include <QObject>
#include "httpheaders.h"

class QHostAddress;
class QUrl;
class JDnsShared;

class HttpRequest : public QObject
{
	Q_OBJECT

public:
	enum ErrorCondition
	{
		ErrorGeneric,
		ErrorPolicy,
		ErrorConnect,
		ErrorTls,
		ErrorTimeout
	};

	HttpRequest(JDnsShared *dns, QObject *parent = 0);
	~HttpRequest();

	void setConnectHost(const QString &host);

	void start(const QString &method, const QUrl &url, const HttpHeaders &headers);

	// may call this multiple times
	void writeBody(const QByteArray &body);

	void endBody();

	int bytesAvailable() const;
	bool isFinished() const;
	ErrorCondition errorCondition() const;

	int responseCode() const;
	QByteArray responseStatus() const;
	HttpHeaders responseHeaders() const;

	QByteArray readResponseBody(int size = -1); // takes from the buffer

signals:
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
