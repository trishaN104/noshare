// SPDX-License-Identifier: MIT
//
// Cross-process SPSC transport over a named shared-memory region.
//
// The queue metadata (head/tail indices) and the ring buffer live together in
// one shared mapping so two separate processes can hand messages back and forth
// with no kernel involvement on the fast path. One process creates the region
// (owner), the other opens it. Element type must be trivially copyable, since
// it is memcpy'd across a process boundary with no shared vtables or pointers.
//
// Backends:
//   POSIX  : shm_open + ftruncate + mmap        (link with -lrt on Linux)
//   Windows: CreateFileMapping + MapViewOfFile
//
// Portability note: this is written to compile on both platforms, but the
// POSIX path is the one meant to be benchmarked on an isolated Linux box.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lfq {

// Layout placed at the start of the shared region. Kept POD and index-based so
// it is valid regardless of where each process maps it.
struct ShmHeader {
    std::atomic<std::uint64_t> head;
    std::atomic<std::uint64_t> tail;
    std::uint64_t capacity;   // power of two
    std::uint64_t elem_size;
};

template <typename T>
class ShmSpscChannel {
    static_assert(std::is_trivially_copyable_v<T>,
                  "shared-memory element type must be trivially copyable");

public:
    // Create (owner) or open an existing channel by name. Capacity is rounded
    // to a power of two; it is ignored when opening (taken from the header).
    static ShmSpscChannel create(const std::string& name, std::size_t capacity) {
        return ShmSpscChannel(name, round_up_pow2(capacity), /*owner=*/true);
    }
    static ShmSpscChannel open(const std::string& name) {
        return ShmSpscChannel(name, 0, /*owner=*/false);
    }

    ShmSpscChannel(ShmSpscChannel&& o) noexcept { move_from(o); }
    ShmSpscChannel& operator=(ShmSpscChannel&& o) noexcept {
        if (this != &o) { close(); move_from(o); }
        return *this;
    }
    ShmSpscChannel(const ShmSpscChannel&) = delete;
    ShmSpscChannel& operator=(const ShmSpscChannel&) = delete;
    ~ShmSpscChannel() { close(); }

    bool try_push(const T& item) {
        const std::uint64_t tail = hdr_->tail.load(std::memory_order_relaxed);
        const std::uint64_t head = hdr_->head.load(std::memory_order_acquire);
        if (tail - head >= hdr_->capacity) return false;  // full
        std::memcpy(slot(tail & mask_), &item, sizeof(T));
        hdr_->tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        const std::uint64_t head = hdr_->head.load(std::memory_order_relaxed);
        const std::uint64_t tail = hdr_->tail.load(std::memory_order_acquire);
        if (head == tail) return false;  // empty
        std::memcpy(&out, slot(head & mask_), sizeof(T));
        hdr_->head.store(head + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept {
        return static_cast<std::size_t>(hdr_->capacity);
    }

private:
    ShmSpscChannel(const std::string& name, std::size_t capacity, bool owner)
        : name_(name), owner_(owner) {
        map(name, capacity, owner);
        if (owner) {
            hdr_->capacity = capacity;
            hdr_->elem_size = sizeof(T);
            hdr_->head.store(0, std::memory_order_relaxed);
            hdr_->tail.store(0, std::memory_order_relaxed);
        } else if (hdr_->elem_size != sizeof(T)) {
            throw std::runtime_error("shm channel element size mismatch");
        }
        mask_ = hdr_->capacity - 1;
    }

    unsigned char* slot(std::uint64_t index) {
        return data_ + index * sizeof(T);
    }

    static std::size_t region_bytes(std::size_t capacity) {
        return sizeof(ShmHeader) + capacity * sizeof(T);
    }

    static std::size_t round_up_pow2(std::size_t n) {
        if (n < 2) return 2;
        --n;
        n |= n >> 1; n |= n >> 2; n |= n >> 4;
        n |= n >> 8; n |= n >> 16;
        if constexpr (sizeof(std::size_t) == 8) n |= n >> 32;
        return n + 1;
    }

#if defined(_WIN32)
    void map(const std::string& name, std::size_t capacity, bool owner) {
        const std::string obj = "Local\\lfq_" + name;
        if (owner) {
            const std::size_t bytes = region_bytes(capacity);
            handle_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                         PAGE_READWRITE,
                                         static_cast<DWORD>(bytes >> 32),
                                         static_cast<DWORD>(bytes & 0xFFFFFFFF),
                                         obj.c_str());
        } else {
            handle_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, obj.c_str());
        }
        if (!handle_) throw std::runtime_error("CreateFileMapping/OpenFileMapping failed");
        base_ = MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!base_) throw std::runtime_error("MapViewOfFile failed");
        hdr_ = reinterpret_cast<ShmHeader*>(base_);
        data_ = reinterpret_cast<unsigned char*>(base_) + sizeof(ShmHeader);
    }

    void close() {
        if (base_) { UnmapViewOfFile(base_); base_ = nullptr; }
        if (handle_) { CloseHandle(handle_); handle_ = nullptr; }
        hdr_ = nullptr; data_ = nullptr;
    }

    void move_from(ShmSpscChannel& o) {
        handle_ = o.handle_; base_ = o.base_; hdr_ = o.hdr_; data_ = o.data_;
        mask_ = o.mask_; name_ = std::move(o.name_); owner_ = o.owner_;
        o.handle_ = nullptr; o.base_ = nullptr; o.hdr_ = nullptr; o.data_ = nullptr;
    }

    HANDLE handle_{nullptr};
    void* base_{nullptr};
