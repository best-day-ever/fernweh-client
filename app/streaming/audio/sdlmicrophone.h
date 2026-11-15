#pragma once

#include "microphonecapturequeue.h"

struct SDL_AudioStream;

// The Windows capture adapter retains PMIC v2's sample-count contract. It
// deliberately supplies no native capture clock and is not eligible for v3.
class PlankSdlMicrophone
{
public:
    using Packet = PlankMicrophoneCaptureQueue::Packet;
    PlankSdlMicrophone();
    ~PlankSdlMicrophone();
    bool valid() const;
    bool take(Packet& packet);

private:
    void close();
    bool fail();
    SDL_AudioStream* m_Stream = nullptr;
    std::uint64_t m_NextSample = 0;
    bool m_AudioInitialized = false;
    bool m_Failed = false;
};
