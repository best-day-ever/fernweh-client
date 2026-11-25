QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
DEFINES += NOMINMAX
INCLUDEPATH += ../../app ../../libs/windows/include ../../libs/windows/include/x64
LIBS += -L$$PWD/../../libs/windows/lib/x64 -lSDL3 user32.lib gdi32.lib
SOURCES += test_windowsplatform.cpp ../../app/streaming/plankwin32cursor.cpp ../../app/streaming/input/windowspen.cpp
