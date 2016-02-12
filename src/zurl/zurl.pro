CONFIG += console
CONFIG -= app_bundle
QT -= gui
QT += network
TARGET = zurl
DESTDIR = ../..

CONFIG += use_curl

MOC_DIR = $$OUT_PWD/_moc
OBJECTS_DIR = $$OUT_PWD/_obj

LIBS += -L$$PWD/.. -lzurl
PRE_TARGETDEPS += $$PWD/../libzurl.a

include($$OUT_PWD/../../conf.pri)
include(zurl.pri)

unix:!isEmpty(BINDIR) {
	target.path = $$BINDIR
	INSTALLS += target
}
