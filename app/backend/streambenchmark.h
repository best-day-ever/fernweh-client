#pragma once

#include <QJsonObject>
#include <QVector>

#include <algorithm>

namespace StreamBenchmark {

struct Run
{
    QJsonObject mode;
    double seconds = 0;
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
        worstPresentedFps = windows == 0 ? fps : std::min(worstPresentedFps, fps);
        ++windows;
        seconds += interval;
        decodeMs += sample.value("decodeMs").toDouble();
        renderMs += sample.value("renderMs").toDouble();
        receivedBytes += sample.value("bytes").toDouble();
        received += sample.value("received").toInt();
        freshCaptures += sample.value("freshCaptures").toInt();
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
        // Static refresh can still present at 60 fps. Require fresh capture
        // timestamps and enough encoded traffic to exercise the stream.
        constexpr double minimumTargetFraction = 0.05;
        return completed() && fps > 0 && kbps > 0 && freshCaptureFps() >= fps * 0.75 &&
                videoMbps() >= (kbps / 1000.0) * minimumTargetFraction;
    }
    bool meetsTarget() const
    {
        const int fps = mode.value("fps").toInt();
        return exercisedWorkload() && hardware &&
                presentedFps() >= fps * 0.95 && worstPresentedFps >= fps * 0.80;
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
