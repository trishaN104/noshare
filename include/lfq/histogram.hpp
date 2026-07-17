// SPDX-License-Identifier: MIT
//
// Fixed-point latency histogram with O(1) record. Values are bucketed on a
// log2 scale with a configurable number of sub-buckets per power of two, which
// keeps relative error bounded (HdrHistogram-style) while staying tiny and
// dependency-free. Good enough to read p50/p99/p99.9 tails honestly.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lfq {

class Histogram {
public:
    // sub_bits controls resolution: 2^sub_bits linear buckets per octave.
    explicit Histogram(int sub_bits = 4)
        : sub_bits_(sub_bits), sub_count_(std::size_t{1} << sub_bits) {
        buckets_.assign(64 * sub_count_, 0);
    }

    void record(std::uint64_t value) {
        const std::size_t idx = bucket_index(value);
        ++buckets_[idx];
        ++count_;
        if (value > max_) max_ = value;
        sum_ += value;
    }

    std::uint64_t count() const { return count_; }
    std::uint64_t max() const { return max_; }
    double mean() const { return count_ ? double(sum_) / double(count_) : 0.0; }

    std::uint64_t percentile(double p) const {
        if (count_ == 0) return 0;
        const std::uint64_t target =
            std::min<std::uint64_t>(count_, std::uint64_t(p / 100.0 * count_ + 0.5));
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < buckets_.size(); ++i) {
            seen += buckets_[i];
            if (seen >= target && buckets_[i]) {
                return std::min(bucket_value(i), max_);
            }
        }
        return max_;
    }

private:
    std::size_t bucket_index(std::uint64_t value) const {
        if (value < sub_count_) return value;  // linear region near zero
        int octave = 63 - clz(value);
        const std::uint64_t sub =
            (value >> (octave - sub_bits_)) & (sub_count_ - 1);
        const std::size_t idx = std::size_t(octave) * sub_count_ + sub;
        return std::min(idx, buckets_.size() - 1);
    }

    // Upper (inclusive) bound of the bucket, so reported percentiles never
    // understate the true latency. Values in the linear region are exact.
    std::uint64_t bucket_value(std::size_t idx) const {
        if (idx < sub_count_) return idx;
        const std::size_t octave = idx / sub_count_;
        const std::uint64_t sub = idx % sub_count_;
        const std::uint64_t base = std::uint64_t{1} << octave;
        const std::uint64_t width = std::uint64_t{1} << (octave - sub_bits_);
        return base + (sub + 1) * width - 1;
    }

    static int clz(std::uint64_t x) {
        int n = 0;
        while (!(x & (std::uint64_t{1} << 63))) { x <<= 1; ++n; }
        return n;
    }

    int sub_bits_;
    std::size_t sub_count_;
    std::vector<std::uint64_t> buckets_;
    std::uint64_t count_{0};
    std::uint64_t max_{0};
    std::uint64_t sum_{0};
};

}  // namespace lfq
