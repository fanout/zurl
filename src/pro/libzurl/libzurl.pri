SRC_DIR = $$PWD/../..
COMMON_DIR = $$SRC_DIR/common

include($$SRC_DIR/jdns/jdns.pri)

INCLUDEPATH += $$SRC_DIR/qzmq/src
include($$SRC_DIR/qzmq/src/src.pri)

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

INCLUDEPATH += $$SRC_DIR

HEADERS += \
	$$SRC_DIR/httprequest.h \
	$$SRC_DIR/websocket.h

use_curl {
	DEFINES += USE_CURL
	SOURCES += $$SRC_DIR/httprequest_curl.cpp
} else {
	DEFINES += USE_QNAM
	SOURCES += $$SRC_DIR/httprequest_qnam.cpp
}

SOURCES += \
	$$SRC_DIR/websocket.cpp

HEADERS += \
	$$SRC_DIR/appconfig.h \
	$$SRC_DIR/worker.h

SOURCES += \
	$$SRC_DIR/worker.cpp
