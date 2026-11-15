QT += core testlib
QT -= gui
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = windowsresources
SOURCES += test_windowsresources.cpp
RESOURCES += ../../app/resources.qrc
