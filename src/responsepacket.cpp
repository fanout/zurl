#include "responsepacket.h"

#include "tnetstring.h"

ResponsePacket::ResponsePacket() :
	isError(false),
	isLast(false),
	code(-1)
{
}

QByteArray ResponsePacket::toByteArray() const
{
	QVariantHash obj;
	obj["id"] = id;

	if(isError)
	{
		obj["error"] = true;
		obj["condition"] = condition;
	}
	else
	{
		if(isLast)
			obj["last"] = true;
		obj["code"] = code;
		obj["status"] = status;
		QVariantList vheaders;
		foreach(const Request::Header &h, headers)
			vheaders += h.first + ": " + h.second;
		obj["headers"] = vheaders;
		obj["body"] = body;
	}

	return TnetString::fromVariant(obj);
}
