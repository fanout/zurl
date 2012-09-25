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
