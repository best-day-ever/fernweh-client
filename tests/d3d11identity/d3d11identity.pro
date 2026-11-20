QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
SOURCES += test_d3d11identity.cpp
INCLUDEPATH += ../../app
DEFINES += NOMINMAX
LIBS += d3d11.lib d3dcompiler.lib
