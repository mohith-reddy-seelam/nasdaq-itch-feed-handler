// SPDX-License-Identifier: MIT
//
// One side of a price-level (L2) book, aggregated from individual orders.
//
// Real order flow is concentrated within a few ticks of the touch: most adds,
// cancels and executions hit the best few levels. So the level array is kept
// sorted with the BEST price at the BACK:
//
//   * best bid/ask is levels.back(): O(1);
//   * finding a price scans from the back, usually a handful of steps;
//   * inserting or erasing near the touch moves only the few elements after it.
//
// Variants (benchmarked against each other):
//   VectorLevels<Search::linear>  - the above (default)
//   VectorLevels<Search::binary>  - same array, std::lower_bound
//   MapLevels                     - std::map: O(log n), but a node per level
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "book/order_map.hpp"

namespace book {

struct Level {
  std::uint32_t price;
  std::uint32_t orders;
  std::uint64_t shares;
  friend bool operator==(const Level&, const Level&) = default;
};

enum class Search : std::uint8_t { linear, binary };

template <Side S, Search How = Search::linear>
class VectorLevels {
 public:
  VectorLevels() { levels_.reserve(64); }

  void add(std::uint32_t price, std::uint64_t shares) {
    const std::size_t i = upper(price);
    if (i > 0 && levels_[i - 1].price == price) {
      levels_[i - 1].shares += shares;
      ++levels_[i - 1].orders;
    } else {
      levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(i), Level{price, 1, shares});
    }
  }

  /// Removes `shares` at `price`; `order_gone` if the order left the book.
  /// Returns false if no such level exists (inconsistent feed).
  bool remove(std::uint32_t price, std::uint64_t shares, bool order_gone) {
    const std::size_t i = upper(price);
    if (i == 0 || levels_[i - 1].price != price) [[unlikely]] return false;
    Level& l = levels_[i - 1];
    l.shares -= shares;
    if (order_gone && --l.orders == 0) {
      levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(i - 1));
    }
    return true;
  }

  [[nodiscard]] const Level* best() const noexcept {
    return levels_.empty() ? nullptr : &levels_.back();
  }
  [[nodiscard]] std::size_t depth() const noexcept { return levels_.size(); }

  /// Visits levels best to worst; stops when f returns false.
  template <class F>
  void for_each(F&& f) const {
    for (auto it = levels_.rbegin(); it != levels_.rend(); ++it)
      if (!f(*it)) return;
  }

 private:
  /// True if price a is strictly better than price b for this side.
  static constexpr bool better(std::uint32_t a, std::uint32_t b) noexcept {
    return S == Side::bid ? a > b : a < b;
  }

  /// Index one past the last level that is not better than `price`; the level
  /// with exactly `price`, if any, is at index-1.
  [[nodiscard]] std::size_t upper(std::uint32_t price) const noexcept {
    if constexpr (How == Search::linear) {
      std::size_t i = levels_.size();
      while (i > 0 && better(levels_[i - 1].price, price)) --i;
      return i;
    } else {
      // Array is worst..best, i.e. ordered by !better; find first element
      // better than price.
      auto it = std::partition_point(levels_.begin(), levels_.end(), [price](const Level& l) {
        return !better(l.price, price);
      });
      return static_cast<std::size_t>(it - levels_.begin());
    }
  }

  std::vector<Level> levels_;  // worst ... best
};

template <Side S>
class MapLevels {
  using Cmp = std::conditional_t<S == Side::bid, std::greater<>, std::less<>>;

 public:
  void add(std::uint32_t price, std::uint64_t shares) {
    auto [it, inserted] = levels_.try_emplace(price, Level{price, 0, 0});
    it->second.shares += shares;
    ++it->second.orders;
  }

  bool remove(std::uint32_t price, std::uint64_t shares, bool order_gone) {
    auto it = levels_.find(price);
    if (it == levels_.end()) [[unlikely]] return false;
    it->second.shares -= shares;
    if (order_gone && --it->second.orders == 0) levels_.erase(it);
    return true;
  }

  [[nodiscard]] const Level* best() const noexcept {
    return levels_.empty() ? nullptr : &levels_.begin()->second;
  }
  [[nodiscard]] std::size_t depth() const noexcept { return levels_.size(); }

  template <class F>
  void for_each(F&& f) const {
    for (const auto& [px, l] : levels_)
      if (!f(l)) return;
  }

 private:
  std::map<std::uint32_t, Level, Cmp> levels_;  // best first
};

/// Uniform name for "a side implementation parameterised by Side".
template <template <Side> class T>
struct LevelsFamily {
  template <Side S>
  using type = T<S>;
};

template <Side S>
using LinearLevels = VectorLevels<S, Search::linear>;
template <Side S>
using BinaryLevels = VectorLevels<S, Search::binary>;

}  // namespace book
