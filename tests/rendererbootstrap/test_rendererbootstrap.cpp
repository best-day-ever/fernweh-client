#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <QtTest>
#include "../../app/streaming/planksessionevents.h"

class TestRendererBootstrap : public QObject
{
    Q_OBJECT
    SDL_Window* m_Window = nullptr;
private slots:
    void initTestCase()
    {
        SDL_SetMainReady();
        QVERIFY2(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
    }
    void init()
    {
        m_Window = SDL_CreateWindow("Renderer startup regression", 320, 180, SDL_WINDOW_HIDDEN);
        QVERIFY2(m_Window, SDL_GetError());
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    }
    void cleanup() { SDL_DestroyWindow(m_Window); m_Window = nullptr; }
    void cleanupTestCase() { SDL_Quit(); }
    void startsWithoutShownOrResizeNotification()
    {
        SDL_Event event {};
        // The former event-only startup has nothing to dispatch in this state.
        QVERIFY(!SDL_WaitEventTimeout(&event, 1));
        QVERIFY(PlankSessionEvents::waitForEvent(event, 1, m_Window, true));
        QCOMPARE(event.type, Uint32(SDL_EVENT_RENDER_DEVICE_RESET));
        // The real SDL renderer can initialize and present on that same surface.
        SDL_Renderer* renderer = SDL_CreateRenderer(m_Window, nullptr);
        QVERIFY2(renderer, SDL_GetError());
        QVERIFY(SDL_SetRenderDrawColor(renderer, 17, 89, 151, 255));
        QVERIFY(SDL_RenderClear(renderer));
        SDL_Surface* pixels = SDL_RenderReadPixels(renderer, nullptr);
        QVERIFY2(pixels, SDL_GetError());
        Uint8 red = 0, green = 0, blue = 0, alpha = 0;
        const bool readable = SDL_ReadSurfacePixel(pixels, 0, 0, &red, &green, &blue, &alpha);
        SDL_DestroySurface(pixels);
        QVERIFY(readable);
        QCOMPARE(red, Uint8(17));
        QCOMPARE(green, Uint8(89));
        QCOMPARE(blue, Uint8(151));
        QVERIFY(SDL_RenderPresent(renderer));
        SDL_DestroyRenderer(renderer);
    }
    void fullscreenTeardownDoesNotNeedAnotherResize()
    {
        SDL_Event event {};
        QVERIFY(PlankSessionEvents::waitForEvent(event, 1, m_Window, true));
        QCOMPARE(event.type, Uint32(SDL_EVENT_RENDER_DEVICE_RESET));
        // Once a decoder exists, ordinary event waiting resumes.
        QVERIFY(!PlankSessionEvents::waitForEvent(event, 1, m_Window, false));
        // Fullscreen teardown drops the decoder; missing resize is recoverable.
        QVERIFY(PlankSessionEvents::waitForEvent(event, 1, m_Window, true));
        QCOMPARE(event.type, Uint32(SDL_EVENT_RENDER_DEVICE_RESET));
    }
    void bootstrapPreservesPendingCloseAndControl()
    {
        for (auto type : {SDL_EVENT_QUIT, SDL_EVENT_USER}) {
            SDL_Event queued {};
            queued.type = type;
            queued.user.code = 1234;
            QVERIFY(SDL_PushEvent(&queued));
            SDL_Event event {};
            QVERIFY(PlankSessionEvents::waitForEvent(event, 1, m_Window, true));
            QCOMPARE(event.type, Uint32(SDL_EVENT_RENDER_DEVICE_RESET));
            QVERIFY(PlankSessionEvents::waitForEvent(event, 1, m_Window, false));
            QCOMPARE(event.type, Uint32(type));
            QCOMPARE(event.user.code, 1234);
        }
    }
    void aFullQueueCannotLoseBootstrap()
    {
        SDL_Event queued {};
        queued.type = SDL_EVENT_USER;
        int count = 0;
        while (count < 100000 && SDL_PushEvent(&queued)) ++count;
        QVERIFY(count > 0 && count < 100000);
        SDL_Event event {};
        QVERIFY(PlankSessionEvents::waitForEvent(event, 1, m_Window, true));
        QCOMPARE(event.type, Uint32(SDL_EVENT_RENDER_DEVICE_RESET));
        QCOMPARE(SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_USER, SDL_EVENT_USER), count);
    }
    void missingWindowAndReconnectKeepWaiting()
    {
        SDL_Event event {};
        QVERIFY(!PlankSessionEvents::waitForEvent(event, 1, nullptr, true));
        // The caller suppresses decoder startup during transport reconnect.
        QVERIFY(!PlankSessionEvents::waitForEvent(event, 1, m_Window, false));
    }
    void rejectedBarrierDoesNotSuppressFutureWindows()
    {
        int pending = 0;
        SDL_Event queued {};
        queued.type = SDL_EVENT_USER;
        while (SDL_PushEvent(&queued)) {}
        QVERIFY(!PlankSessionEvents::queueWindowEventBarrier(pending, 100));
        QCOMPARE(pending, 0);
        SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
        SDL_SetEventFilter([](void*, SDL_Event*) { return false; }, nullptr);
        QVERIFY(!PlankSessionEvents::queueWindowEventBarrier(pending, 100));
        QCOMPARE(pending, 0);
        SDL_SetEventFilter(nullptr, nullptr);
        QVERIFY(PlankSessionEvents::queueWindowEventBarrier(pending, 100));
        QCOMPARE(pending, 1);
        SDL_Event event {};
        QVERIFY(SDL_WaitEventTimeout(&event, 50));
        QCOMPARE(event.user.code, 100);
        PlankSessionEvents::completeWindowEventBarrier(pending);
        QCOMPARE(pending, 0);
        // A stale/duplicate event must not make future counts negative.
        PlankSessionEvents::completeWindowEventBarrier(pending);
        QCOMPARE(pending, 0);
        QVERIFY(PlankSessionEvents::queueWindowEventBarrier(pending, 100));
        QVERIFY(PlankSessionEvents::queueWindowEventBarrier(pending, 100));
        QCOMPARE(pending, 2);
        PlankSessionEvents::completeWindowEventBarrier(pending);
        QCOMPARE(pending, 1);
        PlankSessionEvents::completeWindowEventBarrier(pending);
        QCOMPARE(pending, 0);
    }
    void reconnectDrainReleasesQueuedBarrier()
    {
        int pending = 0;
        QVERIFY(PlankSessionEvents::queueWindowEventBarrier(pending, 100));
        SDL_Event event {};
        QVERIFY(SDL_WaitEventTimeout(&event, 50));
        QCOMPARE(event.type, Uint32(SDL_EVENT_USER));
        // This is the shared local dispatch used before reconnect discards
        // unrelated events, and by normal playback. It consumes the same marker.
        QVERIFY(PlankSessionEvents::dispatchWindowEventBarrier(event.user, pending, 100));
        QCOMPARE(pending, 0);
        QVERIFY(PlankSessionEvents::queueWindowEventBarrier(pending, 100));
        SDL_UserEvent unrelated {};
        unrelated.code = 101;
        QVERIFY(!PlankSessionEvents::dispatchWindowEventBarrier(unrelated, pending, 100));
        QCOMPARE(pending, 1);
        QVERIFY(SDL_WaitEventTimeout(&event, 50));
        QVERIFY(PlankSessionEvents::dispatchWindowEventBarrier(event.user, pending, 100));
        QCOMPARE(pending, 0);
    }
};

QTEST_APPLESS_MAIN(TestRendererBootstrap)
#include "test_rendererbootstrap.moc"
