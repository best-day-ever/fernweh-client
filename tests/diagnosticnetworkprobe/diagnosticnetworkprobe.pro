QT += core network testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
SOURCES += test_diagnosticnetworkprobe.cpp ../../app/backend/diagnosticnetworkprobe.cpp
HEADERS += ../../app/backend/diagnosticnetworkprobe.h
INCLUDEPATH += ../../app
win32: LIBS += -liphlpapi
macx: QMAKE_RPATHDIR += $$system(pkg-config --variable=libdir openssl)
