TEMPLATE = lib
CONFIG -= app_bundle
CONFIG += staticlib
QT -= gui
QT += network
TARGET = zurl
DESTDIR = ..

CONFIG += use_curl

MOC_DIR = $$OUT_PWD/_moc
OBJECTS_DIR = $$OUT_PWD/_obj

include($$OUT_PWD/../../conf.pri)
include(libzurl.pri)
