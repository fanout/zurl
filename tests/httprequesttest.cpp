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

#include <assert.h>
#include <QTcpSocket>
#include <QTcpServer>
#include <QtTest/QtTest>
#include "log.h"
#include "qjdnsshared.h"
#include "httpheaders.h"
#include "httprequest.h"

class HttpServer : public QObject
{
	Q_OBJECT

public:
	QTcpServer *server;
	QTcpSocket *sock;
	bool requestParsed;
	QList<QByteArray> headerLines;
	QByteArray requestMethod;
	QByteArray requestUri;
	HttpHeaders requestHeaders;

	HttpServer(QObject *parent = 0) :
		QObject(parent),
		server(0),
		sock(0),
		requestParsed(false)
	{
	}

	bool listen()
	{
		server = new QTcpServer(this);
		connect(server, SIGNAL(newConnection()), SLOT(server_newConnection()));
		if(server->listen(QHostAddress::Any, 0))
			return true;

		delete server;
		server = 0;
		return false;
	}

	int localPort() const
	{
		return server->serverPort();
	}

	void handleRequest(const QByteArray &method, const QByteArray &uri)
	{
		if(method == "HEAD")
		{
			sock->write("HTTP/1.0 200 OK\r\nContent-Length: 12\r\n\r\n");
			return;
		}

		if(uri == "/chunked")
		{
			QByteArray body = "hello world\n";
			QByteArray buf = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
			buf += QByteArray::number(body.size(), 16).toUpper() + "\r\n" + body + "\r\n";
			buf += QByteArray::number(0, 16).toUpper() + "\r\n\r\n";
			sock->write(buf);
		}
		else
		{
			sock->write("HTTP/1.0 200 OK\r\nContent-Length: 12\r\n\r\nhello world\n");
		}
	}

private slots:
	void server_newConnection()
	{
		if(sock)
			sock->close();
		else
			takeNext();
	}

	void takeNext()
	{
		assert(!sock);

		requestParsed = false;
		headerLines.clear();
		requestMethod.clear();
		requestUri.clear();
		requestHeaders.clear();

		sock = server->nextPendingConnection();
		assert(sock);
		connect(sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
		connect(sock, SIGNAL(disconnected()), SLOT(sock_disconnected()));
	}

	void sock_readyRead()
	{
		if(!requestParsed)
		{
			while(!requestParsed && sock->canReadLine())
			{
				QByteArray line = sock->readLine();
				assert(line[line.length() - 2] == '\r');
				assert(line[line.length() - 1] == '\n');
				line.truncate(line.length() - 2);

				if(!line.isEmpty())
				{
					headerLines += line;
				}
				else
				{
					QByteArray statusLine = headerLines.first();
					int end = statusLine.indexOf(' ');
					QByteArray method = statusLine.mid(0, end);
					int start = end + 1;
					end = statusLine.indexOf(' ', start);
					QByteArray uri = statusLine.mid(start, end - start);
					requestParsed = true;

					for(int n = 1; n < headerLines.count(); ++n)
					{
						int at = headerLines[n].indexOf(": ");
						assert(at != -1);
						requestHeaders += HttpHeader(headerLines[n].mid(0, at), headerLines[n].mid(at + 2));
					}

					requestMethod = method;
					requestUri = uri;

					handleRequest(method, uri);
				}
			}
		}
		else
		{
			sock->readAll();
		}
	}

	void sock_disconnected()
	{
		sock->setParent(0);
		sock->disconnect(this);
		sock->deleteLater();
		sock = 0;

		takeNext();
	}
};

class HttpRequestTest : public QObject
{
	Q_OBJECT

private:
	HttpServer *server;
	QJDnsShared *dns;

	void waitForSignal(QSignalSpy *spy)
	{
		while(spy->isEmpty())
			QTest::qWait(10);
	}

private slots:
	void initTestCase()
	{
		log_setOutputLevel(LOG_LEVEL_INFO);

		server = new HttpServer(this);
		server->listen();

		dns = new QJDnsShared(QJDnsShared::UnicastInternet, this);
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
		req.start("GET", QString("http://nosuchhost:%1/").arg(server->localPort()));
		req.endBody();
		waitForSignal(&spy);

		QCOMPARE(req.errorCondition(), HttpRequest::ErrorConnect);
	}

	void requestConnectError()
	{
		HttpRequest req(dns);
		QSignalSpy spy(&req, SIGNAL(error()));
		req.start("GET", QString("http://localhost:1/"));
		req.endBody();
		waitForSignal(&spy);

		QCOMPARE(req.errorCondition(), HttpRequest::ErrorConnect);
	}

	void requestGet()
	{
		HttpRequest req(dns);
		req.start("GET", QString("http://localhost:%1/").arg(server->localPort()), HttpHeaders());
		req.endBody();
		QByteArray respBody;
		while(!req.isFinished())
		{
			respBody += req.readResponseBody();
			QTest::qWait(10);
		}
		respBody += req.readResponseBody();
		HttpHeaders respHeaders = req.responseHeaders();

		QCOMPARE(req.responseCode(), 200);
		QCOMPARE(req.responseReason(), QByteArray("OK"));
		QVERIFY(respHeaders.contains("content-length"));
		QCOMPARE(respHeaders.get("content-length").toInt(), 12);
		QCOMPARE(respBody, QByteArray("hello world\n"));
	}

