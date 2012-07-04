#ifndef REQUEST_H
#define REQUEST_H

#include <QObject>
#include <QPair>

class QHostAddress;
class QUrl;
class JDnsShared;

class Request : public QObject
{
	Q_OBJECT

public:
	typedef QPair<QByteArray, QByteArray> Header;

	enum ErrorCondition
	{
		ErrorGeneric,
		ErrorPolicy,
		ErrorConnect,
		ErrorTimeout,
		ErrorMaxSizeExceeded
	};

	Request(JDnsShared *dns, QObject *parent = 0);
	~Request();

	void setMaximumResponseSize(int size);

	void start(const QString &method, const QUrl &url, const QList<Header> &headers, const QByteArray &body);
	void stop();

	bool isFinished() const;
	ErrorCondition errorCondition() const;

	int responseCode() const;
	QByteArray responseStatus() const;
	QList<Header> responseHeaders() const;

	QByteArray readResponseBody(); // takes from the buffer

signals:
	void nextAddress(const QHostAddress &addr);
	void readyRead();
	void error();

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
