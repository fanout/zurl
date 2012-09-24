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
	$$PWD/yurlrequestpacket.h \
	$$PWD/yurlresponsepacket.h \
	$$PWD/httprequest.h \
	$$PWD/worker.h \
	$$PWD/appconfig.h \
	$$PWD/app.h

SOURCES += \
	$$PWD/tnetstring.cpp \
	$$PWD/httpheaders.cpp \
	$$PWD/yurlrequestpacket.cpp \
	$$PWD/yurlresponsepacket.cpp \
	$$PWD/httprequest.cpp \
	$$PWD/worker.cpp \
	$$PWD/app.cpp \
	$$PWD/main.cpp
