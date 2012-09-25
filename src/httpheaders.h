#ifndef HTTPHEADERS_H
#define HTTPHEADERS_H

#include <QByteArray>
#include <QPair>
#include <QList>

typedef QPair<QByteArray, QByteArray> HttpHeader;

class HttpHeaders : public QList<HttpHeader>
{
public:
	bool contains(const QByteArray &key) const;
	void removeAll(const QByteArray &key);
};

#endif
