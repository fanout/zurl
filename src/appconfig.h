#ifndef APPCONFIG_H
#define APPCONFIG_H

#include <QString>
#include <QStringList>

class AppConfig
{
public:
	QString defaultPolicy;
	QStringList allowExps, denyExps;
	int maxWorkers;

	// putting this here for convenience, even though it's not from config
	QByteArray clientId;
};

#endif
