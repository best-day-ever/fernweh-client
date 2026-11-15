QT += core testlib
QT -= gui
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = sdlmicrophone
SOURCES += test_sdlmicrophone.cpp ../../app/streaming/audio/sdlmicrophone.cpp
INCLUDEPATH += ../../app
win32 {
    INCLUDEPATH += ../../libs/windows/include ../../libs/windows/include/x64 ../../libs/windows/include/x64/SDL3
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += sdl3 opus
}
