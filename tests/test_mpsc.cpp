// SPDX-License-Identifier: MIT
//
// Correctness tests for lfq::MpscQueue. Tiny assert-style harness, no external
// framework. All test data is synthetic (generated here). Nothing reads any
// external, proprietary, telemetry, personal, or consumer data.

#include "lfq/mpsc_queue.hpp"

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

void test_fifo_single_producer() {
    std::printf("test_fifo_single_producer\n");
    lfq::MpscQueue<int> q(8);
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
    lfq::MpscQueue<int> q(4);
    CHECK(q.capacity() == 4);
    for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));  // full
    int out = -1;
    CHECK(q.try_pop(out));
    CHECK(out == 0);
    CHECK(q.try_push(4));
    CHECK(!q.try_push(5));
}

// The real proof: N producers, 1 consumer. Encode (producer_id, seq) in each
// value and verify total conservation and per-producer FIFO order.
void test_multi_producer_conservation() {
    std::printf("test_multi_producer_conservation (the important one)\n");
    constexpr int kProducers = 4;
    constexpr std::uint64_t kPerProducer = 1'000'000;
    constexpr std::uint64_t kTotal = kProducers * kPerProducer;
    lfq::MpscQueue<std::uint64_t> q(1024);

    std::atomic<bool> ok{true};

    std::thread consumer([&] {
        std::vector<std::uint64_t> last_seq(kProducers, 0);
        std::vector<bool> started(kProducers, false);
        std::uint64_t received = 0;
        std::uint64_t value = 0;
        while (received < kTotal) {
            if (q.try_pop(value)) {
                const std::uint32_t pid = static_cast<std::uint32_t>(value >> 40);
                const std::uint64_t seq = value & ((std::uint64_t{1} << 40) - 1);
                if (pid >= kProducers) { ok.store(false); return; }
                if (started[pid] && seq != last_seq[pid] + 1) {
                    ok.store(false);  // per-producer order violated
                    return;
                }
                last_seq[pid] = seq;
                started[pid] = true;
                ++received;
            }
        }
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            const std::uint64_t tag = std::uint64_t(p) << 40;
            for (std::uint64_t s = 1; s <= kPerProducer;) {
                if (q.try_push(tag | s)) ++s;
            }
        });
    }
    for (auto& t : producers) t.join();
    consumer.join();
    CHECK(ok.load());
}

void test_nontrivial_type() {
    std::printf("test_nontrivial_type\n");
    lfq::MpscQueue<std::string> q(8);
    CHECK(q.try_push(std::string("a")));
    CHECK(q.try_push(std::string("b")));
    std::string out;
    CHECK(q.try_pop(out) && out == "a");
    CHECK(q.try_pop(out) && out == "b");
    CHECK(!q.try_pop(out));
}

void test_drain_after_full() {
    std::printf("test_drain_after_full\n");
    lfq::MpscQueue<int> q(4);
    for (int i = 0; i < 4; ++i) CHECK(q.try_push(i));
    CHECK(!q.try_push(99));
    int out = -1;
    for (int i = 0; i < 4; ++i) CHECK(q.try_pop(out) && out == i);
    CHECK(!q.try_pop(out));
    // Reusable after a full drain (sequence numbers advanced a full lap).
    CHECK(q.try_push(100));
    CHECK(q.try_pop(out) && out == 100);
}

void test_capacity_rounding() {
    std::printf("test_capacity_rounding\n");
    CHECK(lfq::MpscQueue<int>(3).capacity() == 4);
    CHECK(lfq::MpscQueue<int>(100).capacity() == 128);
    CHECK(lfq::MpscQueue<int>(1024).capacity() == 1024);
}

}  // namespace

int main() {
    std::printf("=== lfq::MpscQueue correctness tests ===\n");
    test_fifo_single_producer();
    test_full_and_empty();
    test_multi_producer_conservation();
    test_nontrivial_type();
    test_drain_after_full();
    test_capacity_rounding();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("TESTS FAILED\n");
    return 1;
}
