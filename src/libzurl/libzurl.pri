SRC_DIR = $$PWD/..
COMMON_DIR = $$SRC_DIR/common

QMAKE_CXXFLAGS += $$(CXXFLAGS)
QMAKE_CFLAGS += $$(CFLAGS)
QMAKE_LFLAGS += $$(LDFLAGS)

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
	$$SRC_DIR/addressresolver.h \
	$$SRC_DIR/verifyhost.h \
	$$SRC_DIR/httprequest.h \
	$$SRC_DIR/websocket.h

SOURCES += \
	$$SRC_DIR/addressresolver.cpp \
	$$SRC_DIR/verifyhost.cpp \
	$$SRC_DIR/httprequest.cpp \
	$$SRC_DIR/websocket.cpp

HEADERS += \
	$$SRC_DIR/appconfig.h \
	$$SRC_DIR/worker.h

SOURCES += \
	$$SRC_DIR/worker.cpp
