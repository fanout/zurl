/*
 * Copyright (C) 2012-2013 Fanout, Inc.
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

#ifdef USE_CURL
#include <curl/curl.h>
#endif

#ifdef USE_QNAM
#include <QtCrypto>
#endif

#include "app.h"

class AppMain : public QObject
{
	Q_OBJECT

public:
	App *app;

public slots:
	void start()
	{
		app = new App(this);
		connect(app, &App::quit, this, &AppMain::app_quit);
		app->start();
	}

	void app_quit()
	{
		delete app;
		emit quit();
	}

signals:
	void quit();
};

int main(int argc, char **argv)
{
#ifdef USE_QNAM
	QCA::Initializer qcaInit;
#endif
	QCoreApplication qapp(argc, argv);
	AppMain appMain;
	QObject::connect(&appMain, &AppMain::quit, &qapp, &QCoreApplication::quit);
	QTimer::singleShot(0, &appMain, SLOT(start()));
	return qapp.exec();
}

#include "main.moc"
