#pragma once

#include <QJsonObject>
#include <QVector>

#include <algorithm>

namespace StreamBenchmark {

struct Run
{
    static constexpr double minimumCaptureFraction = 0.75;
    static constexpr double minimumBitrateFraction = 0.05;
    static constexpr double minimumMotionFraction = 0.80;
    static constexpr double minimumPresentationFraction = 0.80;

    QJsonObject mode;
    double seconds = 0;
    double motionSeconds = 0;
    double decodeMs = 0;
    double renderMs = 0;
    double receivedBytes = 0;
    qint64 received = 0;
    qint64 freshCaptures = 0;
    qint64 decoded = 0;
    qint64 presented = 0;
    qint64 missing = 0;
    int windows = 0;
    int rttSamples = 0;
    double totalRttMs = 0;
    int lossSamples = 0;
    double fecBefore = 0;
    double fecAfter = 0;
    double worstPresentedFps = 0;
    bool hardware = true;

    void add(const QJsonObject& sample)
    {
        const double interval = sample.value("seconds").toDouble();
        if (interval <= 0) return;
        const int frames = sample.value("presented").toInt();
        const double fps = frames / interval;
        const int captures = sample.value("freshCaptures").toInt();
        const double bytes = sample.value("bytes").toDouble();
        const int targetFps = mode.value("fps").toInt();
        const int targetKbps = mode.value("bitrate_kbps").toInt();
        if (targetFps > 0 && targetKbps > 0 &&
                captures >= interval * targetFps * minimumCaptureFraction &&
                bytes * 8 >= interval * targetKbps * 1000 * minimumBitrateFraction) {
            motionSeconds += interval;
        }
        worstPresentedFps = windows == 0 ? fps : std::min(worstPresentedFps, fps);
        ++windows;
        seconds += interval;
        decodeMs += sample.value("decodeMs").toDouble();
        renderMs += sample.value("renderMs").toDouble();
        receivedBytes += bytes;
        received += sample.value("received").toInt();
        freshCaptures += captures;
        decoded += sample.value("decoded").toInt();
        presented += frames;
        missing += sample.value("missing").toInt();
        const int rtt = sample.value("rtt").toInt();
        if (rtt > 0) {
            ++rttSamples;
            totalRttMs += rtt;
        }
        const double before = sample.value("fecBefore").toDouble(-1);
        const double after = sample.value("fecAfter").toDouble(-1);
        if (before >= 0 && after >= 0) {
            ++lossSamples;
            fecBefore += before;
            fecAfter += after;
        }
        hardware = hardware && sample.value("hardware").toBool();
    }

    double presentedFps() const { return seconds > 0 ? presented / seconds : 0; }
    double freshCaptureFps() const { return seconds > 0 ? freshCaptures / seconds : 0; }
    double videoMbps() const { return seconds > 0 ? receivedBytes * 8 / seconds / 1e6 : 0; }
    bool completed() const { return seconds >= 12.0 && windows >= 10; }
    bool exercisedWorkload() const
    {
        const int fps = mode.value("fps").toInt();
        const int kbps = mode.value("bitrate_kbps").toInt();
        // One burst of traffic in a static preview can raise its averages.
        return completed() && fps > 0 && kbps > 0 &&
                freshCaptureFps() >= fps * minimumCaptureFraction &&
                videoMbps() >= (kbps / 1000.0) * minimumBitrateFraction &&
                motionSeconds >= seconds * minimumMotionFraction;
    }
    bool meetsTarget() const
    {
        const int fps = mode.value("fps").toInt();
        return exercisedWorkload() && hardware &&
                presentedFps() >= fps * 0.95 && worstPresentedFps >= fps * minimumPresentationFraction;
    }
};

inline int recommend(const QVector<Run>& runs)
{
    // A recommendation must describe one comparable display mode.
    if (runs.isEmpty()) return -1;
    const auto& reference = runs.front().mode;
    for (const auto& run : runs) {
        for (const auto* key : {"width", "height", "fps", "route"}) {
            if (run.mode.value(key) != reference.value(key)) return -1;
        }
    }
    // Runs are ordered by the Host's supported hardware profile preference.
    for (int i = 0; i < runs.size(); ++i) {
        if (runs[i].meetsTarget()) return i;
    }
    return -1;
}

} // namespace StreamBenchmark
