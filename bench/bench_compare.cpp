// SPDX-License-Identifier: MIT
//
// Quantify the payoff of the lock-free design: run the same producer/consumer
// workload through the lock-free MPMC queue and through a mutex-guarded
// std::queue, and report the throughput ratio. This turns "lock-free is faster"
// into a measured number on the machine at hand.
//
// All data is synthetic (in-process counters). No external, proprietary,
// telemetry, personal, or consumer data is used.

#include "lfq/mpmc_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Mutex-guarded bounded queue with the same try_push/try_pop shape.
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

}  // namespace

int main() {
    constexpr int kProducers = 2;
    constexpr int kConsumers = 2;
    constexpr std::uint64_t kPerProducer = 2'000'000;

    std::printf("=== lock-free vs mutex (%dP/%dC, %llu msgs/producer) ===\n",
                kProducers, kConsumers,
                static_cast<unsigned long long>(kPerProducer));

    const double lf = run<lfq::MpmcQueue<std::uint64_t>>(
        kProducers, kConsumers, kPerProducer);
    const double mx = run<MutexQueue<std::uint64_t>>(
        kProducers, kConsumers, kPerProducer);

    std::printf("  lock-free MPMC : %8.2f M msgs/s\n", lf / 1e6);
    std::printf("  mutex + queue  : %8.2f M msgs/s\n", mx / 1e6);
    std::printf("  speedup        : %8.2fx\n", lf / mx);
    return 0;
}
