// Native sampling and system WinTab context adapted from Culley Bunker /
// SkulleyFX plank-client 8280fc48 (GPLv3). Fernweh retains its pen wire/state.
#include "windowspen.h"
#include "windowspenlogic.h"
#include <windows.h>
#include <SDL3/SDL_system.h>
#include <algorithm>
#include <deque>
#include <utility>

namespace Wintab {
using HCTX = HANDLE;
using WTPKT = DWORD;
using FIX32 = DWORD;
constexpr UINT WTI_DEFSYSCTX = 4;
constexpr UINT WTI_DEVICES = 100;
constexpr UINT DVC_NPRESSURE = 15;
constexpr UINT CXO_MESSAGES = 0x0004;
constexpr WTPKT PK_STATUS = 0x0002;
constexpr WTPKT PK_TIME = 0x0004;
constexpr WTPKT PK_CURSOR = 0x0020;
constexpr WTPKT PK_BUTTONS = 0x0040;
constexpr WTPKT PK_NORMAL_PRESSURE = 0x0400;
constexpr UINT TPS_PROXIMITY = 0x0001;
constexpr UINT WT_DEFBASE = 0x7FF0;
constexpr UINT WT_PACKET = WT_DEFBASE + 0;
constexpr UINT WT_PROXIMITY = WT_DEFBASE + 5;

struct AXIS {
    LONG axMin;
    LONG axMax;
    UINT axUnits;
    FIX32 axResolution;
};

struct LOGCONTEXTW {
    WCHAR lcName[40];
    UINT lcOptions;
    UINT lcStatus;
    UINT lcLocks;
    UINT lcMsgBase;
    UINT lcDevice;
    UINT lcPktRate;
    WTPKT lcPktData;
    WTPKT lcPktMode;
    WTPKT lcMoveMask;
    DWORD lcBtnDnMask;
    DWORD lcBtnUpMask;
    LONG lcInOrgX;
    LONG lcInOrgY;
    LONG lcInOrgZ;
    LONG lcInExtX;
    LONG lcInExtY;
    LONG lcInExtZ;
    LONG lcOutOrgX;
    LONG lcOutOrgY;
    LONG lcOutOrgZ;
    LONG lcOutExtX;
    LONG lcOutExtY;
    LONG lcOutExtZ;
    FIX32 lcSensX;
    FIX32 lcSensY;
    FIX32 lcSensZ;
    BOOL lcSysMode;
    int lcSysOrgX;
    int lcSysOrgY;
    int lcSysExtX;
    int lcSysExtY;
    FIX32 lcSysSensX;
    FIX32 lcSysSensY;
};

// Field order follows the PK_ bits, lowest first.
struct PACKET {
    UINT pkStatus;
    DWORD pkTime;
    UINT pkCursor;
    DWORD pkButtons;
    UINT pkNormalPressure;
};

using WTInfoWFn = UINT (WINAPI*)(UINT, UINT, LPVOID);
using WTOpenWFn = HCTX (WINAPI*)(HWND, LOGCONTEXTW*, BOOL);
using WTCloseFn = BOOL (WINAPI*)(HCTX);
using WTPacketFn = BOOL (WINAPI*)(HCTX, UINT, LPVOID);
using WTOverlapFn = BOOL (WINAPI*)(HCTX, BOOL);
using WTQueueSizeSetFn = BOOL (WINAPI*)(HCTX, int);


}

struct WindowsPenInput::Impl {
    CanForward allowed;
    Sink sink;
    std::vector<SDL_Window*> windows;
    std::deque<Sample> queue;
    WindowsPenLogic::FractionalPosition fractionX, fractionY;
    WindowsPenLogic::Authority authority;
    HANDLE device = nullptr;
    RECT deviceRect{}, displayRect{};
    HMODULE module = nullptr;
    Wintab::WTInfoWFn info = nullptr;
    Wintab::WTOpenWFn open = nullptr;
    Wintab::WTCloseFn close = nullptr;
    Wintab::WTPacketFn packet = nullptr;
    Wintab::WTOverlapFn overlap = nullptr;
    Wintab::WTQueueSizeSetFn queueSize = nullptr;
    Wintab::HCTX context = nullptr;
    HWND contextWindow = nullptr;
    float pressureMin = 0, pressureRange = 1, pressure = 0;
    bool pressureValid = false, waitForLift = false, driving = false, forwarded = false;
    Uint64 lastInk = 0, lastPacket = 0;
    float inkTiltX = 0, inkTiltY = 0;
    Uint32 inkPen = 1;
    unsigned buttons = 0;
    bool eraser = false;

