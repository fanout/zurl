/*
 * Copyright (C) 2013 Fanout, Inc.
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

#include <QTcpSocket>
#include <QTcpServer>
#include <QtTest/QtTest>
#include "log.h"
#include "jdnsshared.h"
#include "httpheaders.h"
#include "httprequest.h"

class HttpServer : public QObject
{
	Q_OBJECT

public:
	QTcpServer *server;
	QTcpSocket *sock;

	HttpServer(QObject *parent = 0) :
		QObject(parent),
		server(0),
		sock(0)
	{
	}

	bool listen(int port)
	{
		server = new QTcpServer(this);
		connect(server, SIGNAL(newConnection()), SLOT(server_newConnection()));
		if(server->listen(QHostAddress::Any, port))
			return true;

		delete server;
		server = 0;
		return false;
	}

private slots:
	void server_newConnection()
	{
		sock = server->nextPendingConnection();
		connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
		connect(sock, SIGNAL(disconnected()), SLOT(sock_disconnected()));
	}

	void sock_readyRead()
	{
		sock->write("HTTP/1.0 200 OK\r\nContent-Length: 12\r\n\r\nhello world\n");
		sock->close();
	}

	void sock_disconnected()
	{
		sock->setParent(0);
		sock->disconnect(this);
		sock->deleteLater();
		sock = 0;
	}
};

class HttpRequestTest : public QObject
{
	Q_OBJECT

private:
	HttpServer *server;
	JDnsShared *dns;

private slots:
	void initTestCase()
	{
		log_setOutputLevel(LOG_LEVEL_INFO);

		server = new HttpServer(this);
		server->listen(10000);

		dns = new JDnsShared(JDnsShared::UnicastInternet, this);
		dns->addInterface(QHostAddress::Any);
		dns->addInterface(QHostAddress::AnyIPv6);
	}

	void cleanupTestCase()
	{
		delete dns;
		delete server;
	}

	void requestDnsError()
	{
		HttpRequest req(dns);
		QSignalSpy spy(&req, SIGNAL(error()));
		req.start("GET", QUrl("http://nosuchhost:10000/"));
		req.endBody();
		while(spy.isEmpty())
		{
			QTest::qWait(100);
		}

		QVERIFY(req.errorCondition() == HttpRequest::ErrorConnect);
	}

	void requestConnectError()
	{
		HttpRequest req(dns);
		QSignalSpy spy(&req, SIGNAL(error()));
		req.start("GET", QUrl("http://localhost:10001/"));
		req.endBody();
		while(spy.isEmpty())
		{
			QTest::qWait(100);
		}

		QVERIFY(req.errorCondition() == HttpRequest::ErrorConnect);
	}

	void requestGet()
	{
		HttpRequest req(dns);
		req.start("GET", QUrl("http://localhost:10000/"), HttpHeaders());
		req.endBody();
		QByteArray respBody;
		while(!req.isFinished())
		{
			respBody += req.readResponseBody();
			QTest::qWait(100);
		}
		respBody += req.readResponseBody();
		HttpHeaders respHeaders = req.responseHeaders();

		QCOMPARE(req.responseCode(), 200);
		QCOMPARE(req.responseReason(), QByteArray("OK"));
		QVERIFY(respHeaders.contains("content-length"));
		QCOMPARE(respHeaders.get("content-length").toInt(), 12);
		QCOMPARE(respBody, QByteArray("hello world\n"));
	}
};

QTEST_MAIN(HttpRequestTest)
#include "httprequesttest.moc"
