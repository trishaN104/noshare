// SPDX-License-Identifier: MIT
//
// Correctness tests for lfq::MpmcQueue. Tiny assert-style harness, no external
// framework. All test data is synthetic (generated here). Nothing reads any
// external, proprietary, telemetry, personal, or consumer data.

#include "lfq/mpmc_queue.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("  FAIL: %s  (line %d)\n", #cond, __LINE__);      \
        }                                                                 \
    } while (0)

void test_fifo_single_threaded() {
    std::printf("test_fifo_single_threaded\n");
    lfq::MpmcQueue<int> q(8);
    for (int i = 0; i < 5; ++i) CHECK(q.try_push(i));
    int out = -1;
    for (int i = 0; i < 5; ++i) {
        CHECK(q.try_pop(out));
        CHECK(out == i);
    }
    CHECK(!q.try_pop(out));
}

void test_full_and_empty() {
    std::printf("test_full_and_empty\n");
    lfq::MpmcQueue<int> q(4);
    CHECK(q.capacity() == 4);
    for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));
    int out = -1;
    CHECK(q.try_pop(out));
    CHECK(out == 0);
    CHECK(q.try_push(4));
    CHECK(!q.try_push(5));
}

// N producers, M consumers. With multiple consumers we cannot assert global
// order, so we assert conservation: every value pushed is popped exactly once,
// with no loss and no duplication. Each consumer tallies a shared seen-count
// per value; the main thread verifies every count is exactly 1.
void test_nxm_conservation() {
    std::printf("test_nxm_conservation (the important one)\n");
    constexpr int kProducers = 4;
    constexpr int kConsumers = 4;
    constexpr std::uint64_t kPerProducer = 500'000;
    const std::uint64_t total = std::uint64_t(kProducers) * kPerProducer;

    lfq::MpmcQueue<std::uint64_t> q(1024);
    std::vector<std::atomic<std::uint8_t>> seen(total);
    for (auto& s : seen) s.store(0, std::memory_order_relaxed);

    std::atomic<std::uint64_t> consumed{0};
    std::atomic<bool> dup{false};

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            std::uint64_t value = 0;
            while (consumed.load(std::memory_order_relaxed) < total) {
                if (q.try_pop(value)) {
                    if (seen[value].fetch_add(1, std::memory_order_relaxed) != 0) {
                        dup.store(true);
                    }
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            const std::uint64_t base = std::uint64_t(p) * kPerProducer;
            for (std::uint64_t i = 0; i < kPerProducer;) {
                if (q.try_push(base + i)) ++i;
            }
        });
    }

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    CHECK(!dup.load());                       // no value popped twice
    CHECK(consumed.load() == total);          // no value lost
    std::uint64_t missing = 0;
    for (auto& s : seen) {
        if (s.load(std::memory_order_relaxed) != 1) ++missing;
    }
    CHECK(missing == 0);                       // exactly-once for all values
}

void test_nontrivial_type() {
    std::printf("test_nontrivial_type\n");
    lfq::MpmcQueue<std::string> q(8);
    CHECK(q.try_push(std::string("x")));
    CHECK(q.try_push(std::string("y")));
    std::string out;
    CHECK(q.try_pop(out) && out == "x");
    CHECK(q.try_pop(out) && out == "y");
    CHECK(!q.try_pop(out));
}

void test_drain_after_full() {
    std::printf("test_drain_after_full\n");
    lfq::MpmcQueue<int> q(4);
    for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));
    int out = -1;
    for (int i = 0; i < 4; ++i) CHECK(q.try_pop(out) && out == i);
    CHECK(!q.try_pop(out));
    CHECK(q.try_push(100));
    CHECK(q.try_pop(out) && out == 100);
}

void test_capacity_rounding() {
    std::printf("test_capacity_rounding\n");
    CHECK(lfq::MpmcQueue<int>(3).capacity() == 4);
    CHECK(lfq::MpmcQueue<int>(100).capacity() == 128);
    CHECK(lfq::MpmcQueue<int>(1024).capacity() == 1024);
}

// Two producers, two consumers, modest size: conservation must still hold.
void test_two_by_two_small() {
    std::printf("test_two_by_two_small\n");
    constexpr int kProducers = 2;
    constexpr int kConsumers = 2;
    constexpr std::uint64_t kPer = 100'000;
    const std::uint64_t total = std::uint64_t(kProducers) * kPer;

    lfq::MpmcQueue<std::uint64_t> q(256);
    std::vector<std::atomic<std::uint8_t>> seen(total);
    for (auto& s : seen) s.store(0, std::memory_order_relaxed);
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<bool> dup{false};

    std::vector<std::thread> cs;
    for (int c = 0; c < kConsumers; ++c) {
        cs.emplace_back([&] {
            std::uint64_t v = 0;
            while (consumed.load(std::memory_order_relaxed) < total) {
                if (q.try_pop(v)) {
                    if (seen[v].fetch_add(1, std::memory_order_relaxed) != 0)
                        dup.store(true);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    std::vector<std::thread> ps;
    for (int p = 0; p < kProducers; ++p) {
        ps.emplace_back([&, p] {
            for (std::uint64_t i = 0; i < kPer;) {
                if (q.try_push(std::uint64_t(p) * kPer + i)) ++i;
            }
        });
    }
    for (auto& t : ps) t.join();
    for (auto& t : cs) t.join();

    CHECK(!dup.load());
    CHECK(consumed.load() == total);
    std::uint64_t missing = 0;
    for (auto& s : seen)
        if (s.load(std::memory_order_relaxed) != 1) ++missing;
    CHECK(missing == 0);
}

}  // namespace

int main() {
    std::printf("=== lfq::MpmcQueue correctness tests ===\n");
    test_fifo_single_threaded();
    test_full_and_empty();
    test_nxm_conservation();
    test_nontrivial_type();
    test_drain_after_full();
    test_capacity_rounding();
    test_two_by_two_small();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("TESTS FAILED\n");
    return 1;
}
