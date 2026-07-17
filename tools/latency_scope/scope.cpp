// SPDX-License-Identifier: MIT
//
// Latency microscope: measure one-way hand-off latency over an SPSC queue and
// attribute the tail to a concrete cause instead of hand-waving "jitter".
//
// The consumer timestamps each message on arrival and, whenever a sample lands
// in the tail (above a threshold), it snapshots the OS context-switch counters.
// Correlating tail spikes with involuntary context switches is a portable,
// no-privilege way to show *why* the tail exists: the scheduler preempted a
// thread, not the algorithm. On Linux we read getrusage(); on Windows the
// counters aren't exposed the same way, so we report the outlier trace and note
// that counter-level attribution belongs on Linux (perf_event_open / getrusage).
//
// All data is synthetic (in-process sequence numbers and timestamps). No
// external, proprietary, telemetry, personal, or consumer data is used.

#include "lfq/spsc_queue.hpp"
#include "lfq/histogram.hpp"
#include "lfq/timing.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#endif

namespace {

void pin_to_core(unsigned core) {
#if defined(_WIN32)
    SetThreadAffinityMask(GetCurrentThread(),
                          static_cast<DWORD_PTR>(1) << core);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)core;
#endif
}

// Involuntary context switches so far for the calling thread.
std::uint64_t involuntary_ctx_switches() {
#if defined(__linux__)
    struct rusage ru{};
    getrusage(RUSAGE_THREAD, &ru);
    return static_cast<std::uint64_t>(ru.ru_nivcsw);
#else
    return 0;  // not exposed portably on this platform
#endif
}

struct Msg {
    std::uint64_t seq;
    std::uint64_t send_ns;
};

struct Outlier {
    std::uint64_t seq;
    std::uint64_t latency_ns;
    std::uint64_t ctx_switch_delta;
};

}  // namespace

int main() {
    constexpr std::uint64_t kWarmup = 100'000;
    constexpr std::uint64_t kCount = 2'000'000;
    constexpr std::uint64_t kTailThresholdNs = 5'000;  // 5 us = "tail"

    std::printf("=== latency microscope ===\n");
    std::printf("samples: %llu, tail threshold: %llu ns\n",
                static_cast<unsigned long long>(kCount),
                static_cast<unsigned long long>(kTailThresholdNs));

    lfq::SpscQueue<Msg> q(1024);
    const std::uint64_t overhead = lfq::timer_overhead_ns();
    lfq::Histogram hist;
    std::vector<Outlier> outliers;
    outliers.reserve(4096);

    std::uint64_t tail_count = 0;
    std::uint64_t tail_with_ctx_switch = 0;

    std::thread consumer([&] {
        pin_to_core(2);
        std::uint64_t last_ctx = involuntary_ctx_switches();
        Msg m{};
        for (std::uint64_t i = 0; i < kWarmup + kCount;) {
            if (q.try_pop(m)) {
                const std::uint64_t raw = lfq::now_ns() - m.send_ns;
                const std::uint64_t latency = raw > overhead ? raw - overhead : 0;
                if (i >= kWarmup) {
                    hist.record(latency);
                    if (latency >= kTailThresholdNs) {
                        const std::uint64_t now_ctx = involuntary_ctx_switches();
                        const std::uint64_t delta = now_ctx - last_ctx;
                        ++tail_count;
                        if (delta > 0) ++tail_with_ctx_switch;
                        if (outliers.size() < outliers.capacity()) {
                            outliers.push_back({m.seq, latency, delta});
                        }
                        last_ctx = now_ctx;
                    }
                }
                ++i;
            }
        }
    });

    std::thread producer([&] {
        pin_to_core(4);
        for (std::uint64_t i = 0; i < kWarmup + kCount;) {
            Msg m{i, lfq::now_ns()};
            if (q.try_push(m)) {
                ++i;
                const std::uint64_t until = lfq::now_ns() + 200;
                while (lfq::now_ns() < until) {
                }
            }
        }
    });

    producer.join();
    consumer.join();

    std::printf("\ndistribution (timer overhead %llu ns subtracted):\n",
                static_cast<unsigned long long>(overhead));
    std::printf("  p50   : %8llu ns\n",
                static_cast<unsigned long long>(hist.percentile(50)));
    std::printf("  p99   : %8llu ns\n",
                static_cast<unsigned long long>(hist.percentile(99)));
    std::printf("  p99.9 : %8llu ns\n",
                static_cast<unsigned long long>(hist.percentile(99.9)));
    std::printf("  max   : %8llu ns\n",
                static_cast<unsigned long long>(hist.max()));

    std::printf("\ntail attribution (>= %llu ns):\n",
                static_cast<unsigned long long>(kTailThresholdNs));
    std::printf("  tail samples            : %llu\n",
                static_cast<unsigned long long>(tail_count));
#if defined(__linux__)
    const double pct = tail_count ? 100.0 * double(tail_with_ctx_switch) /
                                        double(tail_count)
                                  : 0.0;
    std::printf("  coincident with ctx sw  : %llu (%.1f%%)\n",
                static_cast<unsigned long long>(tail_with_ctx_switch), pct);
    std::printf("  -> tail spikes that line up with an involuntary context\n");
    std::printf("     switch are scheduler preemption, not queue overhead.\n");
#else
    std::printf("  ctx-switch attribution  : unavailable on this platform\n");
    std::printf("  -> run on Linux for getrusage(RUSAGE_THREAD) / perf_event_open\n");
    std::printf("     counter attribution. Worst outliers are listed below.\n");
#endif

    std::sort(outliers.begin(), outliers.end(),
              [](const Outlier& a, const Outlier& b) {
                  return a.latency_ns > b.latency_ns;
              });
    const std::size_t show = std::min<std::size_t>(10, outliers.size());
    std::printf("\ntop %zu outliers:\n", show);
    std::printf("  %-12s %-14s %-10s\n", "seq", "latency_ns", "ctx_sw");
    for (std::size_t i = 0; i < show; ++i) {
        std::printf("  %-12llu %-14llu %-10llu\n",
                    static_cast<unsigned long long>(outliers[i].seq),
                    static_cast<unsigned long long>(outliers[i].latency_ns),
                    static_cast<unsigned long long>(outliers[i].ctx_switch_delta));
    }
    return 0;
}
