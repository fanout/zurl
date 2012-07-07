#ifndef RESPONSEPACKET_H
#define RESPONSEPACKET_H

#include <QVariant>
#include "request.h"

class ResponsePacket
{
public:
	QByteArray id;
	int seq;

	bool isError;
	QByteArray condition;

	bool isLast;
	int code;
	QByteArray status;
	QList<Request::Header> headers;
	QByteArray body;
	QVariant userData;

	ResponsePacket();

	QByteArray toByteArray() const;
};

#endif
