QT += core testlib
QT -= gui
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
INCLUDEPATH += ../../../../protocol/plank-transport/include
SOURCES += test_immersivemouse.cpp
HEADERS += ../../app/streaming/input/immersivemouse.h