    static HWND hwnd(SDL_Window* window) {
        return window ? static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                   SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) : nullptr;
    }
    SDL_Window* ownWindow(HWND handle) const {
        for (auto* window : windows) if (hwnd(window) == handle) return window;
        return nullptr;
    }
    SDL_Window* cursorWindow(float& x, float& y) const {
        POINT cursor;
        if (!GetCursorPos(&cursor)) return nullptr;
        const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
        for (auto* window : windows) {
            HWND handle = hwnd(window);
            if (!handle || handle != foreground) continue;
            RECT rect{}; POINT point = cursor;
            if (!GetClientRect(handle, &rect) || !ScreenToClient(handle, &point) || !PtInRect(&rect, point)) continue;
            x = static_cast<float>(point.x); y = static_cast<float>(point.y);
            toWindowPoints(window, x, y);
            return window;
        }
        return nullptr;
    }
    static void toWindowPoints(SDL_Window* window, float& x, float& y) {
        RECT client{}; int w = 0, h = 0;
        if (GetClientRect(hwnd(window), &client) && SDL_GetWindowSize(window, &w, &h) &&
                client.right > 0 && client.bottom > 0) {
            x *= float(w) / client.right; y *= float(h) / client.bottom;
        }
    }
    void enqueue(Sample sample) {
        if (!allowed()) return;
        if (queue.size() >= 256) {
            queue.clear(); Sample cancel; cancel.cancel = true; queue.push_back(cancel);
            waitForLift = true;
        }
        if (sample.pressure <= 0 && !sample.contact) waitForLift = false;
        if (waitForLift) { sample.contact = false; sample.pressure = 0; }
        queue.push_back(sample);
    }
    void closeContext() {
        if (context && close) close(context);
        context = nullptr; contextWindow = nullptr; pressureValid = false; driving = false;
    }
    void reset() {
        if (forwarded) { Sample cancel; cancel.cancel = true; forwarded = false; sink(cancel); }
        closeContext(); queue.clear(); authority.reset(); fractionX.reset(); fractionY.reset();
        device = nullptr; lastInk = 0; waitForLift = true;
    }
    void openContext(HWND handle) {
        if (!handle || handle == contextWindow ||
                (SDL_getenv("PLANK_WINTAB") && SDL_strcmp(SDL_getenv("PLANK_WINTAB"), "0") == 0)) return;
        closeContext();
        if (!module) {
            module = LoadLibraryExW(L"wintab32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!module) return;
            info = reinterpret_cast<Wintab::WTInfoWFn>(GetProcAddress(module, "WTInfoW"));
            open = reinterpret_cast<Wintab::WTOpenWFn>(GetProcAddress(module, "WTOpenW"));
            close = reinterpret_cast<Wintab::WTCloseFn>(GetProcAddress(module, "WTClose"));
            packet = reinterpret_cast<Wintab::WTPacketFn>(GetProcAddress(module, "WTPacket"));
            overlap = reinterpret_cast<Wintab::WTOverlapFn>(GetProcAddress(module, "WTOverlap"));
            queueSize = reinterpret_cast<Wintab::WTQueueSizeSetFn>(GetProcAddress(module, "WTQueueSizeSet"));
        }
        if (!info || !open || !close || !packet || !info(0, 0, nullptr)) return;
        Wintab::AXIS axis{};
        Wintab::LOGCONTEXTW config{};
        if (!info(Wintab::WTI_DEVICES, Wintab::DVC_NPRESSURE, &axis) || axis.axMax <= axis.axMin ||
                !info(Wintab::WTI_DEFSYSCTX, 0, &config)) return;
        config.lcOptions |= Wintab::CXO_MESSAGES;
        config.lcPktData = Wintab::PK_STATUS | Wintab::PK_TIME | Wintab::PK_CURSOR |
                           Wintab::PK_BUTTONS | Wintab::PK_NORMAL_PRESSURE;
        config.lcPktMode = 0; config.lcMoveMask = config.lcPktData;
        config.lcMsgBase = Wintab::WT_DEFBASE;
        context = open(handle, &config, TRUE);
        if (!context) return;
        contextWindow = handle; pressureMin = float(axis.axMin); pressureRange = float(axis.axMax - axis.axMin);
        if (queueSize) queueSize(context, 512);
        SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "Fernweh WinTab system context: %ld pressure levels", axis.axMax - axis.axMin + 1);
    }
    void wintabSample() {
        float x = 0, y = 0;
        SDL_Window* target = cursorWindow(x, y);
        const bool canDrive = authority.drive(SDL_GetTicks(), allowed() && target && pressureValid, pressure);
        // While Ink is active, WinTab still sends held-still pressure/contact
        // transitions through the same shared converter and pen identity.
        if (!target || !allowed() || !pressureValid) { driving = false; return; }
        if (!canDrive && (!lastInk || SDL_GetTicks() - lastInk >= 250)) return;
        driving = canDrive;
        Sample sample; sample.window = target; sample.pen = inkPen;
        sample.x = x; sample.y = y;
        sample.pressure = canDrive && !authority.acceptsContact() ? 0 : pressure;
        sample.contact = sample.pressure > 0;
        sample.tiltX = inkTiltX; sample.tiltY = inkTiltY;
        if (buttons & 2) sample.flags |= SDL_PEN_INPUT_BUTTON_1;
        if (buttons & 4) sample.flags |= SDL_PEN_INPUT_BUTTON_2;
        if (eraser) sample.flags |= SDL_PEN_INPUT_ERASER_TIP;
        enqueue(sample);
    }
    bool message(MSG* msg) {
        if (!allowed()) return true;
        if (context && msg->message == Wintab::WT_PACKET && reinterpret_cast<Wintab::HCTX>(msg->lParam) == context) {
            Wintab::PACKET data{};
            if (packet(context, UINT(msg->wParam), &data)) {
                lastPacket = SDL_GetTicks();
                pressureValid = (data.pkStatus & Wintab::TPS_PROXIMITY) == 0;
                pressure = std::clamp((float(data.pkNormalPressure) - pressureMin) / pressureRange, 0.0f, 1.0f);
                buttons = unsigned(data.pkButtons); eraser = data.pkCursor % 3 == 2;
                if (pressureValid) wintabSample();
                else { Sample sample; sample.pen = inkPen; sample.leave = true; enqueue(sample); }
            }
        } else if (context && msg->message == Wintab::WT_PROXIMITY &&
                   reinterpret_cast<Wintab::HCTX>(msg->wParam) == context && LOWORD(msg->lParam) == 0) {
            pressureValid = false; authority.reset();
            Sample sample; sample.pen = inkPen; sample.leave = true; enqueue(sample);
        }
        if (msg->message == WM_POINTERLEAVE) {
            if (ownWindow(msg->hwnd)) {
                Sample sample; sample.pen = inkPen; sample.leave = true; enqueue(sample);
                pressureValid = false;
            }
            return true;
        }
        if (msg->message != WM_POINTERENTER && msg->message != WM_POINTERDOWN &&
                msg->message != WM_POINTERUPDATE && msg->message != WM_POINTERUP) return true;
        SDL_Window* target = ownWindow(msg->hwnd);
        POINTER_PEN_INFO pen{};
        if (!target || !GetPointerPenInfo(GET_POINTERID_WPARAM(msg->wParam), &pen)) return true;
        lastInk = SDL_GetTicks(); authority.ink(lastInk); driving = false;
        inkPen = pen.pointerInfo.pointerId;
        POINT point = pen.pointerInfo.ptPixelLocation, origin{};
        if (!ScreenToClient(msg->hwnd, &point) || !ClientToScreen(msg->hwnd, &origin)) return true;
        Sample sample; sample.window = target; sample.pen = inkPen;
        sample.x = float(point.x); sample.y = float(point.y);
        RECT dev{}, display{};
        if (GetPointerDeviceRects(pen.pointerInfo.sourceDevice, &dev, &display) && dev.right > dev.left && dev.bottom > dev.top) {
            if (device != pen.pointerInfo.sourceDevice || !EqualRect(&dev, &deviceRect) || !EqualRect(&display, &displayRect)) {
                fractionX.reset(); fractionY.reset(); device = pen.pointerInfo.sourceDevice; deviceRect = dev; displayRect = display;
            }
            const double mappedX = display.left + double(pen.pointerInfo.ptHimetricLocation.x - dev.left) *
                    (display.right - display.left) / (dev.right - dev.left) - origin.x;
            const double mappedY = display.top + double(pen.pointerInfo.ptHimetricLocation.y - dev.top) *
                    (display.bottom - display.top) / (dev.bottom - dev.top) - origin.y;
            sample.x = fractionX.sample(sample.x, mappedX); sample.y = fractionY.sample(sample.y, mappedY);
        }
        toWindowPoints(target, sample.x, sample.y);
        if (msg->message == WM_POINTERUP && pressureValid && lastInk - lastPacket >= 250) pressureValid = false;
        sample.pressure = pressureValid ? pressure : (pen.penMask & PEN_MASK_PRESSURE ? float(pen.pressure) / 1024 : 1.0f);
        sample.contact = pressureValid ? pressure > 0 : (pen.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
        if (!sample.contact) sample.pressure = 0;
        sample.tiltX = inkTiltX = float(pen.tiltX); sample.tiltY = inkTiltY = float(pen.tiltY);
        if (pressureValid) {
            if (buttons & 2) sample.flags |= SDL_PEN_INPUT_BUTTON_1;
            if (buttons & 4) sample.flags |= SDL_PEN_INPUT_BUTTON_2;
        }
        if (pen.penFlags & PEN_FLAG_BARREL) sample.flags |= SDL_PEN_INPUT_BUTTON_1;
        if (pen.penFlags & (PEN_FLAG_INVERTED | PEN_FLAG_ERASER)) sample.flags |= SDL_PEN_INPUT_ERASER_TIP;
        enqueue(sample);
        return true;
    }
    static bool SDLCALL hook(void* opaque, MSG* msg) { return static_cast<Impl*>(opaque)->message(msg); }
};

