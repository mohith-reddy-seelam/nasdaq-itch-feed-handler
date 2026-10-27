// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "book/book_builder.hpp"
#include "itch/encode.hpp"
#include "itch/parser.hpp"

namespace {

using namespace itch;
using book::FastBook;

constexpr std::uint16_t kLoc = 7;

/// Applies one message to `b` through the real dispatch path.
template <class Book, class M>
void apply(Book& b, const M& m) {
  b.reset_change_flag();
  ASSERT_EQ(dispatch(reinterpret_cast<const std::byte*>(&m), sizeof(M), b), Status::ok);
}

TEST(Book, AddsAggregateIntoLevels) {
  FastBook b(1024);
  apply(b, encode::add_order(kLoc, 1, 1, 'B', 100, "X", 100'0000));
  apply(b, encode::add_order(kLoc, 1, 2, 'B', 200, "X", 100'0000));
  apply(b, encode::add_order(kLoc, 1, 3, 'B', 300, "X", 99'9900));
  apply(b, encode::add_order(kLoc, 1, 4, 'S', 500, "X", 100'0100));
  const auto& t = b.top(kLoc);
  EXPECT_EQ(t.bid_price, 100'0000u);
  EXPECT_EQ(t.bid_shares, 300u);
  EXPECT_EQ(t.ask_price, 100'0100u);
  EXPECT_EQ(t.ask_shares, 500u);
  EXPECT_EQ(b.book(kLoc).bids.depth(), 2u);
  EXPECT_EQ(b.book(kLoc).bids.best()->orders, 2u);
}

TEST(Book, ExecutionsCancelsAndDeletesShrinkThenRemove) {
  FastBook b(1024);
  apply(b, encode::add_order(kLoc, 1, 1, 'S', 300, "X", 50'0000));
  apply(b, encode::add_order(kLoc, 1, 2, 'S', 100, "X", 50'0100));

  apply(b, encode::executed(kLoc, 2, 1, 100, 1));
  EXPECT_EQ(b.top(kLoc).ask_shares, 200u);
  EXPECT_TRUE(b.top_changed());

  apply(b, encode::cancel(kLoc, 3, 1, 50));
  EXPECT_EQ(b.top(kLoc).ask_shares, 150u);

  apply(b, encode::executed(kLoc, 4, 1, 150, 2));  // fully filled: level gone
  EXPECT_EQ(b.top(kLoc).ask_price, 50'0100u);
  EXPECT_EQ(b.live_orders(), 1u);

  apply(b, encode::del(kLoc, 5, 2));
  EXPECT_EQ(b.top(kLoc).ask_price, 0u);
  EXPECT_EQ(b.book(kLoc).asks.depth(), 0u);
  EXPECT_EQ(b.live_orders(), 0u);
}

TEST(Book, ReplaceMovesOrderAndKeepsSide) {
  FastBook b(1024);
  apply(b, encode::add_order(kLoc, 1, 10, 'B', 100, "X", 20'0000));
  apply(b, encode::replace(kLoc, 2, 10, 11, 400, 20'0200));
  EXPECT_EQ(b.top(kLoc).bid_price, 20'0200u);
  EXPECT_EQ(b.top(kLoc).bid_shares, 400u);
  EXPECT_EQ(b.book(kLoc).bids.depth(), 1u);
  apply(b, encode::del(kLoc, 3, 10));  // old reference is gone
  EXPECT_EQ(b.stats().unknown_order, 1u);
  apply(b, encode::del(kLoc, 3, 11));
  EXPECT_EQ(b.live_orders(), 0u);
}

TEST(Book, TopChangedOnlyWhenBestLevelMoves) {
  FastBook b(1024);
  apply(b, encode::add_order(kLoc, 1, 1, 'B', 100, "X", 10'0000));
  EXPECT_TRUE(b.top_changed());
  apply(b, encode::add_order(kLoc, 1, 2, 'B', 100, "X", 9'0000));  // behind the touch
  EXPECT_FALSE(b.top_changed());
  apply(b, encode::add_order(kLoc, 1, 3, 'B', 100, "X", 10'0000));  // joins the touch
  EXPECT_TRUE(b.top_changed());
  apply(b, encode::trade(kLoc, 1, 'B', 500, "X", 10'0000, 1));  // hidden: book unchanged
  EXPECT_FALSE(b.top_changed());
}

TEST(Book, TradeStatisticsAndClosingCross) {
  FastBook b(1024);
  apply(b, encode::stock_directory(kLoc, 0, "AAPL"));
  apply(b, encode::add_order(kLoc, 1, 1, 'S', 300, "AAPL", 291'0000));
  apply(b, encode::executed(kLoc, 2, 1, 100, 1));                         // 100 @ 291.00
  apply(b, encode::trade(kLoc, 3, 'B', 100, "AAPL", 292'0000, 2));         // 100 @ 292.00
  apply(b, encode::cross(kLoc, 4, 1000, "AAPL", 291'5200, 3, 'C'));        // close cross
  const auto& s = b.symbol(kLoc);
  EXPECT_EQ(s.symbol(), "AAPL");
  EXPECT_EQ(s.traded_shares, 1200u);
  EXPECT_NEAR(s.traded_notional, 100 * 291.0 + 100 * 292.0 + 1000 * 291.52, 1e-6);
  EXPECT_EQ(s.close_cross_price, 291'5200u);
  EXPECT_EQ(s.close_cross_shares, 1000u);
}

// The three level structures must agree exactly under random operations.
TEST(Levels, LinearBinaryAndMapAgree) {
  book::VectorLevels<book::Side::ask, book::Search::linear> lin;
  book::VectorLevels<book::Side::ask, book::Search::binary> bin;
  book::MapLevels<book::Side::ask> map;
  struct O {
    std::uint32_t px, qty;
  };
  std::vector<O> live;
  std::mt19937 rng(3);
  for (int i = 0; i < 100'000; ++i) {
    if (live.empty() || rng() % 2) {
      const O o{static_cast<std::uint32_t>(100 + rng() % 40),
                static_cast<std::uint32_t>(1 + rng() % 9)};
      lin.add(o.px, o.qty);
      bin.add(o.px, o.qty);
      map.add(o.px, o.qty);
      live.push_back(o);
    } else {
      const std::size_t k = rng() % live.size();
      ASSERT_TRUE(lin.remove(live[k].px, live[k].qty, true));
      ASSERT_TRUE(bin.remove(live[k].px, live[k].qty, true));
      ASSERT_TRUE(map.remove(live[k].px, live[k].qty, true));
      live[k] = live.back();
      live.pop_back();
    }
    ASSERT_EQ(lin.depth(), map.depth());
    ASSERT_EQ(bin.depth(), map.depth());
    if (map.best()) {
      ASSERT_EQ(*lin.best(), *map.best());
      ASSERT_EQ(*bin.best(), *map.best());
    }
  }
}

}  // namespace
