#include "input.h"
#include "streaming/session.h"
#include <Limelight.h>
#include <chrono>

void SdlInputHandler::initializeImmersiveMouse()
{
    m_ImmersiveMouse = std::make_unique<PlankImmersiveMouse>(PlankImmersiveMouse::Hooks{
        [this] { return beginNativePointerCapture(); },
        [this](QPointF anchor, bool reanchor) { finishNativePointerCapture(anchor, reanchor); },
        [this](const unsigned char* bytes, unsigned length) {
            std::lock_guard<std::mutex> lock(m_PointerBridge->wire);
            return LiSendPlankPointerCapture(bytes, length) == 0;
        },
        [this](QPointF point, QSize size) { return sendPointerAbsolute(point, size); },
        [this] { releaseMouseButtons(); },
        [] {
            SDL_LogError(SDL_LOG_CATEGORY_INPUT, "Immersive mouse synchronization failed; ending the stream");
            LiNotifyPlankHostTermination(-1);
        }
    });
}

void SdlInputHandler::setPointerCaptureSupported(bool supported)
{
#ifdef Q_OS_WIN
    m_PointerCaptureSupported = supported;
#else
    Q_UNUSED(supported);
    m_PointerCaptureSupported = false; // The ordinary Mac product uses its Qt adapter.
#endif
    if (!m_PointerCaptureSupported) cancelImmersiveMouse();
}

bool SdlInputHandler::immersiveMouseAvailable() const
{
    return m_PointerCaptureSupported && !m_ViewOnly;
}

void SdlInputHandler::toggleImmersiveMouse()
{
    if (!m_ImmersiveMouse) return;
    if (!immersiveMouse() && immersiveMouseAvailable()) setToolbarInteractionActive(false);
    SDL_Window* window = SDL_GetKeyboardFocus();
    if (!window || !presentationOutput(window)) window = m_Window;
    const QSize dimensions = m_PresentationLayout.usesSourceRects() ?
                m_PresentationLayout.desktopSize : streamDimensions();
    QPointF anchor(dimensions.width() / 2.0, dimensions.height() / 2.0);
    float x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    if (m_PresentationLayout.usesSourceRects()) mapWindowPointToDesktop(window, x, y, anchor, true);
    else {
        PlankOutputGeometry geometry;
        if (outputGeometry(window, geometry))
            PlankPresentation::mapWindowPointToStream(QPointF(x, y), geometry, dimensions, anchor, true);
    }
    if (!immersiveMouseOwnsPointer()) m_PointerWindow = window;
    const bool focused = window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS);
    const bool allowed = immersiveMouseAvailable() && focused && isCaptureActive() &&
            !m_TabletCursorActive && !m_TabletCursorActivationPending.load() &&
            m_MouseButtonsDown.isEmpty() && SDL_GetMouseState(nullptr, nullptr) == 0;
    if (!m_ImmersiveMouse->toggle(allowed, dimensions, anchor, SDL_GetTicks())) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "Mouse capture unavailable: focus the stream, move the mouse and release its buttons");
    } else SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "Immersive mouse %s",
                       immersiveMouse() ? "enabled (Ctrl+Alt+Shift+M releases)" : "released");
}

bool SdlInputHandler::beginNativePointerCapture()
{
#ifdef Q_OS_WIN
    std::unique_lock<std::mutex> wire(m_PointerBridge->wire, std::try_to_lock);
    if (!wire.owns_lock() || m_PointerBridge->tabletRequested.load() ||
            m_TabletCursorActivationPending.load() || m_TabletCursorActive || !m_PointerWindow ||
            !(SDL_GetWindowFlags(m_PointerWindow) & SDL_WINDOW_INPUT_FOCUS)) return false;
    if (m_PointerBridge->captured.load()) return true;
    const auto hint = [this](const char* name, const char* value) {
        const char* previous = SDL_GetHint(name);
        m_PointerHints.push_back({name, previous != nullptr, previous ? previous : ""});
        return SDL_SetHintWithPriority(name, value, SDL_HINT_OVERRIDE);
    };
    // Raw Windows motion stays unaccelerated and in its original pixel units,
    // independent of OS pointer speed, renderer scale and absolute pointing.
    bool ready = hint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0") &&
            hint(SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE, "1") &&
            hint(SDL_HINT_MOUSE_RELATIVE_WARP_MOTION, "0");
#ifdef SDL_HINT_MOUSE_RELATIVE_SCALING
    ready = hint(SDL_HINT_MOUSE_RELATIVE_SCALING, "0") && ready;
#endif
#ifdef SDL_HINT_MOUSE_RELATIVE_MODE_WARP
    ready = hint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0") && ready;
#endif
    if (!ready || !SDL_SetWindowRelativeMouseMode(m_PointerWindow, true)) {
        finishNativePointerCapture({}, false); return false;
    }
    m_PointerDropBefore = SDL_GetTicksNS();
    {
        std::lock_guard<std::mutex> state(m_PointerBridge->state);
        m_PointerBridge->captured.store(true);
    }
    return true;
#else
    return false;
#endif
}

