// SPDX-License-Identifier: MIT
//
// Rebuilds the full-depth NASDAQ book from market-by-order ITCH messages.
//
// MBO semantics (ITCH 5.0 section 4):
//   A / F  add a displayed order            -> level += shares, orders += 1
//   E      execute part/all at order price  -> level -= executed
//   C      execute at a different price     -> same for the book; trade price differs
//   X      cancel part of an order          -> level -= cancelled
//   D      delete the rest of an order      -> level -= remaining, orders -= 1
//   U      replace: delete old ref, add new ref (same side and stock,
//          new price and size; loses time priority)
//   P / Q  trades against hidden orders / crosses: no visible-book change
//
// After every message the builder records whether that instrument's top of
// book (best bid/ask price or size) changed. That is the event a downstream
// strategy cares about, and what the pipeline forwards.
//
// Templated on the order store and the level structure so each can be
// benchmarked (and differential-tested) against a textbook implementation.
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "book/order_map.hpp"
#include "book/price_levels.hpp"
#include "itch/messages.hpp"

namespace book {

/// Best bid and offer of one instrument. Price 0 / qty 0 = empty side.
struct Top {
  std::uint32_t bid_price = 0;
  std::uint32_t ask_price = 0;
  std::uint64_t bid_shares = 0;
  std::uint64_t ask_shares = 0;
  friend bool operator==(const Top&, const Top&) = default;
};

/// Per-instrument reference data and trade statistics.
struct SymbolInfo {
  std::array<char, 8> name{};
  std::uint64_t traded_shares = 0;  ///< printable executions + hidden trades + crosses
  double traded_notional = 0;       ///< in dollars
  std::uint32_t open_cross_price = 0;
  std::uint32_t close_cross_price = 0;
  std::uint64_t close_cross_shares = 0;
  std::uint64_t top_changes = 0;
  std::uint64_t book_events = 0;

  [[nodiscard]] std::string_view symbol() const {
    std::string_view s(name.data(), name.size());
    return s.substr(0, s.find_last_not_of(' ') + 1);
  }
};

struct BookStats {
  std::uint64_t adds = 0, executions = 0, cancels = 0, deletes = 0, replaces = 0;
  std::uint64_t unknown_order = 0;  ///< E/C/X/D/U naming an order we never saw
  std::uint64_t level_mismatch = 0; ///< order's level missing: would mean a bug
  std::uint64_t peak_live_orders = 0;
};

template <class OrderMap, template <Side> class Levels>
class BookBuilder {
 public:
  static constexpr std::size_t kMaxLocate = 65536;  // stock_locate is a u16

  struct Book {
    Levels<Side::bid> bids;
    Levels<Side::ask> asks;
  };

  explicit BookBuilder(std::size_t expected_live_orders = 1 << 22)
      : orders_(expected_live_orders), books_(kMaxLocate), tops_(kMaxLocate),
        symbols_(kMaxLocate) {}

  // ---- reference data -------------------------------------------------------

  void on(const itch::StockDirectory& m) {
    auto& s = symbols_[m.header.locate()];
    std::copy(m.stock.chars.begin(), m.stock.chars.end(), s.name.begin());
  }

  // ---- order book events (market by order) ------------------------------------

  void on(const itch::AddOrder& m) {
    add(m.header.locate(), m.order_reference_number, side_of(m.buy_sell_indicator), m.shares,
        m.price);
  }
  void on(const itch::AddOrderMpid& m) {
    add(m.header.locate(), m.order_reference_number, side_of(m.buy_sell_indicator), m.shares,
        m.price);
  }

  void on(const itch::OrderExecuted& m) {
    ++stats_.executions;
    if (Order* o = lookup(m.order_reference_number)) {
      record_trade(o->locate, m.executed_shares, o->price);
      reduce(*o, m.executed_shares);
    }
  }

  void on(const itch::OrderExecutedWithPrice& m) {
    ++stats_.executions;
    if (Order* o = lookup(m.order_reference_number)) {
      if (m.printable == 'Y') record_trade(o->locate, m.executed_shares, m.execution_price);
      reduce(*o, m.executed_shares);
    }
  }

