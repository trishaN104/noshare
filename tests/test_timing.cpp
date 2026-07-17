// SPDX-License-Identifier: MIT
//
// Tests for lfq timing helpers. Verifies monotonicity and that measuring an
// elapsed interval reports a sane value. All data is synthetic.

#include "lfq/timing.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

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

void test_monotonic() {
    std::printf("test_monotonic\n");
    std::uint64_t prev = lfq::now_ns();
    for (int i = 0; i < 100000; ++i) {
        const std::uint64_t cur = lfq::now_ns();
        CHECK(cur >= prev);  // steady_clock never goes backwards
        prev = cur;
    }
}

void test_measures_sleep() {
    std::printf("test_measures_sleep\n");
    const std::uint64_t start = lfq::now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::uint64_t elapsed = lfq::now_ns() - start;
    CHECK(elapsed >= 10'000'000);   // at least 10 ms
    CHECK(elapsed <= 500'000'000);  // and not absurdly large
}

void test_overhead_is_reported() {
    std::printf("test_overhead_is_reported\n");
    const std::uint64_t oh = lfq::timer_overhead_ns(10000);
    // The overhead is the minimum observed delta between two reads; it must be a
    // finite, non-sentinel value.
    CHECK(oh != UINT64_MAX);
}

}  // namespace

int main() {
    std::printf("=== lfq timing tests ===\n");
    test_monotonic();
    test_measures_sleep();
    test_overhead_is_reported();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("TESTS FAILED\n");
    return 1;
}
