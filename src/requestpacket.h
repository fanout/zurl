#ifndef REQUESTPACKET_H
#define REQUESTPACKET_H

#include <QUrl>
#include <QVariant>
#include "request.h"

class RequestPacket
{
public:
	QByteArray id;

	QString method;
	QUrl url;
	QList<Request::Header> headers;
	QByteArray body;
	bool stream;
	int maxSize;
	QString connectHost;
	QVariant userData;

	RequestPacket();

	bool fromVariant(const QVariant &in);
};

#endif
