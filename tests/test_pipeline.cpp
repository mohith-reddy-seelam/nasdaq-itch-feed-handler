// SPDX-License-Identifier: MIT
//
// The threaded pipeline (with 1 and 3 book shards) must end with exactly the
// same books, and see exactly the same top-of-book changes, as a plain
// single-threaded replay of the same feed.
#include <gtest/gtest.h>

#include <memory>

#include "book/book_builder.hpp"
#include "feed/pipeline.hpp"
#include "itch/parser.hpp"
#include "itch/synthetic.hpp"

namespace {

void check_against_single_thread(std::size_t shards) {
  const auto feed = itch::synthetic::make_feed({.messages = 400'000, .symbols = 97, .seed = 5});

  auto reference = std::make_unique<book::FastBook>(1 << 16);
  std::uint64_t ref_top_changes = 0;
  {
    itch::FrameReader r(feed);
    for (auto m = r.next(); !m.empty(); m = r.next()) {
      reference->reset_change_flag();
      itch::dispatch(m.data(), m.size(), *reference);
      ref_top_changes += reference->top_changed();
    }
  }

  feed::Pipeline<> pipe(shards, 1 << 16);
  feed::PipelineConfig cfg;
  cfg.feed_cpu = cfg.strategy_cpu = -1;  // unpinned: tests may run on any machine
  cfg.book_cpus.assign(shards, -1);
  const auto result = pipe.run(feed, cfg);

  EXPECT_EQ(result.replay.unknown_type + result.replay.bad_length, 0u);
  EXPECT_EQ(result.top_events, ref_top_changes);
  const auto view = pipe.book();
  EXPECT_EQ(view.stats().unknown_order, 0u);
  EXPECT_EQ(view.stats().level_mismatch, 0u);
  for (std::uint16_t loc = 1; loc <= 97; ++loc) {
    ASSERT_EQ(view.top(loc), reference->top(loc)) << "locate " << loc;
    ASSERT_EQ(book::diff_books(view.shard(loc), *reference, loc), "") << "locate " << loc;
  }
  if (shards > 1) {
    std::uint64_t total = 0;
    for (auto m : result.shard_messages) total += m;
    EXPECT_EQ(total, result.replay.messages);
  }
}

TEST(Pipeline, SingleShardMatchesSingleThread) { check_against_single_thread(1); }
TEST(Pipeline, ThreeShardsMatchSingleThread) { check_against_single_thread(3); }

}  // namespace