#else
    void map(const std::string& name, std::size_t capacity, bool owner) {
        shm_name_ = "/lfq_" + name;
        const int flags = owner ? (O_CREAT | O_RDWR) : O_RDWR;
        fd_ = shm_open(shm_name_.c_str(), flags, 0600);
        if (fd_ < 0) throw std::runtime_error("shm_open failed");

        std::size_t bytes;
        if (owner) {
            bytes = region_bytes(capacity);
            if (ftruncate(fd_, static_cast<off_t>(bytes)) != 0)
                throw std::runtime_error("ftruncate failed");
        } else {
            struct stat st{};
            if (fstat(fd_, &st) != 0) throw std::runtime_error("fstat failed");
            bytes = static_cast<std::size_t>(st.st_size);
        }
        mapped_bytes_ = bytes;
        base_ = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (base_ == MAP_FAILED) throw std::runtime_error("mmap failed");
        hdr_ = reinterpret_cast<ShmHeader*>(base_);
        data_ = reinterpret_cast<unsigned char*>(base_) + sizeof(ShmHeader);
    }

    void close() {
        if (base_ && base_ != MAP_FAILED) { munmap(base_, mapped_bytes_); base_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        if (owner_ && !shm_name_.empty()) shm_unlink(shm_name_.c_str());
        hdr_ = nullptr; data_ = nullptr;
    }

    void move_from(ShmSpscChannel& o) {
        fd_ = o.fd_; base_ = o.base_; hdr_ = o.hdr_; data_ = o.data_;
        mask_ = o.mask_; mapped_bytes_ = o.mapped_bytes_;
        name_ = std::move(o.name_); shm_name_ = std::move(o.shm_name_);
        owner_ = o.owner_;
        o.fd_ = -1; o.base_ = nullptr; o.hdr_ = nullptr; o.data_ = nullptr;
    }

    int fd_{-1};
    void* base_{nullptr};
    std::size_t mapped_bytes_{0};
    std::string shm_name_;
#endif

    ShmHeader* hdr_{nullptr};
    unsigned char* data_{nullptr};
    std::uint64_t mask_{0};
    std::string name_;
    bool owner_{false};
};

}  // namespace lfq
