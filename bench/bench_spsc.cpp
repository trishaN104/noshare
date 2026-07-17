// SPDX-License-Identifier: MIT
//
// Benchmarks for lfq::SpscQueue: sustained throughput and one-way hand-off
// latency distribution (p50/p99/p99.9/max). Threads are pinned to fixed cores
// and a warm-up phase is discarded. Latency runs under a paced producer so the
// queue stays shallow, isolating hand-off cost from queueing delay.
//
// All data is synthetic (in-process sequence numbers and timestamps). No
// external, proprietary, telemetry, personal, or consumer data is used.

#include "lfq/spsc_queue.hpp"
#include "lfq/histogram.hpp"
#include "lfq/timing.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using lfq::now_ns;

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

struct Msg {
    std::uint64_t seq;
    std::uint64_t send_ns;
};

void bench_throughput() {
    constexpr std::uint64_t kCount = 50'000'000;
    lfq::SpscQueue<std::uint64_t> q(4096);

    std::thread consumer([&] {
        pin_to_core(2);
        std::uint64_t v = 0;
        for (std::uint64_t i = 0; i < kCount;) {
            if (q.try_pop(v)) ++i;
        }
    });

    const auto start = Clock::now();
    std::thread producer([&] {
        pin_to_core(4);
        for (std::uint64_t i = 0; i < kCount;) {
            if (q.try_push(i)) ++i;
        }
    });

    producer.join();
    consumer.join();
    const auto end = Clock::now();

    const double secs =
        std::chrono::duration<double>(end - start).count();
    const double mps = static_cast<double>(kCount) / secs;
    std::printf("Throughput:\n");
    std::printf("  messages   : %llu\n",
                static_cast<unsigned long long>(kCount));
    std::printf("  elapsed    : %.3f s\n", secs);
    std::printf("  throughput : %.2f M msgs/s\n", mps / 1e6);
    std::printf("  per message: %.2f ns\n", secs * 1e9 / kCount);
}

void bench_latency() {
    constexpr std::uint64_t kWarmup = 100'000;
    constexpr std::uint64_t kCount = 2'000'000;
    lfq::SpscQueue<Msg> q(1024);

    const std::uint64_t overhead = lfq::timer_overhead_ns();
    lfq::Histogram hist;

    std::thread consumer([&] {
        pin_to_core(2);
        Msg m{};
        for (std::uint64_t i = 0; i < kWarmup + kCount;) {
            if (q.try_pop(m)) {
                const std::uint64_t raw = now_ns() - m.send_ns;
                const std::uint64_t latency = raw > overhead ? raw - overhead : 0;
                if (i >= kWarmup) hist.record(latency);
                ++i;
            }
        }
    });

    std::thread producer([&] {
        pin_to_core(4);
        for (std::uint64_t i = 0; i < kWarmup + kCount;) {
            Msg m{i, now_ns()};
            if (q.try_push(m)) {
                ++i;
                // Keep the queue shallow so we measure hand-off, not backlog.
                const std::uint64_t until = now_ns() + 200;
                while (now_ns() < until) {
                }
            }
        }
    });

    producer.join();
    consumer.join();

    std::printf("\nLatency (one-way hand-off, low load), %llu samples:\n",
                static_cast<unsigned long long>(hist.count()));
    std::printf("  timer overhead subtracted: %llu ns\n",
                static_cast<unsigned long long>(overhead));
    std::printf("  p50   : %8llu ns\n",
                static_cast<unsigned long long>(hist.percentile(50)));
    std::printf("  p99   : %8llu ns\n",
                static_cast<unsigned long long>(hist.percentile(99)));
    std::printf("  p99.9 : %8llu ns\n",
                static_cast<unsigned long long>(hist.percentile(99.9)));
    std::printf("  p99.99: %8llu ns\n",
                static_cast<unsigned long long>(hist.percentile(99.99)));
    std::printf("  max   : %8llu ns\n",
                static_cast<unsigned long long>(hist.max()));
}

}  // namespace

int main() {
    std::printf("=== lfq::SpscQueue benchmarks ===\n");
    bench_throughput();
    bench_latency();
    return 0;
}
