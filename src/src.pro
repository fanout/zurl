CONFIG += console
CONFIG -= app_bundle
QT -= gui
QT += network
TARGET = zurl
DESTDIR = ..

CONFIG += use_curl

include($$OUT_PWD/../conf.pri)
include(src.pri)

unix:!isEmpty(BINDIR) {
	target.path = $$BINDIR
	INSTALLS += target
}
