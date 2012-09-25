#ifndef ZURLRESPONSEPACKET_H
#define ZURLRESPONSEPACKET_H

#include <QVariant>
#include "httpheaders.h"

class ZurlResponsePacket
{
public:
	QByteArray id;
	int seq;

	bool isError;
	QByteArray condition;

	QByteArray replyAddress;
	int code;
	QByteArray status;
	HttpHeaders headers;
	QByteArray body;
	bool more;
	QVariant userData;
	int credits;

	ZurlResponsePacket();

	QVariant toVariant() const;
};

#endif
