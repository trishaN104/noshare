// SPDX-License-Identifier: MIT
//
// Bounded lock-free MPSC queue. Many producers, one consumer.
//
// Each slot carries a sequence counter (Vyukov cell scheme). Producers claim a
// slot by CAS-advancing the enqueue index; the single consumer never contends,
// so it advances the dequeue index with a plain store. Standard C++20.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace lfq {

#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t kMpscCacheLine = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kMpscCacheLine = 64;
#endif

template <typename T>
class MpscQueue {
    static_assert(std::is_nothrow_destructible_v<T>,
                  "T must be nothrow-destructible");

public:
    explicit MpscQueue(std::size_t capacity)
        : capacity_(round_up_pow2(capacity)),
          mask_(capacity_ - 1),
          slots_(static_cast<Cell*>(::operator new[](
              capacity_ * sizeof(Cell), std::align_val_t(alignof(Cell))))) {
        for (std::size_t i = 0; i < capacity_; ++i) {
            ::new (&slots_[i]) Cell();
            slots_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    ~MpscQueue() {
        T tmp;
        while (try_pop(tmp)) {
        }
        for (std::size_t i = 0; i < capacity_; ++i) slots_[i].~Cell();
        ::operator delete[](slots_, std::align_val_t(alignof(Cell)));
    }

    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    template <typename U>
    bool try_push(U&& item) {
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        Cell* cell;
        for (;;) {
            cell = &slots_[pos & mask_];
            const std::size_t seq = cell->seq.load(std::memory_order_acquire);
            const std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        ::new (&cell->storage) T(std::forward<U>(item));
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        const std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        Cell* cell = &slots_[pos & mask_];
        const std::size_t seq = cell->seq.load(std::memory_order_acquire);
        const std::intptr_t diff =
            static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
        if (diff != 0) return false;  // empty

        T* slot = reinterpret_cast<T*>(&cell->storage);
        out = std::move(*slot);
        slot->~T();
        cell->seq.store(pos + capacity_, std::memory_order_release);
        dequeue_pos_.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    struct Cell {
        std::atomic<std::size_t> seq;
        alignas(T) unsigned char storage[sizeof(T)];
    };

    static std::size_t round_up_pow2(std::size_t n) {
        if (n < 2) return 2;
        --n;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        if constexpr (sizeof(std::size_t) == 8) n |= n >> 32;
        return n + 1;
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    Cell* slots_;

    alignas(kMpscCacheLine) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(kMpscCacheLine) std::atomic<std::size_t> dequeue_pos_{0};
    char pad_[kMpscCacheLine];
};

}  // namespace lfq
