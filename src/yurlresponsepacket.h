#ifndef YURLRESPONSEPACKET_H
#define YURLRESPONSEPACKET_H

#include <QVariant>
#include "httpheaders.h"

class YurlResponsePacket
{
public:
	QByteArray id;
	int seq;

	bool isError;
	QByteArray condition;

	bool more;
	QByteArray replyAddress;
	int code;
	QByteArray status;
	HttpHeaders headers;
	QByteArray body;
	QVariant userData;
	int credits;

	YurlResponsePacket();

	QVariant toVariant() const;
};

#endif
