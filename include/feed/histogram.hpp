// SPDX-License-Identifier: MIT
//
// Fixed-size log-linear latency histogram (HdrHistogram-style).
//
// A full NASDAQ day is ~400 M messages; keeping one 4-byte sample each would
// be 1.6 GB per measured stage. Instead each value (in TSC cycles) goes into
// one of 32 sub-buckets per power of two: 2.2 % relative resolution, constant
// 10 KB of memory, and record() is a few instructions with no branches on the
// data path besides the clamp.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <ostream>

namespace feed {

class LogHistogram {
 public:
  static constexpr int kSubBits = 5;  // 32 sub-buckets per doubling
  static constexpr int kSub = 1 << kSubBits;
  static constexpr int kMaxExp = 44;  // values up to 2^44 cycles (~2 hours)
  // Index (e - kSubBits + 1) * kSub + sub for e in [kSubBits, kMaxExp], plus the
  // kSub linear buckets below 2^kSubBits.
  static constexpr int kBuckets = (kMaxExp - kSubBits + 2) * kSub;

  void record(std::uint64_t v) noexcept {
    ++counts_[index(v)];
    ++total_;
    if (v > max_) max_ = v;
  }

  void merge(const LogHistogram& o) noexcept {
    for (int i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
    total_ += o.total_;
    if (o.max_ > max_) max_ = o.max_;
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return total_; }
  [[nodiscard]] std::uint64_t max() const noexcept { return max_; }

  /// Upper bound of the bucket holding the q-quantile (nearest rank), so the
  /// reported value is never an underestimate. q = 1 returns the exact max.
  [[nodiscard]] std::uint64_t quantile(double q) const noexcept {
    if (total_ == 0) return 0;
    if (q >= 1.0) return max_;
    const auto rank = static_cast<std::uint64_t>(q * static_cast<double>(total_)) + 1;
    std::uint64_t seen = 0;
    for (int i = 0; i < kBuckets; ++i) {
      seen += counts_[i];
      if (seen >= rank) return std::min(upper(i), max_);
    }
    return max_;
  }

  /// CSV rows "bucket_lo,bucket_hi,count" for non-empty buckets, scaled by
  /// `scale` (e.g. 1/GHz to convert cycles to ns).
  void write_csv(std::ostream& os, double scale) const {
    os << "bucket_lo_ns,bucket_hi_ns,count\n";
    for (int i = 0; i < kBuckets; ++i)
      if (counts_[i] != 0)
        os << lower(i) * scale << ',' << upper(i) * scale << ',' << counts_[i] << '\n';
  }

 private:
  static int index(std::uint64_t v) noexcept {
    if (v < kSub) return static_cast<int>(v);
    const int e = std::bit_width(v) - 1;  // floor(log2 v) >= kSubBits
    if (e > kMaxExp) return kBuckets - 1;
    const int sub = static_cast<int>((v >> (e - kSubBits)) & (kSub - 1));
    return (e - kSubBits + 1) * kSub + sub;
  }
  static std::uint64_t lower(int i) noexcept {
    if (i < kSub) return static_cast<std::uint64_t>(i);
    const int e = i / kSub + kSubBits - 1;
    return (std::uint64_t{kSub} + (i % kSub)) << (e - kSubBits);
  }
  static std::uint64_t upper(int i) noexcept {
    if (i < kSub) return static_cast<std::uint64_t>(i);
    const int e = i / kSub + kSubBits - 1;
    return ((std::uint64_t{kSub} + (i % kSub) + 1) << (e - kSubBits)) - 1;
  }

  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t total_ = 0;
  std::uint64_t max_ = 0;
};

}  // namespace feed
