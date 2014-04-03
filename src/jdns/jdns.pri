# qmake project include file

QT *= network

DEFINES += JDNS_STATIC
INCLUDEPATH += $$PWD/include/jdns

windows:{
	LIBS += -lWs2_32 -lAdvapi32
}
unix:{
	#QMAKE_CFLAGS += -pedantic
}

HEADERS += \
	$$PWD/src/jdns/jdns_packet.h \
	$$PWD/src/jdns/jdns_mdnsd.h \
	$$PWD/src/jdns/jdns_p.h \
	$$PWD/include/jdns/jdns.h \
	$$PWD/src/qjdns/qjdns_sock.h \
	$$PWD/src/qjdns/qjdns_p.h \
	$$PWD/src/qjdns/qjdnsshared_p.h \
	$$PWD/include/jdns/qjdns.h \
	$$PWD/include/jdns/qjdnsshared.h \
	$$PWD/include/jdns/jdns_export.h

SOURCES += \
	$$PWD/src/jdns/jdns_util.c \
	$$PWD/src/jdns/jdns_packet.c \
	$$PWD/src/jdns/jdns_mdnsd.c \
	$$PWD/src/jdns/jdns_sys.c \
	$$PWD/src/jdns/jdns.c \
	$$PWD/src/qjdns/qjdns_sock.cpp \
	$$PWD/src/qjdns/qjdns.cpp \
	$$PWD/src/qjdns/qjdnsshared.cpp
