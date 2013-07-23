COMMON_DIR = $$PWD/common

INCLUDEPATH += $$PWD/jdns
include(jdns/jdns.pri)

INCLUDEPATH += $$PWD/jdnsshared
HEADERS += $$PWD/jdnsshared/jdnsshared.h
SOURCES += $$PWD/jdnsshared/jdnsshared.cpp

INCLUDEPATH += $$PWD/qzmq/src
include(qzmq/src/src.pri)

INCLUDEPATH += $$COMMON_DIR
DEFINES += NO_IRISNET

HEADERS += \
	$$COMMON_DIR/processquit.h \
	$$COMMON_DIR/tnetstring.h \
	$$COMMON_DIR/httpheaders.h \
	$$COMMON_DIR/zhttprequestpacket.h \
	$$COMMON_DIR/zhttpresponsepacket.h \
	$$COMMON_DIR/bufferlist.h \
	$$COMMON_DIR/log.h

SOURCES += \
	$$COMMON_DIR/processquit.cpp \
	$$COMMON_DIR/tnetstring.cpp \
	$$COMMON_DIR/httpheaders.cpp \
	$$COMMON_DIR/zhttprequestpacket.cpp \
	$$COMMON_DIR/zhttpresponsepacket.cpp \
	$$COMMON_DIR/bufferlist.cpp \
	$$COMMON_DIR/log.cpp

HEADERS += \
	$$PWD/httprequest.h

use_curl {
	DEFINES += USE_CURL
	SOURCES += $$PWD/httprequest_curl.cpp
} else {
	DEFINES += USE_QNAM
	SOURCES += $$PWD/httprequest_qnam.cpp
}

HEADERS += \
	$$PWD/appconfig.h \
	$$PWD/worker.h \
	$$PWD/app.h

SOURCES += \
	$$PWD/worker.cpp \
	$$PWD/app.cpp \
	$$PWD/main.cpp
