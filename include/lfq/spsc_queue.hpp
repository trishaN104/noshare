// SPDX-License-Identifier: MIT
//
// Bounded wait-free SPSC ring buffer. Single producer, single consumer.
// Standard C++20, no OS dependencies.

#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace lfq {

#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

template <typename T>
class SpscQueue {
    static_assert(std::is_nothrow_destructible_v<T>,
                  "T must be nothrow-destructible");

public:
    // Rounded up to a power of two so index->slot uses a mask, not a modulo.
    explicit SpscQueue(std::size_t capacity)
        : capacity_(round_up_pow2(capacity)),
          mask_(capacity_ - 1),
          slots_(static_cast<T*>(::operator new[](
              capacity_ * sizeof(T), std::align_val_t(alignof(T))))) {}

    ~SpscQueue() {
        // Destroy any elements the consumer never got to.
        std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        while (head != tail) {
            slots_[head & mask_].~T();
            ++head;
        }
        ::operator delete[](slots_, std::align_val_t(alignof(T)));
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Producer side only. Returns false if the queue is full.
    template <typename U>
    bool try_push(U&& item) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = tail + 1;

        // Check the cached head first; only read the shared atomic if it looks full.
        if (next - cached_head_ > capacity_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (next - cached_head_ > capacity_) {
                return false;
            }
        }

        ::new (&slots_[tail & mask_]) T(std::forward<U>(item));
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side only. Returns false if the queue is empty.
    bool try_pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);

        if (head == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head == cached_tail_) {
                return false;
            }
        }

        T* slot = &slots_[head & mask_];
        out = std::move(*slot);
        slot->~T();
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Approximate unless the caller owns both ends (e.g. a single-threaded test).
    std::size_t size() const noexcept {
        return tail_.load(std::memory_order_acquire) -
               head_.load(std::memory_order_acquire);
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    static std::size_t round_up_pow2(std::size_t n) {
        if (n < 2) return 2;
        --n;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        if constexpr (sizeof(std::size_t) == 8) {
            n |= n >> 32;
        }
        return n + 1;
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    T* slots_;

    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // producer line
    std::size_t cached_head_{0};

    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // consumer line
    std::size_t cached_tail_{0};

    char pad_[kCacheLine];
};

}  // namespace lfq
