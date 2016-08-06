/*
 * Copyright (C) 2013-2015 Fanout, Inc.
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
		connect(server, &QTcpServer::newConnection, this, &HttpServer::server_newConnection);
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

	void closeAllRequests()
	{
		if(sock)
		{
			sock->close();
			sock = 0;
		}
	}

	void handleRequest(const QByteArray &method, const QByteArray &uri)
	{
		if(method == "HEAD")
		{
			sock->write("HTTP/1.0 200 OK\r\nContent-Length: 12\r\n\r\n");
			return;
		}

		if(uri == "/204")
		{
			sock->write("HTTP/1.0 204 No Content\r\nContent-Length: 12\r\n\r\n");
		}
		else if(uri == "/304")
		{
			sock->write("HTTP/1.0 304 Not Modified\r\nContent-Length: 12\r\n\r\n");
		}
		else if(uri == "/chunked")
		{
			QByteArray body = "hello world\n";
			QByteArray buf = "HTTP/1.1 200 OK\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n";
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
		requestParsed = false;
		headerLines.clear();
		requestMethod.clear();
		requestUri.clear();
		requestHeaders.clear();

		sock = server->nextPendingConnection();
		assert(sock);
		connect(sock, &QTcpSocket::readyRead, this, &HttpServer::sock_readyRead);
		connect(sock, &QTcpSocket::disconnected, this, &HttpServer::sock_disconnected);
	}

	void sock_readyRead()
	{
		QTcpSocket *s = (QTcpSocket *)sender();
		if(s != sock)
			return;

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

		if(requestParsed)
			sock->readAll();
	}

	void sock_disconnected()
	{
		QTcpSocket *s = (QTcpSocket *)sender();

		if(s == sock)
			sock = 0;
		s->setParent(0);
		s->disconnect(this);
		s->deleteLater();
	}
};

class DnsDebug : public QObject
{
	Q_OBJECT

private:
	QJDnsSharedDebug *dnsDebug_;

public:
	DnsDebug(QObject *parent = 0) :
		QObject(parent)
	{
		dnsDebug_ = new QJDnsSharedDebug(this);
		connect(dnsDebug_, &QJDnsSharedDebug::readyRead, this, &DnsDebug::flush);
	}

	void applyTo(QJDnsShared *dns)
	{
		dns->setDebug(dnsDebug_, "U");
	}

public slots:
	void flush()
	{
		foreach(const QString &line, dnsDebug_->readDebugLines())
			log_debug("%s", qPrintable(line));
	}
};

class HttpRequestTest : public QObject
{
	Q_OBJECT

private:
	HttpServer *server;
	DnsDebug *dnsDebug;
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
		//log_setOutputLevel(LOG_LEVEL_DEBUG);

		server = new HttpServer(this);
		server->listen();

		dnsDebug = new DnsDebug(this);

		dns = new QJDnsShared(QJDnsShared::UnicastInternet, this);
		dnsDebug->applyTo(dns);

		dns->addInterface(QHostAddress::Any);
		dns->addInterface(QHostAddress::AnyIPv6);
	}

	void cleanupTestCase()
	{
		// this will delete dns
		QJDnsShared::waitForShutdown(QList<QJDnsShared*>() << dns);

		delete dnsDebug;
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
		server->closeAllRequests();
	}

	void requestConnectError()
	{
		HttpRequest req(dns);
		QSignalSpy spy(&req, SIGNAL(error()));
		req.start("GET", QString("http://localhost:1/"));
		req.endBody();
		waitForSignal(&spy);

		QCOMPARE(req.errorCondition(), HttpRequest::ErrorConnect);
		server->closeAllRequests();
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

		QCOMPARE(server->requestMethod, QByteArray("GET"));
		QCOMPARE(req.responseCode(), 200);
		QCOMPARE(req.responseReason(), QByteArray("OK"));
		QVERIFY(respHeaders.contains("content-length"));
		QCOMPARE(respHeaders.get("content-length").toInt(), 12);
		QCOMPARE(respBody, QByteArray("hello world\n"));
		server->closeAllRequests();
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

		QCOMPARE(server->requestMethod, QByteArray("GET"));
		QCOMPARE(req.responseCode(), 200);
		QCOMPARE(req.responseReason(), QByteArray("OK"));
		QVERIFY(!respHeaders.contains("content-length"));
		QCOMPARE(respBody, QByteArray("hello world\n"));
		server->closeAllRequests();
	}

	void requestGetNoContent()
	{
		HttpRequest req(dns);
		req.start("GET", QString("http://localhost:%1/204").arg(server->localPort()), HttpHeaders());
		req.endBody();
		QByteArray respBody;
		while(!req.isFinished())
		{
			respBody += req.readResponseBody();
			QTest::qWait(10);
		}
		respBody += req.readResponseBody();
		HttpHeaders respHeaders = req.responseHeaders();

		QCOMPARE(server->requestMethod, QByteArray("GET"));
		QCOMPARE(req.responseCode(), 204);
		QCOMPARE(req.responseReason(), QByteArray("No Content"));
		QVERIFY(respHeaders.contains("content-length"));
		QCOMPARE(respHeaders.get("content-length").toInt(), 12);
		server->closeAllRequests();
	}

	void requestGetNotModified()
	{
		HttpRequest req(dns);
		req.start("GET", QString("http://localhost:%1/304").arg(server->localPort()), HttpHeaders());
		req.endBody();
		QByteArray respBody;
		while(!req.isFinished())
		{
			respBody += req.readResponseBody();
			QTest::qWait(10);
		}
		respBody += req.readResponseBody();
		HttpHeaders respHeaders = req.responseHeaders();

		QCOMPARE(server->requestMethod, QByteArray("GET"));
		QCOMPARE(req.responseCode(), 304);
		QCOMPARE(req.responseReason(), QByteArray("Not Modified"));
		QVERIFY(respHeaders.contains("content-length"));
		QCOMPARE(respHeaders.get("content-length").toInt(), 12);
		server->closeAllRequests();
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

		QCOMPARE(server->requestMethod, QByteArray("POST"));
		QCOMPARE(req.responseCode(), 200);
		QCOMPARE(server->requestHeaders.get("Content-Length"), QByteArray("6"));
		server->closeAllRequests();
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

		QCOMPARE(server->requestMethod, QByteArray("POST"));
		QCOMPARE(req.responseCode(), 200);
		QCOMPARE(server->requestHeaders.get("Content-Length"), QByteArray("0"));
		server->closeAllRequests();
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
		server->closeAllRequests();
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
		server->closeAllRequests();
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
		server->closeAllRequests();
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

		QCOMPARE(server->requestMethod, QByteArray("DELETE"));
		QCOMPARE(req.responseCode(), 200);
		QVERIFY(!server->requestHeaders.contains("Content-Length"));
		QVERIFY(!server->requestHeaders.contains("Transfer-Encoding"));
		server->closeAllRequests();
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

		QCOMPARE(server->requestMethod, QByteArray("DELETE"));
		QCOMPARE(req.responseCode(), 200);
		QVERIFY(!server->requestHeaders.contains("Content-Length"));
		QCOMPARE(server->requestHeaders.get("Transfer-Encoding"), QByteArray("chunked"));
		server->closeAllRequests();
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

		QCOMPARE(server->requestMethod, QByteArray("DELETE"));
		QCOMPARE(req.responseCode(), 200);
		QVERIFY(!server->requestHeaders.contains("Content-Length"));
		QVERIFY(!server->requestHeaders.contains("Transfer-Encoding"));
		server->closeAllRequests();
	} };

QTEST_MAIN(HttpRequestTest)
#include "httprequesttest.moc"
