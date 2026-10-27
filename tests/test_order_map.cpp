// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <random>
#include <unordered_map>
#include <vector>

#include "book/order_map.hpp"

namespace {

using book::Order;

template <class Map>
class OrderMapTest : public ::testing::Test {};
using Maps = ::testing::Types<book::FlatOrderMap, book::DirectOrderMap>;
TYPED_TEST_SUITE(OrderMapTest, Maps);

Order order(std::uint64_t ref) {
  return Order{ref, static_cast<std::uint32_t>(ref * 3), static_cast<std::uint32_t>(ref % 1000),
               static_cast<std::uint16_t>(ref % 7), book::Side::bid};
}

TYPED_TEST(OrderMapTest, InsertFindErase) {
  TypeParam m(16);
  m.insert(order(5));
  m.insert(order(9));
  ASSERT_NE(m.find(5), nullptr);
  EXPECT_EQ(m.find(5)->price, 15u);
  EXPECT_EQ(m.find(6), nullptr);
  m.erase(m.find(5));
  EXPECT_EQ(m.find(5), nullptr);
  EXPECT_NE(m.find(9), nullptr);
  EXPECT_EQ(m.size(), 1u);
}

// Random inserts and erases against std::unordered_map as the model. A small
// table forces long probe chains, wrap-around at the end of the array, and
// backward-shift deletions in the middle of chains.
TYPED_TEST(OrderMapTest, MatchesModelUnderChurn) {
  TypeParam m(64);
  std::unordered_map<std::uint64_t, Order> model;
  std::vector<std::uint64_t> live;
  std::mt19937_64 rng(7);
  std::uint64_t next = 1;
  for (int step = 0; step < 200'000; ++step) {
    if (live.size() < 20 || rng() % 100 < 50) {
      // Mostly sequential, like ITCH reference numbers, with some jumps.
      if (rng() % 10 == 0) next += 1000;
      const std::uint64_t ref = next++;  // unique, as ITCH guarantees
      m.insert(order(ref));
      model.emplace(ref, order(ref));
      live.push_back(ref);
    } else {
      const std::size_t i = rng() % live.size();
      Order* o = m.find(live[i]);
      ASSERT_NE(o, nullptr) << "lost key " << live[i] << " at step " << step;
      m.erase(o);
      model.erase(live[i]);
      live[i] = live.back();
      live.pop_back();
    }
    ASSERT_EQ(m.size(), model.size());
  }
  for (const auto& [ref, o] : model) {
    const Order* f = m.find(ref);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->price, o.price);
  }
  for (std::uint64_t ref = next; ref < next + 1000; ++ref) EXPECT_EQ(m.find(ref), nullptr);
}

TYPED_TEST(OrderMapTest, GrowsPastExpectedSizeWithoutLosingOrders) {
  TypeParam m(8);  // 16 slots
  const std::size_t start = m.capacity();
  for (std::uint64_t r = 1; r <= 10'000; ++r) m.insert(order(r));
  EXPECT_GT(m.capacity(), start);
  EXPECT_GT(m.rehashes(), 0u);
  for (std::uint64_t r = 1; r <= 10'000; ++r) ASSERT_NE(m.find(r), nullptr);
}

}  // namespace
