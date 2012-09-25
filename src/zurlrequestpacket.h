#ifndef ZURLREQUESTPACKET_H
#define ZURLREQUESTPACKET_H

#include <QUrl>
#include <QVariant>
#include "httpheaders.h"

class ZurlRequestPacket
{
public:
	QByteArray id;
	QByteArray sender;
	int seq;

	bool cancel;
	QString method;
	QUrl url;
	HttpHeaders headers;
	QByteArray body;
	bool more;
	bool stream;
	int maxSize;
	QString connectHost;
	QVariant userData;
	int credits;

	ZurlRequestPacket();

	bool fromVariant(const QVariant &in);
};

#endif
