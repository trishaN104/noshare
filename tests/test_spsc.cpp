// SPDX-License-Identifier: MIT
//
// Correctness tests for lfq::SpscQueue. No external framework — a tiny
// assert-style harness keeps the project dependency-free and easy to read.
//
// All test data is synthetic (generated in this file). Nothing here reads any
// external, proprietary, telemetry, personal, or consumer data.

#include "lfq/spsc_queue.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
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

// --- Pillar 3: algorithmic correctness on the single-threaded path. ---

void test_fifo_order() {
    std::printf("test_fifo_order\n");
    lfq::SpscQueue<int> q(8);
    for (int i = 0; i < 5; ++i) CHECK(q.try_push(i));
    int out = -1;
    for (int i = 0; i < 5; ++i) {
        CHECK(q.try_pop(out));
        CHECK(out == i);  // strict FIFO
    }
    CHECK(!q.try_pop(out));  // now empty
}

void test_full_and_empty() {
    std::printf("test_full_and_empty\n");
    lfq::SpscQueue<int> q(4);  // rounds to capacity 4
    CHECK(q.capacity() == 4);
    for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));  // full: push must fail, not corrupt
    int out = -1;
    CHECK(q.try_pop(out));
    CHECK(out == 0);
    CHECK(q.try_push(4));    // one slot freed -> push succeeds again
    CHECK(!q.try_push(5));   // full again
}

void test_wraparound() {
    std::printf("test_wraparound\n");
    lfq::SpscQueue<int> q(4);
    int out = -1;
    // Push/pop far more than capacity to exercise index wraparound.
    for (int i = 0; i < 10000; ++i) {
        CHECK(q.try_push(i));
        CHECK(q.try_pop(out));
        CHECK(out == i);
    }
    CHECK(q.size() == 0);
}

void test_capacity_rounding() {
    std::printf("test_capacity_rounding\n");
    CHECK(lfq::SpscQueue<int>(1).capacity() == 2);
    CHECK(lfq::SpscQueue<int>(5).capacity() == 8);
    CHECK(lfq::SpscQueue<int>(1024).capacity() == 1024);
    CHECK(lfq::SpscQueue<int>(1025).capacity() == 2048);
}

// --- Pillar 3 + concurrency: the real proof it is a correct SPSC queue. ---

void test_concurrent_stress() {
    std::printf("test_concurrent_stress (this is the important one)\n");
    constexpr std::uint64_t kCount = 5'000'000;
    lfq::SpscQueue<std::uint64_t> q(1024);

    std::atomic<bool> ok{true};

    std::thread consumer([&] {
        std::uint64_t expected = 0;
        std::uint64_t value = 0;
        while (expected < kCount) {
            if (q.try_pop(value)) {
                // Every value must arrive exactly once, in order: no loss,
                // no duplication, no reordering.
                if (value != expected) {
                    ok.store(false, std::memory_order_relaxed);
                    return;
                }
                ++expected;
            }
        }
    });

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount;) {
            if (q.try_push(i)) ++i;  // retry on full
        }
    });

    producer.join();
    consumer.join();
    CHECK(ok.load());
}

}  // namespace

int main() {
    std::printf("=== lfq::SpscQueue correctness tests ===\n");
    test_fifo_order();
    test_full_and_empty();
    test_wraparound();
    test_capacity_rounding();
    test_concurrent_stress();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("TESTS FAILED\n");
    return 1;
}
