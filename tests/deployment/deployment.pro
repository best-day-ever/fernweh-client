QT += core testlib qml quick quickcontrols2
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
include(../../deployment/fernweh.pri)
macx: QMAKE_CXXFLAGS += -include arm_acle.h
SOURCES += test_deployment.cpp
SOURCES += ../../app/settings/streamingpreferences.cpp ../../app/settings/plankclientpolicy.cpp
HEADERS += ../../app/settings/streamingpreferences.h ../../app/settings/plankclientpolicy.h
INCLUDEPATH += ../../app
RESOURCES += $$FERNWEH_QML_RESOURCE
