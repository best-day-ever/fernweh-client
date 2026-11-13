QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
include(../../deployment/fernweh.pri)
macx: QMAKE_CXXFLAGS += -include arm_acle.h
SOURCES += test_deployment.cpp
