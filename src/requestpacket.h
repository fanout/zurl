#ifndef REQUESTPACKET_H
#define REQUESTPACKET_H

#include <QUrl>
#include "request.h"

class QVariant;

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

	RequestPacket();

	bool fromVariant(const QVariant &in);
};

#endif
