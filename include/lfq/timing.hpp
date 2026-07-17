// SPDX-License-Identifier: MIT
//
// Portable nanosecond timing. Defaults to std::chrono::steady_clock, which is
// monotonic and available on every platform. rdtsc-style counters are faster
// but need per-CPU calibration and behave differently under emulation, so they
// are deliberately left out of the hot path here.

#pragma once

#include <chrono>
#include <cstdint>

namespace lfq {

inline std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Rough per-call overhead of now_ns() so a benchmark can subtract it out.
inline std::uint64_t timer_overhead_ns(int samples = 200000) {
    std::uint64_t best = UINT64_MAX;
    for (int i = 0; i < samples; ++i) {
        const std::uint64_t a = now_ns();
        const std::uint64_t b = now_ns();
        if (b - a < best) best = b - a;
    }
    return best;
}

}  // namespace lfq
