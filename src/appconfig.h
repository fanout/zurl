/*
 * Copyright (C) 2012 Fan Out Networks, Inc.
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef APPCONFIG_H
#define APPCONFIG_H

#include <QString>
#include <QStringList>

class AppConfig
{
public:
	QByteArray clientId;
	QString defaultPolicy;
	QStringList allowExps, denyExps;
	int maxWorkers;
	int sessionBufferSize;
	int sessionTimeout;
};

#endif
