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

#include <QCoreApplication>
#include <QTimer>
#include <QtCrypto>
#include "app.h"

class AppMain : public QObject
{
	Q_OBJECT

public slots:
	void start()
	{
		App *app = new App(this);
		connect(app, SIGNAL(quit()), SIGNAL(quit()));
		app->start();
	}

signals:
	void quit();
};

int main(int argc, char **argv)
{
	QCA::Initializer qcaInit;
	QCoreApplication qapp(argc, argv);
	AppMain appMain;
	QObject::connect(&appMain, SIGNAL(quit()), &qapp, SLOT(quit()));
	QTimer::singleShot(0, &appMain, SLOT(start()));
	return qapp.exec();
}

#include "main.moc"