	void requestGetChunked()
	{
		HttpRequest req(dns);
		req.start("GET", QString("http://localhost:%1/chunked").arg(server->localPort()), HttpHeaders());
		req.endBody();
		QByteArray respBody;
		while(!req.isFinished())
		{
			respBody += req.readResponseBody();
			QTest::qWait(10);
		}
		respBody += req.readResponseBody();
		HttpHeaders respHeaders = req.responseHeaders();

		QCOMPARE(req.responseCode(), 200);
		QCOMPARE(req.responseReason(), QByteArray("OK"));
		QVERIFY(!respHeaders.contains("content-length"));
		QCOMPARE(respBody, QByteArray("hello world\n"));
	}

	void requestPostBody()
	{
		HttpRequest req(dns);
		HttpHeaders headers;
		headers += HttpHeader("Content-Length", "6");
		req.start("POST", QString("http://localhost:%1/").arg(server->localPort()), headers);
		req.writeBody("hello\n");
		req.endBody();
		while(!req.isFinished())
		{
			req.readResponseBody();
			QTest::qWait(10);
		}

		QCOMPARE(req.responseCode(), 200);
		QCOMPARE(server->requestHeaders.get("Content-Length"), QByteArray("6"));
	}

	void requestPostNoBody()
	{
		HttpRequest req(dns);
		req.start("POST", QString("http://localhost:%1/").arg(server->localPort()), HttpHeaders(), false);
		while(!req.isFinished())
		{
			req.readResponseBody();
			QTest::qWait(10);
		}

		QCOMPARE(req.responseCode(), 200);
		QCOMPARE(server->requestHeaders.get("Content-Length"), QByteArray("0"));
	}

	void requestHead()
	{
		HttpRequest req(dns);
		req.start("HEAD", QString("http://localhost:%1/").arg(server->localPort()), HttpHeaders(), false);
		while(!req.isFinished())
		{
			req.readResponseBody();
			QTest::qWait(10);
		}

		QCOMPARE(req.responseCode(), 200);
		QCOMPARE(server->requestMethod, QByteArray("HEAD"));
		QVERIFY(!server->requestHeaders.contains("Content-Length"));
		QVERIFY(!server->requestHeaders.contains("Transfer-Encoding"));
	}

	void requestHeadMaybeBody()
	{
		HttpRequest req(dns);
		req.start("HEAD", QString("http://localhost:%1/").arg(server->localPort()), HttpHeaders());
		QTest::qWait(10);
		req.endBody();
		while(!req.isFinished())
		{
			req.readResponseBody();
			QTest::qWait(10);
		}

		QCOMPARE(req.responseCode(), 200);
		QCOMPARE(server->requestMethod, QByteArray("HEAD"));
		QVERIFY(!server->requestHeaders.contains("Content-Length"));
		QVERIFY(!server->requestHeaders.contains("Transfer-Encoding"));
	}

	void requestHeadTryBody()
	{
		HttpRequest req(dns);
		req.start("HEAD", QString("http://localhost:%1/").arg(server->localPort()), HttpHeaders());
		req.writeBody("hello\n");
		req.endBody();
		while(!req.isFinished())
		{
			req.readResponseBody();
			QTest::qWait(10);
		}

		QCOMPARE(req.errorCondition(), HttpRequest::ErrorBodyNotAllowed);
		QCOMPARE(server->requestMethod, QByteArray("HEAD"));
		QVERIFY(!server->requestHeaders.contains("Content-Length"));
		QVERIFY(!server->requestHeaders.contains("Transfer-Encoding"));
	}

	void requestDeleteNoBody()
	{
		HttpRequest req(dns);
		req.start("DELETE", QString("http://localhost:%1/").arg(server->localPort()), HttpHeaders(), false);
		while(!req.isFinished())
		{
			req.readResponseBody();
			QTest::qWait(10);
		}

		QCOMPARE(req.responseCode(), 200);
		QVERIFY(!server->requestHeaders.contains("Content-Length"));
		QVERIFY(!server->requestHeaders.contains("Transfer-Encoding"));
	}

	void requestDeleteBody()
	{
		HttpRequest req(dns);
		req.start("DELETE", QString("http://localhost:%1/").arg(server->localPort()), HttpHeaders());
		req.writeBody("hello\n");
		req.endBody();
		while(!req.isFinished())
		{
			req.readResponseBody();
			QTest::qWait(10);
		}

		QCOMPARE(req.responseCode(), 200);
		QVERIFY(!server->requestHeaders.contains("Content-Length"));
		QCOMPARE(server->requestHeaders.get("Transfer-Encoding"), QByteArray("chunked"));
	}

	void requestDeleteMaybeBody()
	{
		HttpRequest req(dns);
		req.start("DELETE", QString("http://localhost:%1/").arg(server->localPort()), HttpHeaders());
		req.endBody();
		while(!req.isFinished())
		{
			req.readResponseBody();
			QTest::qWait(10);
		}

		QCOMPARE(req.responseCode(), 200);
		QVERIFY(!server->requestHeaders.contains("Content-Length"));
		QCOMPARE(server->requestHeaders.get("Transfer-Encoding"), QByteArray("chunked"));
	}
};

QTEST_MAIN(HttpRequestTest)
#include "httprequesttest.moc"
