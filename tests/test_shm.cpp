// SPDX-License-Identifier: MIT
//
// Smoke tests for lfq::ShmSpscChannel. The owner creates a named region and a
// second channel opens the SAME region through an independent mapping, so this
// exercises the actual shared-memory path (two mappings, atomics living in
// shared memory) rather than a plain in-process queue. Full cross-process
// benchmarking is meant to run on Linux with two separate processes.
//
// All data is synthetic (generated here). Nothing reads any external,
// proprietary, telemetry, personal, or consumer data.

#include "lfq/shm_transport.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
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

struct Payload {
    std::uint64_t seq;
    std::uint64_t checksum;
};

void test_create_open_roundtrip() {
    std::printf("test_create_open_roundtrip\n");
    const std::string name = "unit_roundtrip";
    auto owner = lfq::ShmSpscChannel<Payload>::create(name, 1024);
    auto peer = lfq::ShmSpscChannel<Payload>::open(name);
    CHECK(owner.capacity() == peer.capacity());

    Payload in{42, 42 * 2654435761ULL};
    CHECK(owner.try_push(in));
    Payload out{};
    CHECK(peer.try_pop(out));
    CHECK(out.seq == in.seq);
    CHECK(out.checksum == in.checksum);
    CHECK(!peer.try_pop(out));  // empty again
}

void test_streaming_across_mappings() {
    std::printf("test_streaming_across_mappings (the important one)\n");
    constexpr std::uint64_t kCount = 2'000'000;
    const std::string name = "unit_stream";
    auto owner = lfq::ShmSpscChannel<Payload>::create(name, 4096);
    auto peer = lfq::ShmSpscChannel<Payload>::open(name);

    std::atomic<bool> ok{true};

    std::thread consumer([&] {
        Payload m{};
        for (std::uint64_t i = 0; i < kCount;) {
            if (peer.try_pop(m)) {
                if (m.seq != i || m.checksum != i * 2654435761ULL) {
                    ok.store(false);
                    return;
                }
                ++i;
            }
        }
    });

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount;) {
            Payload m{i, i * 2654435761ULL};
            if (owner.try_push(m)) ++i;
        }
    });

    producer.join();
    consumer.join();
    CHECK(ok.load());
}

}  // namespace

int main() {
    std::printf("=== lfq::ShmSpscChannel smoke tests ===\n");
    try {
        test_create_open_roundtrip();
        test_streaming_across_mappings();
    } catch (const std::exception& e) {
        std::printf("  EXCEPTION: %s\n", e.what());
        ++g_failures;
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("TESTS FAILED\n");
    return 1;
}
