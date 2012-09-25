#include "zurlresponsepacket.h"

ZurlResponsePacket::ZurlResponsePacket() :
	isError(false),
	code(-1),
	more(false),
	credits(-1)
{
}

QVariant ZurlResponsePacket::toVariant() const
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
		obj["seq"] = seq;

		if(!replyAddress.isEmpty())
			obj["reply-address"] = replyAddress;

		if(code != -1)
		{
			obj["code"] = code;
			obj["status"] = status;
			QVariantList vheaders;
			foreach(const HttpHeader &h, headers)
			{
				QVariantList vheader;
				vheader += h.first;
				vheader += h.second;
				vheaders += QVariant(vheader);
			}
			obj["headers"] = vheaders;
		}

		if(!body.isNull())
			obj["body"] = body;

		if(more)
			obj["more"] = true;

		if(credits != -1)
			obj["credits"] = credits;
	}

	if(userData.isValid())
		obj["user-data"] = userData;

	return obj;
}
