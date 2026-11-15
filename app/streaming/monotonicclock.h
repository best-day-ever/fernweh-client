#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace PlankClock {
// An absent sample is stale; a future sample must not underflow the age.
inline std::uint32_t ageMilliseconds(std::uint64_t last, std::uint64_t now,
                                     std::uint32_t maximum = 60000)
{
    if (!last) return maximum;
    if (now < last) return 0;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(maximum, now - last));
}

// One process-wide epoch for audio/video synchronization, telemetry and
// cancellation deadlines. Wall-clock changes must not alter media timing.
inline std::uint64_t milliseconds()
{
    using Clock = std::chrono::steady_clock;
    static const auto epoch = Clock::now();
    return 1 + std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - epoch).count());
}
}
