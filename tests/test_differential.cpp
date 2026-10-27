// SPDX-License-Identifier: MIT
//
// Differential testing: the optimised book (flat hash map + vector levels)
// must produce exactly the same book as a textbook one (std::unordered_map +
// std::map), message by message, on a synthetic feed and, if available, on a
// real NASDAQ capture (set ITCH_FILE=/path/to/file.NASDAQ_ITCH50.bin).
#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <set>
#include <string>

#include "book/book_builder.hpp"
#include "itch/parser.hpp"
#include "itch/synthetic.hpp"
#include "mapped_file.hpp"

namespace {

using namespace itch;
using BinaryBook = book::BookBuilder<book::FlatOrderMap, book::BinaryLevels>;
using FibBook = book::BookBuilder<book::FlatOrderMap, book::LinearLevels>;

/// Feeds every message to four builders; after each one, checks the touched
/// instrument's top of book agrees. At the end, compares full depth.
void check_equivalent(std::span<const std::byte> feed, std::uint64_t limit) {
  auto fast = std::make_unique<book::FastBook>(1 << 20);
  auto binary = std::make_unique<BinaryBook>(1 << 20);
  auto direct = std::make_unique<FibBook>(1 << 20);
  auto ref = std::make_unique<book::ReferenceBook>(1 << 20);
  std::set<std::uint16_t> touched;

  FrameReader reader(feed);
  std::uint64_t n = 0;
  for (auto msg = reader.next(); !msg.empty() && (limit == 0 || n < limit);
       msg = reader.next(), ++n) {
    fast->reset_change_flag();
    ref->reset_change_flag();
    dispatch(msg.data(), msg.size(), *fast);
    dispatch(msg.data(), msg.size(), *binary);
    dispatch(msg.data(), msg.size(), *direct);
    dispatch(msg.data(), msg.size(), *ref);
    ASSERT_EQ(fast->top_changed(), ref->top_changed()) << "message " << n;
    const auto loc = ref->last_locate();
    touched.insert(loc);
    ASSERT_EQ(fast->top(loc), ref->top(loc)) << "message " << n << " locate " << loc;
    ASSERT_EQ(binary->top(loc), ref->top(loc)) << "message " << n << " locate " << loc;
    ASSERT_EQ(direct->top(loc), ref->top(loc)) << "message " << n << " locate " << loc;
  }
  for (auto loc : touched) {
    ASSERT_EQ(book::diff_books(*fast, *ref, loc), "") << "locate " << loc;
    ASSERT_EQ(book::diff_books(*binary, *ref, loc), "") << "locate " << loc;
    ASSERT_EQ(book::diff_books(*direct, *ref, loc), "") << "locate " << loc;
  }
  EXPECT_EQ(fast->live_orders(), ref->live_orders());
  EXPECT_EQ(fast->stats().level_mismatch, 0u);
  EXPECT_EQ(ref->stats().level_mismatch, 0u);
  std::printf("[ checked  ] %llu messages, %zu instruments, %zu live orders at end\n",
              static_cast<unsigned long long>(n), touched.size(), ref->live_orders());
}

TEST(Differential, SyntheticFeed) {
  const auto feed = synthetic::make_feed({.messages = 2'000'000, .symbols = 300, .seed = 11});
  check_equivalent(feed, 0);
}

TEST(Differential, RealNasdaqCapture) {
  const char* path = std::getenv("ITCH_FILE");
  if (path == nullptr) GTEST_SKIP() << "set ITCH_FILE to a NASDAQ ITCH 5.0 BinaryFILE capture";
  const std::uint64_t limit =
      std::getenv("ITCH_DIFF_LIMIT") ? std::stoull(std::getenv("ITCH_DIFF_LIMIT")) : 20'000'000;
  harness::MappedFile file(path);
  check_equivalent(file.bytes(), limit);
}

}  // namespace