WindowsPenInput::WindowsPenInput(CanForward allowed, Sink sink) : m_Impl(new Impl) {
    m_Impl->allowed = std::move(allowed); m_Impl->sink = std::move(sink);
    SDL_SetWindowsMessageHook(Impl::hook, m_Impl.get());
}
WindowsPenInput::~WindowsPenInput() {
    SDL_SetWindowsMessageHook(nullptr, nullptr);
    m_Impl->reset();
    if (m_Impl->module) FreeLibrary(m_Impl->module);
}
void WindowsPenInput::setWindows(std::vector<SDL_Window*> windows) {
    if (windows != m_Impl->windows) { reset(); m_Impl->windows = std::move(windows); }
}
void WindowsPenInput::reset() { m_Impl->reset(); }
bool WindowsPenInput::handlesPenEvents() const {
    return m_Impl->allowed() && (m_Impl->driving || (m_Impl->lastInk && SDL_GetTicks() - m_Impl->lastInk < 250));
}
void WindowsPenInput::poll() {
    auto& state = *m_Impl;
    if (!state.allowed()) { state.reset(); return; }
    float x = 0, y = 0;
    if (auto* target = state.cursorWindow(x, y)) state.openContext(Impl::hwnd(target));
    else { state.reset(); return; }
    if (state.driving && !WindowsPenLogic::ownsPointer(true, state.pressure, state.lastPacket, SDL_GetTicks())) state.driving = false;
    if (state.pressureValid && SDL_GetTicks() - state.lastPacket < 300 && !state.driving && (!state.lastInk || SDL_GetTicks() - state.lastInk >= 250)) state.wintabSample();
    while (!state.queue.empty()) {
        const auto sample = state.queue.front(); state.queue.pop_front();
        if (!state.allowed()) { state.reset(); return; }
        state.forwarded = !sample.leave && !sample.cancel;
        state.sink(sample);
    }
}

bool WindowsPenInput::ownsPointer() const {
    return m_Impl->allowed() && WindowsPenLogic::ownsPointer(m_Impl->driving, m_Impl->pressure,
                                                            m_Impl->lastPacket, SDL_GetTicks());
}