void SdlInputHandler::finishNativePointerCapture(QPointF anchor, bool reanchor)
{
    SDL_Window* target = m_PointerWindow;
    QPointF local;
    bool mapped = false;
    if (reanchor && target && (SDL_GetWindowFlags(target) & SDL_WINDOW_INPUT_FOCUS)) {
        for (const auto& output : m_PresentationLayout.outputs) {
            int w, h, pw, ph;
            SDL_GetWindowSize(output.window, &w, &h);
            SDL_GetWindowSizeInPixels(output.window, &pw, &ph);
            if (m_PresentationLayout.usesSourceRects()) {
                if (!QRectF(output.desktopRect).contains(anchor)) continue;
                const QPointF frame(output.sourceRect.left() + (anchor.x() - output.desktopRect.left()) *
                                    output.sourceRect.width() / output.desktopRect.width(),
                                    output.sourceRect.top() + (anchor.y() - output.desktopRect.top()) *
                                    output.sourceRect.height() / output.desktopRect.height());
                mapped = PlankPresentation::mapStreamPointToSourceWindow(frame, streamDimensions(), output,
                            QSize(w, h), QSize(pw, ph), local);
            } else {
                PlankOutputGeometry geometry;
                mapped = outputGeometry(output.window, geometry) &&
                        PlankPresentation::mapStreamPointToWindow(anchor, streamDimensions(), geometry, local);
            }
            if (mapped) { target = output.window; break; }
        }
    }
    if (mapped && target == m_PointerWindow) SDL_WarpMouseInWindow(target, float(local.x()), float(local.y()));
    if (m_PointerWindow) SDL_SetWindowRelativeMouseMode(m_PointerWindow, false);
    if (mapped && target != m_PointerWindow) SDL_WarpMouseInWindow(target, float(local.x()), float(local.y()));
    // SDL flushes relative motion on mode change. Timestamp filtering also
    // rejects motion already dequeued before the authoritative reanchor.
    m_PointerDropBefore = SDL_GetTicksNS();
    for (auto it = m_PointerHints.rbegin(); it != m_PointerHints.rend(); ++it) {
        if (it->existed) SDL_SetHintWithPriority(it->name, it->value.c_str(), SDL_HINT_OVERRIDE);
        else SDL_ResetHint(it->name);
    }
    m_PointerHints.clear();
    {
        std::lock_guard<std::mutex> state(m_PointerBridge->state);
        m_PointerBridge->captured.store(false);
    }
    m_PointerBridge->changed.notify_all();
    updatePointerRegionLock();
}

void SdlInputHandler::cancelImmersiveMouse()
{
    if (m_ImmersiveMouse) m_ImmersiveMouse->cancel();
}

void SdlInputHandler::handlePointerCapture(const unsigned char* data, unsigned length)
{
    {
        std::lock_guard<std::mutex> lock(m_PointerAckMutex);
        if (length != PLANK_POINTER_PACKET_SIZE || m_PointerAcks.size() >= 8) m_PointerAckOverflow = true;
        else m_PointerAcks.emplace_back(data, data + length);
    }
    Session::postPointerCaptureEvent();
}

void SdlInputHandler::pollImmersiveMouse()
{
#ifdef Q_OS_WIN
    refreshNormalizedPen();
    pollWindowsPen();
    updateTabletCursorVisibility();
#endif
    if (!m_ImmersiveMouse) return;
    if (m_PointerBridge->tabletRequested.exchange(false)) {
        cancelImmersiveMouse();
        m_TabletCursorActive = true;
        m_TabletCursorActivationPending.store(true);
        applyPendingTabletCursorActivation();
    }
    std::vector<std::vector<unsigned char>> acks;
    bool overflow;
    {
        std::lock_guard<std::mutex> lock(m_PointerAckMutex);
        acks = std::move(m_PointerAcks); m_PointerAcks.clear();
        overflow = std::exchange(m_PointerAckOverflow, false);
    }
    if (overflow) m_ImmersiveMouse->acknowledge(nullptr, 0);
    for (const auto& ack : acks) m_ImmersiveMouse->acknowledge(ack.data(), unsigned(ack.size()));
    m_ImmersiveMouse->poll(SDL_GetTicks());
}

bool SdlInputHandler::sendPointerAbsolute(QPointF position, QSize dimensions)
{
    return LiSendMousePositionEvent(short(qRound(position.x())), short(qRound(position.y())),
                                   short(dimensions.width()), short(dimensions.height())) == 0;
}

void SdlInputHandler::sendMouseButton(int button, bool down)
{
    if (down) m_MouseButtonsDown.insert(button);
    else if (!m_MouseButtonsDown.remove(button)) return;
    if (LiSendMouseButtonEvent(down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, button) != 0 &&
            m_MousePressFailureLogs++ < 3) SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Remote mouse button could not be queued");
}

void SdlInputHandler::releaseMouseButtons()
{
    for (int button : std::as_const(m_MouseButtonsDown)) LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
    m_MouseButtonsDown.clear();
}

bool SdlInputHandler::deferMouseKey(const SDL_KeyboardEvent& event)
{
    return m_ImmersiveMouse && m_ImmersiveMouse->defer([this, event](QPointF) {
        auto copy = event; handleKeyEvent(&copy);
    });
}
