#pragma once

#include "../../backend/displayplanner.h"
#include "../plankpresentation.h"
#include <QMetaObject>
#include <QObject>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <functional>
#include <algorithm>

struct QtSessionLayout
{
    struct Output {
        QString key;
        QRect clientBounds;
        QRectF sourcePixels;
        QRectF desktopPixels;
        bool primary = false;
    };
    QVector<Output> outputs;
    QSize desktopSize;
    QSize streamSize;
    bool planned = false;
    bool guest = false;
};

// Projection only: Host geometry is authoritative after an arrangement launch.
// This does not discover displays, alter a profile or send a Host request.
inline QtSessionLayout qtSessionLayout(const DisplayPlanner::Plan& plan,
                                       const QVector<NvClientDisplay>& displays,
                                       const NvOutputTopology& topology,
                                       QSize stream, bool guest = false,
                                       const QString& sharedOutput = QString())
{
    QtSessionLayout result;
    result.guest = guest;
    result.streamSize = stream;
    if (guest) {
        result.planned = true;
        result.desktopSize = QSize(topology.desktopWidth, topology.desktopHeight);
        auto outputs = topology.outputs;
        std::stable_sort(outputs.begin(), outputs.end(), [](const auto& a, const auto& b) {
            return a.primary && !b.primary;
        });
        for (const auto& output : outputs) {
            if (!sharedOutput.isEmpty() && output.id != sharedOutput) continue;
            const QRect desktop(output.x - topology.desktopX, output.y - topology.desktopY,
                                output.width, output.height);
            result.outputs.append({QStringLiteral("guest:") + output.id, {},
                sharedOutput.isEmpty() ? PlankPresentation::sourceRectInStream(
                    output.captureRect(), topology.captureSize(), stream) : QRectF(QPointF(), stream),
                desktop, result.outputs.isEmpty()});
        }
        return result;
    }
    if (!plan.ok) return result;
    result.planned = true;
    result.desktopSize = plan.canvas;
    const auto targets = DisplayPlanner::presentationTargets(plan, displays, true);
    const bool published = !plan.arrangement.isEmpty() && topology.matchesRequestedArrangement(plan.arrangement);
    for (int index = 0; index < displays.size(); ++index) {
        const int entry = targets.planIndices.value(index, -1);
        if (entry < 0) continue;
        const auto& output = plan.outputs.at(entry);
        QRect desktop(output.position, output.size);
        QRect capture = output.captureRect.isValid() ? output.captureRect : desktop;
        QSize captureSize = plan.capture.isValid() ? plan.capture : plan.canvas;
        if (published) {
            for (const auto& actual : topology.outputs) {
                if (actual.arrangementIndex != output.arrangementIndex) continue;
                desktop = QRect(actual.x - topology.desktopX, actual.y - topology.desktopY,
                                actual.width, actual.height);
                capture = actual.captureRect();
                captureSize = topology.captureSize();
                result.desktopSize = QSize(topology.desktopWidth, topology.desktopHeight);
                break;
            }
        }
        result.outputs.append({displays[index].key, displays[index].bounds,
            PlankPresentation::sourceRectInStream(capture, captureSize, stream),
            desktop, index == targets.primaryDisplay});
    }
    if (!targets.separateWindows && result.outputs.size() > 1) {
        auto selected = std::find_if(result.outputs.begin(), result.outputs.end(),
            [](const auto& output) { return output.primary; });
        auto primary = selected == result.outputs.end() ? result.outputs.first() : *selected;
        const bool packed = plan.packed || (published && topology.captureSize() != result.desktopSize);
        if (!packed) {
            primary.sourcePixels = QRectF(QPointF(), stream);
            primary.desktopPixels = QRectF(QPointF(), result.desktopSize);
        }
        result.outputs = {primary};
    }
    return result;
}

// Serialized worker-to-GUI handoff. Cancellation is durable and releases the
// worker even if the queued GUI operation never gets serviced during Quit.
class QtSessionLayoutHandoff
{
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        bool cancelled = false, ready = false, accepted = false;
    };
    std::shared_ptr<State> m_State = std::make_shared<State>();
public:
    bool request(QObject* owner, std::function<bool()> apply)
    {
        const auto state = m_State;
        std::unique_lock<std::mutex> lock(state->mutex);
        if (state->cancelled) return false;
        state->ready = false;
        const bool queued = QMetaObject::invokeMethod(owner, [state, apply = std::move(apply)] {
            {
                std::lock_guard<std::mutex> guard(state->mutex);
                if (state->cancelled) return;
            }
            const bool accepted = apply();
            std::lock_guard<std::mutex> guard(state->mutex);
            state->accepted = accepted;
            state->ready = true;
            state->changed.notify_all();
        }, Qt::QueuedConnection);
        if (!queued) return false;
        state->changed.wait(lock, [&] { return state->ready || state->cancelled; });
        return !state->cancelled && state->accepted;
    }
    void cancel()
    {
        const auto state = m_State;
        std::lock_guard<std::mutex> lock(state->mutex);
        state->cancelled = true;
        state->changed.notify_all();
    }
};
