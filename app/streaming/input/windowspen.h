#pragma once
#include <SDL3/SDL.h>
#include <functional>
#include <memory>
#include <vector>

// Native Win32/WinTab adapter. All forwarding still goes through the shared
// normalized-pen state machine. Raw HID/role/focus policy stays with the caller.
class WindowsPenInput {
public:
    struct Sample {
        SDL_Window* window = nullptr;
        Uint32 pen = 0;
        float x = 0, y = 0, pressure = 0, tiltX = 0, tiltY = 0;
        Uint32 flags = 0;
        bool contact = false, leave = false, cancel = false;
    };
    using CanForward = std::function<bool()>;
    using Sink = std::function<void(const Sample&)>;
    WindowsPenInput(CanForward allowed, Sink sink);
    ~WindowsPenInput();
    void setWindows(std::vector<SDL_Window*> windows);
    void poll();
    void reset();
    bool handlesPenEvents() const;
    bool ownsPointer() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
