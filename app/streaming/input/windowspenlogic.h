#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace WindowsPenLogic {
// Inspired by the native sampling/WinTab fixes in Culley Bunker's SkulleyFX
// client (8280fc48). Keep driver mapping authoritative and bound fractions.
class FractionalPosition {
public:
    float sample(float pixel, double mapped) {
        if (!std::isfinite(pixel)) return 0;
        if (!std::isfinite(mapped) || std::abs(mapped - pixel) >= 16) {
            reset(); return pixel;
        }
        if (!m_Valid) { m_Offset = mapped - pixel - 0.5; m_Valid = true; }
        else {
            const double weight = std::min(0.1, std::abs(mapped - m_Last) * 0.01);
            m_Offset += weight * (mapped - pixel - 0.5 - m_Offset);
        }
        m_Last = mapped;
        return pixel + static_cast<float>(std::clamp(mapped - m_Offset - pixel, -1.0, 1.0));
    }
    void reset() { m_Valid = false; }
private:
    bool m_Valid = false;
    double m_Offset = 0, m_Last = 0;
};

// WinTab may enrich Ink immediately. It takes position ownership only after
// Ink has been quiet, and never continues a stroke begun in another app.
class Authority {
public:
    void ink(std::uint64_t now) { m_LastInk = now; m_HaveInk = true; m_Driving = false; m_Streak = 0; }
    bool drive(std::uint64_t now, bool target, float pressure) {
        if (!target || (m_HaveInk && now - m_LastInk < 250)) {
            m_Driving = false; m_Streak = 0; return false;
        }
        if (!m_Streak) m_Streak = now ? now : 1;
        if (!m_Driving) {
            if (now < m_Streak || now - m_Streak < 120) return false;
            m_Driving = true; m_WaitForLift = pressure > 0;
        }
        if (pressure <= 0) m_WaitForLift = false;
        return true;
    }
    bool acceptsContact() const { return !m_WaitForLift; }
    void reset() { *this = Authority(); }
private:
    std::uint64_t m_LastInk = 0, m_Streak = 0;
    bool m_HaveInk = false, m_Driving = false, m_WaitForLift = false;
};
}
