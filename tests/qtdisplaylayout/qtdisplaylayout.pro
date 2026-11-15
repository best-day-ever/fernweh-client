QT += core gui qml testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = qtdisplaylayout
SOURCES += test_qtdisplaylayout.cpp \
    ../../app/backend/outputtopology.cpp \
    ../../app/backend/displayarrangement.cpp \
    ../../app/backend/displayplanner.cpp \
    ../../app/streaming/plankpresentation.cpp
HEADERS += ../../app/streaming/qt/sessionlayout.h ../../app/streaming/qt/sessiongeometry.h
INCLUDEPATH += ../../app/backend ../../app
