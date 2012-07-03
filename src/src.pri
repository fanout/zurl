INCLUDEPATH += $$PWD/jdns
include(jdns/jdns.pri)

INCLUDEPATH += $$PWD/jdnsshared
HEADERS += $$PWD/jdnsshared/jdnsshared.h
SOURCES += $$PWD/jdnsshared/jdnsshared.cpp

INCLUDEPATH += $$PWD/qzmq/src
include(qzmq/src/src.pri)

HEADERS += \
	$$PWD/tnetstring.h \
	$$PWD/request.h \
	$$PWD/requestpacket.h \
	$$PWD/responsepacket.h \
	$$PWD/app.h

SOURCES += \
	$$PWD/tnetstring.cpp \
	$$PWD/request.cpp \
	$$PWD/requestpacket.cpp \
	$$PWD/responsepacket.cpp \
	$$PWD/app.cpp \
	$$PWD/main.cpp
