#pragma once
#include <SDL3/SDL.h>

namespace PlankWindow {
// Use SDL's show path so its hidden/window state stays synchronized. A
// secondary output must not take keyboard focus merely because it is shown.
inline bool showWithoutActivation(SDL_Window* window) {
    const auto flags = SDL_GetWindowFlags(window);
    const bool suspendFocus = !(flags & (SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_NOT_FOCUSABLE));
    if (suspendFocus && !SDL_SetWindowFocusable(window, false)) return false;
    const bool shown = SDL_ShowWindow(window);
    const bool restored = !suspendFocus || SDL_SetWindowFocusable(window, true);
    return shown && restored;
}
}
