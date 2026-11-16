#pragma once

#include <QPointF>
#include <QSize>
#include <QString>
#include <climits>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>
#include <plank_transport_pointer.h>

// The session thread owns this state. Platform adapters supply native capture;
// the reliable Host ACK is the only authority for returning to absolute input.
class PlankImmersiveMouse
{
public:
    struct Hooks {
        std::function<bool()> begin;
        std::function<void(QPointF, bool)> finish;
        std::function<bool(const unsigned char*, unsigned)> send;
        std::function<bool(QPointF, QSize)> absolute;
        std::function<void()> releaseButtons;
        std::function<void()> failed;
    };
    explicit PlankImmersiveMouse(Hooks hooks) : m_Hooks(std::move(hooks)) {}

    bool enabled() const { return m_Enabled; }
    bool ownsPointer() const { return m_Native; }
    bool captured() const { return m_State == Captured; }
    bool ending() const { return m_State == Ending; }
    QSize dimensions() const { return m_Size; }

    bool toggle(bool allowed, QSize dimensions, QPointF anchor, std::uint64_t now)
    {
        if (!m_Enabled) {
            if (!allowed || !validSize(dimensions) || !finite(anchor)) return false;
            m_Enabled = true;
            if (m_State == Ending) return true;
            m_Size = dimensions;
            return begin(anchor);
        }
        m_Enabled = false;
        if (m_State == Captured) {
            if (m_Hooks.releaseButtons) m_Hooks.releaseButtons();
            if (!send(PLANK_POINTER_END)) return fail();
            m_State = Ending;
            m_Deadline = now + 2000;
        }
        return true;
    }

    void motion(QPointF delta)
    {
        if (!finite(delta) || qAbs(delta.x()) > SHRT_MAX || qAbs(delta.y()) > SHRT_MAX) {
            fail(); return;
        }
        if (ending()) {
            if (!m_Pending.empty() && m_Pending.back().motion &&
                    qAbs((m_Pending.back().delta + delta).x()) <= SHRT_MAX &&
                    qAbs((m_Pending.back().delta + delta).y()) <= SHRT_MAX) {
                m_Pending.back().delta += delta;
            } else enqueue({true, delta, {}});
        } else if (captured() && !delta.isNull() && !send(PLANK_POINTER_MOVE, delta)) fail();
    }

    // Actions retain their position in the same input stream as deferred motion.
    bool defer(std::function<void(QPointF)> action)
    {
        if (!ending()) return false;
        enqueue({false, {}, std::move(action)});
        return true;
    }

    void acknowledge(const unsigned char* data, unsigned length)
    {
        PlankPointerPacket packet {};
        if (plank_pointer_decode(data, length, &packet) || packet.operation != PLANK_POINTER_END_ACK) {
            fail(); return;
        }
        if (!ending() || packet.epoch != m_Epoch || packet.sequence != m_Sequence) return;
        QPointF position(packet.x, packet.y);
        if (packet.width != m_Size.width() || packet.height != m_Size.height() ||
                position.x() < 0 || position.y() < 0 ||
                position.x() >= m_Size.width() || position.y() >= m_Size.height()) {
            fail(); return;
        }
        m_State = Idle;
        m_Deadline = 0;
        if (m_Enabled && !begin(position)) return;
        auto pending = std::exchange(m_Pending, {});
        for (auto& item : pending) {
            if (!m_Native) return;
            if (ending()) { enqueue(std::move(item)); continue; }
            if (item.motion) {
                if (captured()) motion(item.delta);
                else {
                    position += item.delta;
                    position.setX(qBound(0.0, position.x(), double(m_Size.width() - 1)));
                    position.setY(qBound(0.0, position.y(), double(m_Size.height() - 1)));
                    if (!m_Hooks.absolute(position, m_Size)) { fail(); return; }
                }
            } else item.action(position);
        }
        if (m_State == Idle && m_Native) finish(position, true);
    }

    void poll(std::uint64_t now)
    {
        if (ending() && now >= m_Deadline) fail();
    }

    void cancel()
    {
        m_Enabled = false;
        m_Pending.clear();
        if (m_State != Idle) {
            if (m_Hooks.releaseButtons) m_Hooks.releaseButtons();
            send(PLANK_POINTER_CANCEL);
        }
        m_State = Idle;
        m_Deadline = 0;
        if (m_Native) finish({}, false);
    }

private:
    enum State { Idle, Captured, Ending };
    struct Pending {
        bool motion;
        QPointF delta;
        std::function<void(QPointF)> action;
    };
    static bool finite(QPointF p) { return std::isfinite(p.x()) && std::isfinite(p.y()); }
    static bool validSize(QSize size) {
        return size.width() > 0 && size.height() > 0 && size.width() <= SHRT_MAX && size.height() <= SHRT_MAX;
    }
    bool begin(QPointF anchor)
    {
        if (!m_Hooks.begin || !m_Hooks.begin()) {
            m_Enabled = false;
            if (m_Native) cancel();
            return false;
        }
        m_Native = true;
        m_State = Captured;
        ++m_Epoch;
        m_Sequence = 0;
        anchor.setX(qBound(0.0, anchor.x(), double(m_Size.width() - 1)));
        anchor.setY(qBound(0.0, anchor.y(), double(m_Size.height() - 1)));
        if (!send(PLANK_POINTER_BEGIN, anchor)) return fail();
        return true;
    }
    bool send(unsigned char operation, QPointF value = {})
    {
        PlankPointerPacket packet {operation, m_Epoch, ++m_Sequence,
            static_cast<std::uint16_t>(m_Size.width()), static_cast<std::uint16_t>(m_Size.height()),
            float(value.x()), float(value.y())};
        unsigned char bytes[PLANK_POINTER_PACKET_SIZE];
        plank_pointer_encode(bytes, &packet);
        return m_Hooks.send && m_Hooks.send(bytes, sizeof(bytes));
    }
    void finish(QPointF anchor, bool reanchor)
    {
        m_Native = false;
        if (m_Hooks.finish) m_Hooks.finish(anchor, reanchor);
    }
    void enqueue(Pending item)
    {
        if (m_Pending.size() >= 128) { fail(); return; }
        m_Pending.push_back(std::move(item));
    }
    bool fail()
    {
        cancel();
        if (m_Hooks.failed) m_Hooks.failed();
        return false;
    }
    Hooks m_Hooks;
    State m_State = Idle;
    bool m_Enabled = false, m_Native = false;
    QSize m_Size;
    std::uint64_t m_Epoch = 0, m_Sequence = 0;
    std::uint64_t m_Deadline = 0;
    std::vector<Pending> m_Pending;
};
