#ifndef RESPONSEPACKET_H
#define RESPONSEPACKET_H

#include "request.h"

class ResponsePacket
{
public:
	QByteArray id;

	bool isError;
	QByteArray condition;

	bool isLast;
	int code;
	QByteArray status;
	QList<Request::Header> headers;
	QByteArray body;

	ResponsePacket();

	QByteArray toByteArray() const;
};

#endif
