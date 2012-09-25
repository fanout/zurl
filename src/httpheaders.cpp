#include "httpheaders.h"

bool HttpHeaders::contains(const QByteArray &key) const
{
	for(int n = 0; n < count(); ++n)
	{
		if(qstricmp(at(n).first.data(), key.data()) == 0)
			return true;
	}

	return false;
}

void HttpHeaders::removeAll(const QByteArray &key)
{
	for(int n = 0; n < count(); ++n)
	{
		if(qstricmp(at(n).first.data(), key.data()) == 0)
		{
			removeAt(n);
			--n; // adjust position
		}
	}
}
