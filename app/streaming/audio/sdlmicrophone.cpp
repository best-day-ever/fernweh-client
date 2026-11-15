#include "sdlmicrophone.h"
#include "microphoneopus.h"
#include <SDL3/SDL.h>
#include <cmath>

PlankSdlMicrophone::PlankSdlMicrophone()
{
    m_AudioInitialized = SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (m_AudioInitialized) {
        SDL_AudioSpec format {SDL_AUDIO_F32, PlankMicrophoneChannels, PlankMicrophoneRate};
        m_Stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &format, nullptr, nullptr);
    }
    if (!m_Stream || !SDL_ResumeAudioStreamDevice(m_Stream)) fail();
}

PlankSdlMicrophone::~PlankSdlMicrophone() { close(); }

void PlankSdlMicrophone::close()
{
    if (m_Stream) SDL_DestroyAudioStream(m_Stream);
    m_Stream = nullptr;
    if (m_AudioInitialized) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    m_AudioInitialized = false;
}

bool PlankSdlMicrophone::fail()
{
    m_Failed = true;
    close();
    return false;
}

bool PlankSdlMicrophone::valid() const { return m_Stream && !m_Failed; }

bool PlankSdlMicrophone::take(Packet& packet)
{
    if (!valid()) return false;
    const int available = SDL_GetAudioStreamAvailable(m_Stream);
    if (available < 0) return fail();
    constexpr int frameBytes = PlankMicrophoneChannels * int(sizeof(float));
    constexpr int packetBytes = PlankMicrophoneFrames * frameBytes;
    if (available > 6 * packetBytes) {
        // Drop stale speech rather than replaying the backlog after a stall.
        // Whole-packet gaps remain visible to the existing Opus reset policy.
        m_NextSample += std::uint64_t(available / packetBytes) * PlankMicrophoneFrames;
        if (!SDL_ClearAudioStream(m_Stream)) return fail();
        return false;
    }
    if (available < packetBytes) return false;
    packet = {};
    if (SDL_GetAudioStreamData(m_Stream, packet.samples, packetBytes) != packetBytes) return fail();
    for (const auto sample : packet.samples) if (!std::isfinite(sample)) return fail();
    packet.sampleTime = m_NextSample;
    m_NextSample += PlankMicrophoneFrames;
    return true;
}
