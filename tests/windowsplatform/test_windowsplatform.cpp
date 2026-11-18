#include <QtTest>
#include <windows.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include "streaming/plankwin32cursor.h"
#include "streaming/input/windowspen.h"

class TestWindowsPlatform : public QObject {
    Q_OBJECT
private:
    std::vector<SDL_Window*> windows;
    static HWND handle(SDL_Window* window) {
        return static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    }
    static HWND findOverlay() {
        HWND found=nullptr;
        EnumThreadWindows(GetCurrentThreadId(), [](HWND window,LPARAM out)->BOOL {
            wchar_t name[80]{}; GetClassNameW(window,name,80);
            if (wcscmp(name,L"PlankViewerCursor")==0) *reinterpret_cast<HWND*>(out)=window;
            return TRUE;
        },reinterpret_cast<LPARAM>(&found));
        return found;
    }
private slots:
    void initTestCase() { QVERIFY(SDL_Init(SDL_INIT_VIDEO)); }
    void overlayIsOwnedClickThroughAndDoesNotActivate() {
        const HWND foreground=GetForegroundWindow();
        auto* window=SDL_CreateWindow("Fernweh isolated cursor test",640,480,SDL_WINDOW_HIDDEN);
        QVERIFY(window); windows.push_back(window);
        ShowWindow(handle(window),SW_SHOWNOACTIVATE);
        qInfo() << "driver" << SDL_GetCurrentVideoDriver() << "thread" << GetCurrentThreadId()
                << "owner thread" << GetWindowThreadProcessId(handle(window),nullptr)
                << "native visible" << IsWindowVisible(handle(window)) << "iconic" << IsIconic(handle(window));
        QVERIFY(IsWindow(handle(window)));
        QVERIFY(IsWindowVisible(handle(window)));
        bool visible=true;
        auto overlay=PlankWin32Cursor::create([&](SDL_Window*& target,int& x,int& y){target=window;x=100;y=120;return visible;});
        QVERIFY(overlay);
        overlay->update();
        HWND native=findOverlay(); QVERIFY(native);
        QCOMPARE(GetWindow(native,GW_OWNER),handle(window));
        const auto style=GetWindowLongPtrW(native,GWL_EXSTYLE);
        QVERIFY(style&WS_EX_NOACTIVATE); QVERIFY(style&WS_EX_TRANSPARENT); QVERIFY(style&WS_EX_LAYERED);
        QCOMPARE(GetForegroundWindow(),foreground);
        QCOMPARE(SendMessageW(native,WM_NCHITTEST,0,0),LRESULT(HTTRANSPARENT));
        visible=false;overlay->update();QVERIFY(!IsWindowVisible(native));
        overlay.reset();QVERIFY(!IsWindow(native));
    }
    void penAdapterCannotForwardWhenDenied() {
        int events=0;
        WindowsPenInput pen([]{return false;},[&](const auto&){++events;});
        pen.setWindows(windows);pen.poll();pen.reset();
        QVERIFY(!pen.handlesPenEvents()); QCOMPARE(events,0);
    }
    void cleanupTestCase() {
        for(auto* window:windows) SDL_DestroyWindow(window);
        SDL_Quit();
    }
};
QTEST_GUILESS_MAIN(TestWindowsPlatform)
#include "test_windowsplatform.moc"
