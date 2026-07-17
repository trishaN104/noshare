// SPDX-License-Identifier: MIT
//
// Tests for lfq::Histogram. Verifies bucketing, percentiles, mean/max, and the
// bounded relative-error property. All inputs are synthetic (generated here).

#include "lfq/histogram.hpp"

#include <cstdint>
#include <cstdio>

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

void test_empty() {
    std::printf("test_empty\n");
    lfq::Histogram h;
    CHECK(h.count() == 0);
    CHECK(h.max() == 0);
    CHECK(h.mean() == 0.0);
    CHECK(h.percentile(50) == 0);
}

void test_single_value() {
    std::printf("test_single_value\n");
    lfq::Histogram h;
    h.record(100);
    CHECK(h.count() == 1);
    CHECK(h.max() == 100);
    CHECK(h.percentile(50) >= 100);   // bucketed, so >= exact value
}

void test_count_and_max() {
    std::printf("test_count_and_max\n");
    lfq::Histogram h;
    for (std::uint64_t v = 1; v <= 1000; ++v) h.record(v);
    CHECK(h.count() == 1000);
    CHECK(h.max() == 1000);
}

void test_percentile_monotonic() {
    std::printf("test_percentile_monotonic\n");
    lfq::Histogram h;
    for (std::uint64_t v = 0; v < 100000; ++v) h.record(v);
    CHECK(h.percentile(50) <= h.percentile(90));
    CHECK(h.percentile(90) <= h.percentile(99));
    CHECK(h.percentile(99) <= h.percentile(99.9));
    CHECK(h.percentile(99.9) <= h.max());
}

void test_bounded_relative_error() {
    std::printf("test_bounded_relative_error\n");
    // Record a dense uniform distribution and check reported percentiles land
    // within the histogram's bounded relative error of the true percentiles.
    // With 4 sub-bits the per-octave error is ~1/16, so 8% is a safe envelope.
    lfq::Histogram h(4);
    constexpr std::uint64_t n = 100000;
    for (std::uint64_t v = 0; v < n; ++v) h.record(v);
    for (double p : {50.0, 90.0, 99.0}) {
        const double truth = p / 100.0 * double(n - 1);
        const std::uint64_t reported = h.percentile(p);
        const double rel = double(reported) / truth - 1.0;
        CHECK(rel <= 0.08);
        CHECK(rel >= -0.08);
    }
}

void test_linear_region_exact() {
    std::printf("test_linear_region_exact\n");
    lfq::Histogram h(4);
    for (std::uint64_t v = 0; v < 16; ++v) {
        lfq::Histogram one(4);
        one.record(v);
        CHECK(one.percentile(100) == v);  // small values are bucketed exactly
    }
}

void test_mean() {
    std::printf("test_mean\n");
    lfq::Histogram h;
    for (std::uint64_t v = 0; v <= 999; ++v) h.record(v);
    CHECK(h.mean() > 499.0 && h.mean() < 500.0);  // exact mean is 499.5
}

void test_zero_value() {
    std::printf("test_zero_value\n");
    lfq::Histogram h;
    h.record(0);
    CHECK(h.count() == 1);
    CHECK(h.max() == 0);
    CHECK(h.percentile(50) == 0);  // zero is in the exact linear region
}

void test_never_understates() {
    std::printf("test_never_understates\n");
    // The reported p100 must be >= the true maximum for any recorded value,
    // so latency tails are never reported as faster than they were.
    for (std::uint64_t v : {1ull, 7ull, 63ull, 1000ull, 999983ull}) {
        lfq::Histogram h(4);
        h.record(v);
        CHECK(h.percentile(100) >= v);
        CHECK(h.percentile(100) == h.max());  // clamped to the true max
    }
}

}  // namespace

int main() {
    std::printf("=== lfq::Histogram tests ===\n");
    test_empty();
    test_single_value();
    test_count_and_max();
    test_percentile_monotonic();
    test_bounded_relative_error();
    test_linear_region_exact();
    test_mean();
    test_zero_value();
    test_never_understates();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("TESTS FAILED\n");
    return 1;
}
