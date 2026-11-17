#pragma once

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

namespace PlankSessionEvents {
// A visible native window does not guarantee that a Shown/resize notification
// is still queued. Bootstrap (and repair after fullscreen teardown) from the
// decoder's state instead. Do not enqueue the reset: a full queue must not
// strand video without a decoder, and existing control/close events stay intact.
inline bool waitForEvent(SDL_Event& event, int timeoutMs, SDL_Window* window,
                         bool decoderMissing)
{
    if (decoderMissing && window != nullptr &&
            !(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) {
        event = {};
        event.type = SDL_EVENT_RENDER_DEVICE_RESET;
        return true;
    }
    return SDL_WaitEventTimeout(&event, timeoutMs);
}

// SDL emits Quit only for the last visible top-level window. A stream can
// own several windows, so each owned Close request must end the session.
inline bool closeRequestsDisconnect(const SDL_Event& event, bool isPresentationWindow)
{
    return event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && isPresentationWindow;
}
}
