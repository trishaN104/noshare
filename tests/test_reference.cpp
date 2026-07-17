// SPDX-License-Identifier: MIT
//
// Differential validation: run randomized push/pop sequences against each queue
// and against a std::queue "oracle", and require identical observable behaviour
// (same accept/reject on push-when-full and pop-when-empty, same FIFO output).
// A lock-free queue that matches a trivially-correct reference on millions of
// randomized operations is strong evidence the fast implementation is correct.
//
// All inputs are synthetic (a seeded PRNG). Nothing reads any external,
// proprietary, telemetry, personal, or consumer data.

#include "lfq/spsc_queue.hpp"
#include "lfq/mpsc_queue.hpp"
#include "lfq/mpmc_queue.hpp"

#include <cstdint>
#include <cstdio>
#include <queue>
#include <random>
#include <string>

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

// A bounded FIFO reference implemented with the standard library. Obviously
// correct; used only as the oracle to compare the lock-free queues against.
template <typename T>
class RefQueue {
public:
    explicit RefQueue(std::size_t capacity) : cap_(capacity) {}
    bool try_push(const T& v) {
        if (q_.size() >= cap_) return false;
        q_.push(v);
        return true;
    }
    bool try_pop(T& out) {
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop();
        return true;
    }
private:
    std::size_t cap_;
    std::queue<T> q_;
};

template <typename Queue>
void differential(const char* label, std::size_t capacity) {
    std::printf("differential vs reference: %s\n", label);
    Queue q(capacity);
    RefQueue<std::uint64_t> ref(q.capacity());

    std::mt19937_64 rng(0xC0FFEE);  // fixed seed -> reproducible
    std::uniform_int_distribution<int> op(0, 1);
    std::uint64_t next = 1;

    for (int i = 0; i < 5'000'000; ++i) {
        if (op(rng) == 0) {
            const std::uint64_t v = next++;
            const bool a = q.try_push(v);
            const bool b = ref.try_push(v);
            CHECK(a == b);  // both must agree on full/not-full
        } else {
            std::uint64_t va = 0, vb = 0;
            const bool a = q.try_pop(va);
            const bool b = ref.try_pop(vb);
            CHECK(a == b);          // agree on empty/not-empty
            if (a && b) CHECK(va == vb);  // and on the value (FIFO order)
        }
        if (g_failures) return;  // stop at first divergence
    }
}

}  // namespace

int main() {
    std::printf("=== differential validation against std::queue oracle ===\n");
    differential<lfq::SpscQueue<std::uint64_t>>("SpscQueue", 256);
    differential<lfq::MpscQueue<std::uint64_t>>("MpscQueue", 256);
    differential<lfq::MpmcQueue<std::uint64_t>>("MpmcQueue", 256);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("TESTS FAILED\n");
    return 1;
}
