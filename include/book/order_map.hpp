// SPDX-License-Identifier: MIT
//
// Live-order storage: order reference number -> resting order.
//
// Every E/C/X/D/U message names an order only by its 64-bit reference number,
// so this lookup is on the hot path of ~70 % of all ITCH traffic.
//
//   BasicFlatOrderMap<Hash> - open addressing, linear probing, backward-shift
//                  deletion (no tombstones, so probe lengths never degrade
//                  over a day of churn), one flat 24-byte entry per slot,
//                  preallocated: no allocation per order. Two hash policies:
//                  FibonacciHash (scatter) and IdentityHash (direct-mapped).
//   StdOrderMap  - std::unordered_map: a node allocation per order and a
//                  pointer chase per lookup. The baseline.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <cstdlib>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace book {

enum class Side : std::uint8_t { bid = 0, ask = 1 };

struct Order {
  std::uint64_t ref;
  std::uint32_t price;   ///< Price(4)
  std::uint32_t shares;  ///< remaining visible shares
  std::uint16_t locate;
  Side side;
};
static_assert(sizeof(Order) == 24);

/// Scatters keys uniformly: multiply by 2^64/phi, keep the top bits.
struct FibonacciHash {
  static std::size_t slot(std::uint64_t ref, std::size_t /*mask*/, int shift) noexcept {
    return static_cast<std::size_t>((ref * 0x9E37'79B9'7F4A'7C15ull) >> shift);
  }
};

/// Direct-mapped: slot = ref mod capacity. NASDAQ assigns reference numbers
/// nearly sequentially, and most book activity is on recently added orders,
/// so consecutive orders land in consecutive slots: the working set is a few
/// cache lines at the "end" of the table instead of random lines all over it.
struct IdentityHash {
  static std::size_t slot(std::uint64_t ref, std::size_t mask, int /*shift*/) noexcept {
    return static_cast<std::size_t>(ref) & mask;
  }
};

/// Page size for the order table. The table is hundreds of MB and lookups of
/// older orders land anywhere in it; with 4 KB pages most such lookups also
/// miss the TLB (a page walk on top of the cache miss). 2 MB pages cut the
/// number of pages 512x. Linux: transparent huge pages via madvise. Other
/// platforms: falls back to normal pages.
enum class Pages : std::uint8_t { normal, huge };

namespace detail {

inline constexpr std::size_t kHugePage = std::size_t{2} << 20;

template <class T>
struct TableDeleter {
  void operator()(T* p) const noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
  }
};

template <class T>
std::unique_ptr<T[], TableDeleter<T>> allocate_table(std::size_t n, Pages pages) {
  const std::size_t align = pages == Pages::huge ? kHugePage : 64;
  const std::size_t bytes = (n * sizeof(T) + align - 1) / align * align;
#if defined(_WIN32)
  void* p = _aligned_malloc(bytes, align);  // the Windows CRT has no aligned_alloc
#else
  void* p = std::aligned_alloc(align, bytes);
#endif
  if (p == nullptr) throw std::bad_alloc();
#if defined(__linux__)
  if (pages == Pages::huge) ::madvise(p, bytes, MADV_HUGEPAGE);
#endif
  return std::unique_ptr<T[], TableDeleter<T>>(static_cast<T*>(p));
}

}  // namespace detail

template <class Hash, Pages PageKind = Pages::huge>
class BasicFlatOrderMap {
 public:
  static constexpr std::uint64_t kEmpty = ~std::uint64_t{0};

  /// `expected_live` = peak number of simultaneously resting orders. The
  /// table is sized for a load factor of at most 1/2 at that peak.
  explicit BasicFlatOrderMap(std::size_t expected_live = 1 << 22)
      : capacity_(std::bit_ceil(std::max<std::size_t>(16, expected_live * 2))),
        mask_(capacity_ - 1),
        shift_(64 - std::countr_zero(capacity_)),
        slots_(detail::allocate_table<Order>(capacity_, PageKind)) {
    for (std::size_t i = 0; i < capacity_; ++i) slots_[i].ref = kEmpty;
  }

  /// Starts loading the slot `ref` hashes to, for a lookup a few messages from
  /// now (software pipelining: the miss overlaps with useful work).
  void prefetch(std::uint64_t ref) const noexcept {
    __builtin_prefetch(&slots_[home(ref)], 1 /*for write*/, 3);
  }

  [[nodiscard]] Order* find(std::uint64_t ref) noexcept {
    for (std::size_t i = home(ref);; i = (i + 1) & mask_) {
      Order& o = slots_[i];
      if (o.ref == ref) return &o;
      if (o.ref == kEmpty) return nullptr;
    }
  }

  /// Inserts a new order. Precondition: `ref` is not present (ITCH reference
  /// numbers are unique for the day).
  Order& insert(const Order& order) {
    if (size_ + 1 > capacity_ / 2) [[unlikely]] grow();
    std::size_t i = home(order.ref);
    while (slots_[i].ref != kEmpty) i = (i + 1) & mask_;
    slots_[i] = order;
    ++size_;
    return slots_[i];
  }

  /// Removes the entry `o` points to (a pointer returned by find()).
  void erase(Order* o) noexcept {
    std::size_t hole = static_cast<std::size_t>(o - slots_.get());
    // Backward-shift deletion: pull later members of the probe chain back
    // into the hole so lookups never need tombstones.
    for (std::size_t j = (hole + 1) & mask_;; j = (j + 1) & mask_) {
      const std::uint64_t ref = slots_[j].ref;
      if (ref == kEmpty) break;
      const std::size_t h = home(ref);
      // Move slot j into the hole iff its home is not in (hole, j] cyclically.
      const bool movable = hole <= j ? (h <= hole || h > j) : (h <= hole && h > j);
      if (movable) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole].ref = kEmpty;
    --size_;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t rehashes() const noexcept { return rehashes_; }

 private:
  [[nodiscard]] std::size_t home(std::uint64_t ref) const noexcept {
    return Hash::slot(ref, mask_, shift_);
  }

  void grow() {
    BasicFlatOrderMap bigger(capacity_);  // capacity_ live -> 2 * capacity_ slots
    for (std::size_t i = 0; i < capacity_; ++i)
      if (slots_[i].ref != kEmpty) bigger.insert(slots_[i]);
    bigger.rehashes_ = rehashes_ + 1;
    *this = std::move(bigger);
  }

  std::size_t capacity_;
  std::size_t mask_;
  int shift_;
  std::unique_ptr<Order[], detail::TableDeleter<Order>> slots_;
  std::size_t size_ = 0;
  std::size_t rehashes_ = 0;
};

using FlatOrderMap = BasicFlatOrderMap<FibonacciHash>;
using DirectOrderMap = BasicFlatOrderMap<IdentityHash>;
using DirectOrderMap4K = BasicFlatOrderMap<IdentityHash, Pages::normal>;

class StdOrderMap {
 public:
  explicit StdOrderMap(std::size_t expected_live = 1 << 22) { map_.reserve(expected_live); }

  [[nodiscard]] Order* find(std::uint64_t ref) {
    auto it = map_.find(ref);
    return it == map_.end() ? nullptr : &it->second;
  }
  Order& insert(const Order& order) { return map_.emplace(order.ref, order).first->second; }
  void prefetch(std::uint64_t) const noexcept {}
  void erase(Order* o) { map_.erase(o->ref); }

  [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
  [[nodiscard]] std::size_t rehashes() const noexcept { return 0; }

 private:
  std::unordered_map<std::uint64_t, Order> map_;
};

}  // namespace book
