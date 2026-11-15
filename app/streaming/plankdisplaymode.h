#pragma once

#include <QSize>
#include <QString>

class PlankDisplayMode
{
public:
    // Discovery includes every monitor for profile planning. Legacy bookmarks
    // and planned subsets must size their stream from the selected outputs.
    // The snapshot type remains owned by Session (including its platform ID).
    template<class Displays, class DisplayId>
    static QSize detectedClientResolution(const Displays& displays, DisplayId target,
                                         bool multi, bool planned)
    {
        QSize canvas(0, 0);
        for (const auto& display : displays) {
            if (planned ? display.planIndex < 0 : !display.legacySelected) continue;
            if (!multi) {
                if (display.displayId == target) return display.matchTarget;
                continue;
            }
            canvas.rwidth() += display.matchTarget.width();
            canvas.setHeight(qMax(canvas.height(), display.matchTarget.height()));
        }
        return canvas.isEmpty() ? QSize() : canvas;
    }

    // The ceiling of the legacy scaled-span path only (a 4K stream fitted to
    // the client); display arrangements use resolveClient instead.
    static QSize qualifiedMaximum();

    // Display arrangement: the stream is the workstation desktop 1:1. Returns
    // the canvas when it is even and fits the limit (encoder and decoder,
    // invalid for none), otherwise an invalid size and a user-facing reason.
    // Never scales silently: the planner already fitted the layout.
    static QSize resolveClient(const QSize& canvas, const QSize& limit, QString* error = nullptr);

    static QSize resolve(const QSize& detectedResolution,
                         const QSize& hostCanvasResolution = QSize(),
                         const QSize& maximumResolution = qualifiedMaximum());

private:
    static QSize fitWithin(const QSize& requestedResolution,
                           const QSize& maximumResolution);
};
