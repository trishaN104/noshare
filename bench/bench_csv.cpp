// SPDX-License-Identifier: MIT
//
// Emits benchmark results as CSV so they can be plotted (see tools/plot).
// Produces two files in the output directory (argv[1], default "docs/results"):
//   scaling.csv      throughput vs producer/consumer count, lock-free vs mutex
//   latency_cdf.csv  one-way SPSC hand-off latency as a percentile -> ns curve
//
// All data is synthetic (in-process counters and timestamps). No external,
// proprietary, telemetry, personal, or consumer data is used.

#include "lfq/mpmc_queue.hpp"
#include "lfq/spsc_queue.hpp"
#include "lfq/histogram.hpp"
#include "lfq/timing.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using lfq::now_ns;

template <typename T>
class MutexQueue {
public:
    explicit MutexQueue(std::size_t capacity) : cap_(capacity) {}
    bool try_push(const T& v) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.size() >= cap_) return false;
        q_.push(v);
        return true;
    }
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop();
        return true;
    }
private:
    std::size_t cap_;
    std::mutex m_;
    std::queue<T> q_;
};

template <typename Queue>
double run(int producers, int consumers, std::uint64_t per_producer) {
    Queue q(4096);
    const std::uint64_t total = std::uint64_t(producers) * per_producer;
    std::atomic<std::uint64_t> consumed{0};

    const auto start = Clock::now();
    std::vector<std::thread> cs;
    for (int c = 0; c < consumers; ++c) {
        cs.emplace_back([&] {
            std::uint64_t v = 0;
            while (consumed.load(std::memory_order_relaxed) < total) {
                if (q.try_pop(v)) consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    std::vector<std::thread> ps;
    for (int p = 0; p < producers; ++p) {
        ps.emplace_back([&, p] {
            for (std::uint64_t i = 0; i < per_producer;) {
                if (q.try_push(std::uint64_t(p) * per_producer + i)) ++i;
            }
        });
    }
    for (auto& t : ps) t.join();
    for (auto& t : cs) t.join();

    const double secs = std::chrono::duration<double>(Clock::now() - start).count();
    return double(total) / secs;
}

void write_scaling(const std::string& dir) {
    const std::string path = dir + "/scaling.csv";
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::printf("could not open %s\n", path.c_str());
        return;
    }
    std::fprintf(f, "threads,lockfree_mps,mutex_mps\n");
    for (int n : {1, 2, 4, 8}) {
        const double lf = run<lfq::MpmcQueue<std::uint64_t>>(n, n, 1'000'000);
        const double mx = run<MutexQueue<std::uint64_t>>(n, n, 1'000'000);
        std::fprintf(f, "%d,%.0f,%.0f\n", n, lf, mx);
        std::printf("scaling  %dP/%dC: lock-free %.2f M/s  mutex %.2f M/s\n",
                    n, n, lf / 1e6, mx / 1e6);
    }
    std::fclose(f);
    std::printf("wrote %s\n", path.c_str());
}

void write_latency_cdf(const std::string& dir) {
    constexpr std::uint64_t kWarmup = 100'000;
    constexpr std::uint64_t kCount = 2'000'000;
    struct Msg {
        std::uint64_t seq;
        std::uint64_t send_ns;
    };
    lfq::SpscQueue<Msg> q(1024);
    const std::uint64_t overhead = lfq::timer_overhead_ns();
    lfq::Histogram hist;

    std::thread consumer([&] {
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
        for (std::uint64_t i = 0; i < kWarmup + kCount;) {
            Msg m{i, now_ns()};
            if (q.try_push(m)) {
                ++i;
                const std::uint64_t until = now_ns() + 200;
                while (now_ns() < until) {
                }
            }
        }
    });
    producer.join();
    consumer.join();

    const std::string path = dir + "/latency_cdf.csv";
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::printf("could not open %s\n", path.c_str());
        return;
    }
    std::fprintf(f, "percentile,latency_ns\n");
    for (int p = 1; p <= 99; ++p) {
        std::fprintf(f, "%d,%llu\n", p,
                     static_cast<unsigned long long>(hist.percentile(p)));
    }
    for (double p : {99.9, 99.99, 99.999}) {
        std::fprintf(f, "%.3f,%llu\n", p,
                     static_cast<unsigned long long>(hist.percentile(p)));
    }
    std::fclose(f);
    std::printf("wrote %s (%llu samples)\n", path.c_str(),
                static_cast<unsigned long long>(hist.count()));
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "docs/results";
    std::printf("=== emitting benchmark CSV to %s ===\n", dir.c_str());
    write_scaling(dir);
    write_latency_cdf(dir);
    return 0;
}
