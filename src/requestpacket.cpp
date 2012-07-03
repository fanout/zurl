#include "requestpacket.h"

#include "tnetstring.h"

RequestPacket::RequestPacket() :
	stream(false),
	maxResponseSize(-1)
{
}

bool RequestPacket::fromVariant(const QVariant &in)
{
	if(in.type() != QVariant::Hash)
		return false;

	QVariantHash obj = in.toHash();

	if(!obj.contains("id") || obj["id"].type() != QVariant::ByteArray)
		return false;
	id = obj["id"].toByteArray();

	if(!obj.contains("method") || obj["method"].type() != QVariant::ByteArray)
		return false;
	method = QString::fromLatin1(obj["method"].toByteArray());

	if(!obj.contains("url") || obj["url"].type() != QVariant::ByteArray)
		return false;
	url = QUrl::fromEncoded(obj["url"].toByteArray(), QUrl::StrictMode);

	if(obj.contains("headers"))
	{
		if(obj["headers"].type() != QVariant::List)
			return false;

		headers.clear();
		foreach(const QVariant &i, obj["headers"].toList())
		{
			QVariantList list = i.toList();
			if(list.count() != 2)
				return false;

			if(list[0].type() != QVariant::ByteArray || list[1].type() != QVariant::ByteArray)
				return false;

			headers += QPair<QByteArray, QByteArray>(list[0].toByteArray(), list[1].toByteArray());
		}
	}

	body.clear();
	if(obj.contains("body"))
	{
		if(obj["body"].type() != QVariant::ByteArray)
			return false;

		body = obj["body"].toByteArray();
	}

	stream = false;
	if(obj.contains("stream"))
	{
		if(obj["stream"].type() != QVariant::Bool)
			return false;

		stream = obj["stream"].toBool();
	}

	maxResponseSize = -1;
	if(obj.contains("maxResponseSize"))
	{
		if(obj["maxResponseSize"].type() != QVariant::Int)
			return false;

		maxResponseSize = obj["maxResponseSize"].toInt();
	}

	return true;
}
