#ifndef YURLREQUESTPACKET_H
#define YURLREQUESTPACKET_H

#include <QUrl>
#include <QVariant>
#include "httpheaders.h"

class YurlRequestPacket
{
public:
	QByteArray id;
	int seq;

	bool cancel;
	bool more;
	QString method;
	QUrl url;
	HttpHeaders headers;
	QByteArray body;
	bool stream;
	int maxSize;
	QString connectHost;
	QVariant userData;
	int credits;

	YurlRequestPacket();

	bool fromVariant(const QVariant &in);
};

#endif