  void on(const itch::OrderCancel& m) {
    ++stats_.cancels;
    if (Order* o = lookup(m.order_reference_number)) reduce(*o, m.cancelled_shares);
  }

  void on(const itch::OrderDelete& m) {
    ++stats_.deletes;
    if (Order* o = lookup(m.order_reference_number)) reduce(*o, o->shares);
  }

  void on(const itch::OrderReplace& m) {
    ++stats_.replaces;
    Order* o = lookup(m.original_order_reference_number);
    if (o == nullptr) return;
    const std::uint16_t locate = o->locate;
    const Side side = o->side;
    remove_order(*o, o->shares);
    insert_order(Order{m.new_order_reference_number, m.price, m.shares, locate, side});
    finish(locate);
  }

  // ---- trades that do not touch the visible book ---------------------------------

  void on(const itch::Trade& m) {
    record_trade(m.header.locate(), m.shares, m.price);
    last_changed_ = false;
  }

  void on(const itch::CrossTrade& m) {
    auto& s = symbols_[m.header.locate()];
    record_trade(m.header.locate(), m.shares, m.cross_price);
    if (m.cross_type == 'O') s.open_cross_price = m.cross_price;
    if (m.cross_type == 'C') {
      s.close_cross_price = m.cross_price;
      s.close_cross_shares = m.shares;
    }
    last_changed_ = false;
  }

  // ---- queries ------------------------------------------------------------------

  /// Did the last book event change its instrument's best bid/offer?
  [[nodiscard]] bool top_changed() const noexcept { return last_changed_; }
  [[nodiscard]] std::uint16_t last_locate() const noexcept { return last_locate_; }
  [[nodiscard]] const Top& top(std::uint16_t locate) const noexcept { return tops_[locate]; }
  [[nodiscard]] const Book& book(std::uint16_t locate) const noexcept { return books_[locate]; }
  [[nodiscard]] const SymbolInfo& symbol(std::uint16_t locate) const noexcept {
    return symbols_[locate];
  }
  [[nodiscard]] const BookStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::size_t live_orders() const noexcept { return orders_.size(); }
  [[nodiscard]] std::size_t order_map_rehashes() const noexcept { return orders_.rehashes(); }

  /// Software prefetch for a message that will be applied a few messages from
  /// now. Starts the cache misses it will take: the instrument's book, top and
  /// statistics (every ITCH header carries stock_locate), and the order-table
  /// slots it will read or write. Pure hint: never changes state.
  void prefetch(const std::byte* msg, std::size_t len) const noexcept {
    if (len < 19) return;  // shortest order-referencing message is 19 bytes
    std::uint16_t loc;
    std::memcpy(&loc, msg + 1, 2);
    loc = std::byteswap(loc);
    __builtin_prefetch(&books_[loc], 0, 3);
    __builtin_prefetch(&tops_[loc], 1, 3);
    __builtin_prefetch(&symbols_[loc], 1, 3);
    // Every order message carries a reference number at byte 11: the order
    // being added (A F), or the existing order it acts on (E C X D U). An add's
    // slot is the most important one: it was last touched ~capacity orders
    // ago, so without the hint every insert is a cache miss (the top item in
    // the baseline flame graph). A replace also inserts its new reference.
    switch (static_cast<char>(msg[0])) {
      case 'U':
        if (len >= 27) {
          std::uint64_t fresh;
          std::memcpy(&fresh, msg + 19, 8);
          orders_.prefetch(std::byteswap(fresh));
        }
        [[fallthrough]];
      case 'A': case 'F': case 'E': case 'C': case 'X': case 'D': {
        std::uint64_t ref;
        std::memcpy(&ref, msg + 11, 8);
        orders_.prefetch(std::byteswap(ref));
        break;
      }
      default: break;
    }
  }

  /// Clears the "top changed" flag; the pipeline calls it before each message.
  void reset_change_flag() noexcept { last_changed_ = false; }

 private:
  static Side side_of(char c) noexcept { return c == 'B' ? Side::bid : Side::ask; }

