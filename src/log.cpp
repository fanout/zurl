#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <QString>
#include <QTime>

static int g_level = LOG_LEVEL_DEBUG;
static QTime g_time;

static void log(int level, const char *fmt, va_list ap)
{
	if(!g_time.isValid())
		g_time.start();

	if(level <= g_level)
	{
		QString str;
		str.vsprintf(fmt, ap);

		const char *lstr;
		switch(level)
		{
			case LOG_LEVEL_ERROR:   lstr = "ERR"; break;
			case LOG_LEVEL_WARNING: lstr = "WARN"; break;
			case LOG_LEVEL_INFO:    lstr = "INFO"; break;
			case LOG_LEVEL_DEBUG:
			default:
				lstr = "DEBUG"; break;
		}

		QTime t(0, 0);
		t = t.addMSecs(g_time.elapsed());
		fprintf(stderr, "[%s] %s %s\n", lstr, qPrintable(t.toString("HH:mm:ss.zzz")), qPrintable(str));
	}
}

void log_setOutputLevel(int level)
{
	g_level = level;
}

void log_error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	log(LOG_LEVEL_ERROR, fmt, ap);
	va_end(ap);
}

void log_warning(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	log(LOG_LEVEL_WARNING, fmt, ap);
	va_end(ap);
}

void log_info(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	log(LOG_LEVEL_INFO, fmt, ap);
	va_end(ap);
}

void log_debug(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	log(LOG_LEVEL_DEBUG, fmt, ap);
	va_end(ap);
}
