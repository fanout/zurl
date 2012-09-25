#ifndef WORKER_H
#define WORKER_H

#include <QObject>

class QVariant;
class JDnsShared;
class AppConfig;

class Worker : public QObject
{
	Q_OBJECT

public:
	enum Mode
	{
		Single, // for REQ/REP
		Stream  // for PUSH/PUB
	};

	Worker(JDnsShared *dns, AppConfig *config, QObject *parent = 0);
	~Worker();

	QByteArray rid() const;

	void start(const QVariant &request, Mode mode);
	void write(const QVariant &request);

signals:
	void readyRead(const QByteArray &receiver, const QVariant &response);
	void finished();

private:
	class Private;
	friend class Private;
	Private *d;
};

#endif
