/*
 * Copyright (C) 2013-2022 Fanout, Inc.
 *
 * This file is part of Zurl.
 *
 * $FANOUT_BEGIN_LICENSE:GPL$
 *
 * Zurl is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Zurl is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Alternatively, Zurl may be used under the terms of a commercial license,
 * where the commercial license agreement is provided with the software or
 * contained in a written agreement between you and Fanout. For further
 * information use the contact form at <https://fanout.io/enterprise/>.
 *
 * $FANOUT_END_LICENSE$
 */

#include <QTcpSocket>
#include <QTcpServer>
#include <QtTest/QtTest>
#include "log.h"
#include "httpheaders.h"
#include "websocket.h"

class WebSocketServer : public QObject
{
	Q_OBJECT

public:
	QTcpServer *server;
	QTcpSocket *sock;
	bool requestParsed;

	WebSocketServer(QObject *parent = 0) :
		QObject(parent),
		server(0),
		sock(0),
		requestParsed(false)
	{
	}

	bool listen()
	{
		server = new QTcpServer(this);
		connect(server, &QTcpServer::newConnection, this, &WebSocketServer::server_newConnection);
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
		Q_UNUSED(method);

		if(uri == "/")
		{
			sock->write("HTTP/1.1 101 Switching Protocols\r\nHeaderA: ValueA\r\nHeaderB: ValueB\r\n\r\n");
			sock->disconnectFromHost();
		}
		else if(uri == "/fail")
		{
			sock->write("HTTP/1.1 400 OK\r\nContent-Length: 19\r\n\r\nFailed negotiation\n");
			sock->disconnectFromHost();
		}
		else if(uri == "/fail-nocontent")
		{
			sock->write("HTTP/1.1 400 OK\r\nContent-Length: 0\r\n\r\n");
			sock->disconnectFromHost();
		}
		else if(uri == "/fail-chunked")
		{
			QByteArray body = "Failed negotiation\n";
			QByteArray buf = "HTTP/1.1 400 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
			buf += QByteArray::number(body.size(), 16).toUpper() + "\r\n" + body + "\r\n";
			buf += QByteArray::number(0, 16).toUpper() + "\r\n\r\n";
			sock->write(buf);
			sock->disconnectFromHost();
		}
		else if(uri == "/fail-indefinite")
		{
			sock->write("HTTP/1.0 400 OK\r\nContent-Type: text/plain\r\n\r\nFailed negotiation\n");
			sock->disconnectFromHost();
		}
		else
			sock->disconnectFromHost();
	}

private slots:
	void server_newConnection()
	{
		sock = server->nextPendingConnection();
		connect(sock, &QTcpSocket::readyRead, this, &WebSocketServer::sock_readyRead);
		connect(sock, &QTcpSocket::disconnected, this, &WebSocketServer::sock_disconnected);
	}

	void sock_readyRead()
	{
		if(!requestParsed)
		{
			if(sock->canReadLine())
			{
				QByteArray line = sock->readLine();
				line.truncate(line.count() - 1);
				int end = line.indexOf(' ');
				QByteArray method = line.mid(0, end);
				int start = end + 1;
				end = line.indexOf(' ', start);
				QByteArray uri = line.mid(start, end - start);
				requestParsed = true;

				handleRequest(method, uri);
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
		requestParsed = false;
	}
};

class WebSocketTest : public QObject
{
	Q_OBJECT

private:
	WebSocketServer *server;

	void waitForSignal(QSignalSpy *spy)
	{
		while(spy->isEmpty())
			QTest::qWait(10);
	}

private slots:
	void initTestCase()
	{
		log_setOutputLevel(LOG_LEVEL_INFO);

		server = new WebSocketServer(this);
		server->listen();
	}

	void cleanupTestCase()
	{
		delete server;
	}

	void handshakeDnsError()
	{
		WebSocket sock;
		QSignalSpy spy(&sock, SIGNAL(error()));
		sock.start(QString("http://nosuchhost:%1/").arg(server->localPort()));
		waitForSignal(&spy);

		QCOMPARE(sock.errorCondition(), WebSocket::ErrorConnect);
	}

	void handshakeConnectError()
	{
		WebSocket sock;
		QSignalSpy spy(&sock, SIGNAL(error()));
		sock.start(QString("http://localhost:1/"));
		waitForSignal(&spy);

		QCOMPARE(sock.errorCondition(), WebSocket::ErrorConnect);
	}

	void handshakeSuccess()
	{
		WebSocket sock;
		QSignalSpy spy(&sock, SIGNAL(connected()));
		sock.start(QString("http://localhost:%1/").arg(server->localPort()), HttpHeaders());
		waitForSignal(&spy);

		HttpHeaders respHeaders = sock.responseHeaders();

		QCOMPARE(sock.responseCode(), 101);
		QCOMPARE(sock.responseReason(), QByteArray("Switching Protocols"));
		QCOMPARE(respHeaders.get("HeAdErA"), QByteArray("ValueA"));
	}

	void handshakeFail()
	{
		WebSocket sock;
		QSignalSpy spy(&sock, SIGNAL(error()));
		sock.start(QString("http://localhost:%1/fail").arg(server->localPort()), HttpHeaders());
		waitForSignal(&spy);

		QCOMPARE(sock.errorCondition(), WebSocket::ErrorRejected);
		QCOMPARE(sock.responseCode(), 400);
		QCOMPARE(sock.readResponseBody(), QByteArray("Failed negotiation\n"));
	}

	void handshakeFailNoContent()
	{
		WebSocket sock;
		QSignalSpy spy(&sock, SIGNAL(error()));
		sock.start(QString("http://localhost:%1/fail-nocontent").arg(server->localPort()), HttpHeaders());
		waitForSignal(&spy);

		QCOMPARE(sock.errorCondition(), WebSocket::ErrorRejected);
		QCOMPARE(sock.responseCode(), 400);
		QVERIFY(sock.readResponseBody().isEmpty());
	}

	void handshakeFailChunked()
	{
		WebSocket sock;
		QSignalSpy spy(&sock, SIGNAL(error()));
		sock.start(QString("http://localhost:%1/fail-chunked").arg(server->localPort()), HttpHeaders());
		waitForSignal(&spy);

		QCOMPARE(sock.errorCondition(), WebSocket::ErrorRejected);
		QCOMPARE(sock.responseCode(), 400);
		QCOMPARE(sock.readResponseBody(), QByteArray("Failed negotiation\n"));
	}

	void handshakeFailIndefinite()
	{
		WebSocket sock;
		QSignalSpy spy(&sock, SIGNAL(error()));
		sock.start(QString("http://localhost:%1/fail-indefinite").arg(server->localPort()), HttpHeaders());
		waitForSignal(&spy);

		QCOMPARE(sock.errorCondition(), WebSocket::ErrorRejected);
		QCOMPARE(sock.responseCode(), 400);
		QCOMPARE(sock.readResponseBody(), QByteArray("Failed negotiation\n"));
	}
};

QTEST_MAIN(WebSocketTest)
#include "websockettest.moc"
