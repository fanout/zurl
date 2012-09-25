INCLUDEPATH += $$PWD/jdns
include(jdns/jdns.pri)

INCLUDEPATH += $$PWD/jdnsshared
HEADERS += $$PWD/jdnsshared/jdnsshared.h
SOURCES += $$PWD/jdnsshared/jdnsshared.cpp

INCLUDEPATH += $$PWD/qzmq/src
include(qzmq/src/src.pri)

HEADERS += \
	$$PWD/tnetstring.h \
	$$PWD/httpheaders.h \
	$$PWD/zurlrequestpacket.h \
	$$PWD/zurlresponsepacket.h \
	$$PWD/httprequest.h \
	$$PWD/log.h \
	$$PWD/appconfig.h \
	$$PWD/worker.h \
	$$PWD/app.h

SOURCES += \
	$$PWD/tnetstring.cpp \
	$$PWD/httpheaders.cpp \
	$$PWD/zurlrequestpacket.cpp \
	$$PWD/zurlresponsepacket.cpp \
	$$PWD/httprequest.cpp \
	$$PWD/log.cpp \
	$$PWD/worker.cpp \
	$$PWD/app.cpp \
	$$PWD/main.cpp
