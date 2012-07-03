CONFIG += console
CONFIG -= app_bundle
QT -= gui
QT += network
TARGET = yurl
DESTDIR = ..

exists(../conf.pri):include(../conf.pri)
include(src.pri)
