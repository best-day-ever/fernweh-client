QT += core gui network qml quick quickcontrols2 testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = displaysetupcontroller
SOURCES += test_displaysetupcontroller.cpp \
    ../../app/gui/displaysetupcontroller.cpp \
    ../../app/backend/outputtopology.cpp \
    ../../app/backend/displayarrangement.cpp \
    ../../app/backend/displayplanner.cpp
HEADERS += ../../app/gui/displaysetupcontroller.h ../../app/backend/displaymonitor.h
INCLUDEPATH += ../../app ../../app/backend ../../app/gui