  Order* lookup(std::uint64_t ref) {
    Order* o = orders_.find(ref);
    if (o == nullptr) [[unlikely]] ++stats_.unknown_order;
    return o;
  }

  void add(std::uint16_t locate, std::uint64_t ref, Side side, std::uint32_t shares,
           std::uint32_t price) {
    ++stats_.adds;
    insert_order(Order{ref, price, shares, locate, side});
    finish(locate);
  }

  void insert_order(const Order& o) {
    orders_.insert(o);
    if (orders_.size() > stats_.peak_live_orders) stats_.peak_live_orders = orders_.size();
    auto& b = books_[o.locate];
    if (o.side == Side::bid) b.bids.add(o.price, o.shares);
    else b.asks.add(o.price, o.shares);
  }

  /// Takes `shares` off a resting order; removes it when nothing is left.
  void reduce(Order& o, std::uint32_t shares) {
    const std::uint16_t locate = o.locate;
    if (shares >= o.shares) remove_order(o, o.shares);
    else {
      o.shares -= shares;
      level_remove(o, shares, false);
    }
    finish(locate);
  }

  void remove_order(Order& o, std::uint32_t remaining) {
    level_remove(o, remaining, true);
    orders_.erase(&o);
  }

  void level_remove(const Order& o, std::uint32_t shares, bool gone) {
    auto& b = books_[o.locate];
    const bool ok = o.side == Side::bid ? b.bids.remove(o.price, shares, gone)
                                        : b.asks.remove(o.price, shares, gone);
    if (!ok) [[unlikely]] ++stats_.level_mismatch;
  }

  /// Updates the cached top of book and the change flag for `locate`.
  void finish(std::uint16_t locate) {
    const auto& b = books_[locate];
    Top t;
    if (const Level* l = b.bids.best()) t.bid_price = l->price, t.bid_shares = l->shares;
    if (const Level* l = b.asks.best()) t.ask_price = l->price, t.ask_shares = l->shares;
    auto& s = symbols_[locate];
    ++s.book_events;
    last_locate_ = locate;
    last_changed_ = t != tops_[locate];
    if (last_changed_) {
      tops_[locate] = t;
      ++s.top_changes;
    }
  }

  void record_trade(std::uint16_t locate, std::uint64_t shares, std::uint32_t price) {
    auto& s = symbols_[locate];
    s.traded_shares += shares;
    s.traded_notional += static_cast<double>(shares) * price * itch::kPrice4Scale;
  }

  OrderMap orders_;
  std::vector<Book> books_;
  std::vector<Top> tops_;
  std::vector<SymbolInfo> symbols_;
  BookStats stats_;
  std::uint16_t last_locate_ = 0;
  bool last_changed_ = false;
};

/// The production configuration (chosen by measurement on real NASDAQ data,
/// see docs/RESULTS.md) and the textbook one it is differential-tested against.
using FastBook = BookBuilder<DirectOrderMap, LinearLevels>;
using ReferenceBook = BookBuilder<StdOrderMap, MapLevels>;

/// Compares every level of both sides of one instrument. Returns an empty
/// string if identical, else a description of the first difference.
template <class A, class B>
std::string diff_books(const A& a, const B& b, std::uint16_t locate) {
  auto collect = [](const auto& levels) {
    std::vector<Level> out;
    levels.for_each([&](const Level& l) {
      out.push_back(l);
      return true;
    });
    return out;
  };
  const auto ab = collect(a.book(locate).bids), bb = collect(b.book(locate).bids);
  const auto aa = collect(a.book(locate).asks), ba = collect(b.book(locate).asks);
  auto describe = [&](const char* side, const std::vector<Level>& x, const std::vector<Level>& y) {
    for (std::size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
      if (i >= x.size() || i >= y.size() || !(x[i] == y[i])) {
        return std::string(side) + " level " + std::to_string(i) + " differs (depth " +
               std::to_string(x.size()) + " vs " + std::to_string(y.size()) + ")";
      }
    }
    return std::string();
  };
  if (auto d = describe("bid", ab, bb); !d.empty()) return d;
  return describe("ask", aa, ba);
}

}  // namespace book
