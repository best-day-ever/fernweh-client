#include <QtTest>
#include <SDL3/SDL.h>
#include <cstring>
#include <limits>
#include <vector>
#include "streaming/audio/sdlmicrophone.h"
#include "streaming/audio/microphoneopus.h"

namespace {
struct FakeCapture {
    bool initialize = true, open = true, resume = true, read = true, available = true, clear = true;
    unsigned opens = 0, destroys = 0, quits = 0, clears = 0;
    std::vector<float> samples;
} capture;
constexpr auto PacketSamples = PlankMicrophoneFrames * PlankMicrophoneChannels;
void appendPacket() {
    for (int i = 0; i < PlankMicrophoneFrames; ++i) {
        capture.samples.push_back(0.25f);
        capture.samples.push_back(-0.5f);
    }
}
}

// Exact SDL capture calls are replaced. This fixture never loads a device or
// reads the builder's microphone, including on native Windows.
extern "C" {
bool SDLCALL SDL_InitSubSystem(SDL_InitFlags flags) {
    if (flags != SDL_INIT_AUDIO) qFatal("Unexpected SDL subsystem initialization");
    return capture.initialize;
}
void SDLCALL SDL_QuitSubSystem(SDL_InitFlags flags) {
    if (flags != SDL_INIT_AUDIO) qFatal("Unexpected SDL subsystem cleanup");
    ++capture.quits;
}
SDL_AudioStream* SDLCALL SDL_OpenAudioDeviceStream(SDL_AudioDeviceID device, const SDL_AudioSpec* format,
                                                   SDL_AudioStreamCallback callback, void* userdata) {
    if (device != SDL_AUDIO_DEVICE_DEFAULT_RECORDING || callback || userdata ||
            format->format != SDL_AUDIO_F32 || format->channels != PlankMicrophoneChannels ||
            format->freq != PlankMicrophoneRate) qFatal("Unexpected capture device or PCM contract");
    ++capture.opens;
    return capture.open ? reinterpret_cast<SDL_AudioStream*>(&capture) : nullptr;
}
bool SDLCALL SDL_ResumeAudioStreamDevice(SDL_AudioStream*) { return capture.resume; }
void SDLCALL SDL_DestroyAudioStream(SDL_AudioStream*) { ++capture.destroys; }
int SDLCALL SDL_GetAudioStreamAvailable(SDL_AudioStream*) {
    return capture.available ? int(capture.samples.size() * sizeof(float)) : -1;
}
int SDLCALL SDL_GetAudioStreamData(SDL_AudioStream*, void* buffer, int bytes) {
    if (!capture.read) return -1;
    std::memcpy(buffer, capture.samples.data(), std::size_t(bytes));
    capture.samples.erase(capture.samples.begin(), capture.samples.begin() + bytes / int(sizeof(float)));
    return bytes;
}
bool SDLCALL SDL_ClearAudioStream(SDL_AudioStream*) {
    ++capture.clears;
    capture.samples.clear();
    return capture.clear;
}
}

class SdlMicrophoneTest : public QObject
{
    Q_OBJECT
private slots:
    void init() { capture = {}; }
    void initializationFailure() {
        capture.initialize = false;
        PlankSdlMicrophone microphone;
        QVERIFY(!microphone.valid());
        QCOMPARE(capture.opens, 0u);
        QCOMPARE(capture.quits, 0u);
    }
    void deviceOpenFailure() {
        capture.open = false;
        PlankSdlMicrophone microphone;
        QVERIFY(!microphone.valid());
        QCOMPARE(capture.destroys, 0u);
        QCOMPARE(capture.quits, 1u);
    }
    void resumeFailureClosesImmediately() {
        capture.resume = false;
        PlankSdlMicrophone microphone;
        QVERIFY(!microphone.valid());
        QCOMPARE(capture.destroys, 1u);
        QCOMPARE(capture.quits, 1u);
    }
    void stereoPacketsAndSampleClock() {
        {
            PlankSdlMicrophone microphone;
            QVERIFY(microphone.valid());
            PlankSdlMicrophone::Packet packet;
            QVERIFY(!microphone.take(packet));
            appendPacket(); appendPacket();
            QVERIFY(microphone.take(packet));
            QCOMPARE(packet.sampleTime, std::uint64_t(0));
            QCOMPARE(packet.captureTimeNs, std::uint64_t(0));
            QCOMPARE(packet.samples[0], 0.25f);
            QCOMPARE(packet.samples[1], -0.5f);
            QVERIFY(microphone.take(packet));
            QCOMPARE(packet.sampleTime, std::uint64_t(PlankMicrophoneFrames));
            QVERIFY(!microphone.take(packet));
        }
        QCOMPARE(capture.destroys, 1u);
        QCOMPARE(capture.quits, 1u);
    }
    void partialPacketStaysQueued() {
        PlankSdlMicrophone microphone;
        capture.samples.assign(PacketSamples - 1, 0.5f);
        PlankSdlMicrophone::Packet packet;
        QVERIFY(!microphone.take(packet));
        QCOMPARE(capture.samples.size(), std::size_t(PacketSamples - 1));
        capture.samples.push_back(0.5f);
        QVERIFY(microphone.take(packet));
    }
    void staleBacklogProducesSampleGap() {
        PlankSdlMicrophone microphone;
        for (int i = 0; i < 7; ++i) appendPacket();
        PlankSdlMicrophone::Packet packet;
        QVERIFY(!microphone.take(packet));
        QCOMPARE(capture.clears, 1u);
        appendPacket();
        QVERIFY(microphone.take(packet));
        QCOMPARE(packet.sampleTime, std::uint64_t(7 * PlankMicrophoneFrames));
    }
    void captureFailure_data() {
        QTest::addColumn<int>("failure");
        QTest::newRow("availability") << 0;
        QTest::newRow("read") << 1;
        QTest::newRow("non-finite-sample") << 2;
        QTest::newRow("clear") << 3;
    }
    void captureFailure() {
        QFETCH(int, failure);
        {
            PlankSdlMicrophone microphone;
            appendPacket();
            if (failure == 0) capture.available = false;
            if (failure == 1) capture.read = false;
            if (failure == 2) capture.samples[0] = std::numeric_limits<float>::quiet_NaN();
            if (failure == 3) {
                for (int i = 0; i < 6; ++i) appendPacket();
                capture.clear = false;
            }
            PlankSdlMicrophone::Packet packet;
            QVERIFY(!microphone.take(packet));
            QVERIFY(!microphone.valid());
            QCOMPARE(capture.destroys, 1u);
            QCOMPARE(capture.quits, 1u);
        }
        QCOMPARE(capture.destroys, 1u);
        QCOMPARE(capture.quits, 1u);
    }
};
QTEST_GUILESS_MAIN(SdlMicrophoneTest)
#include "test_sdlmicrophone.moc"
