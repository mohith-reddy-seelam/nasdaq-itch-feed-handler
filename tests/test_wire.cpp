// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "itch/encode.hpp"
#include "itch/messages.hpp"
#include "itch/parser.hpp"

namespace {

using namespace itch;

std::vector<std::byte> bytes(std::initializer_list<int> v) {
  std::vector<std::byte> out;
  for (int b : v) out.push_back(static_cast<std::byte>(b));
  return out;
}

TEST(Wire, BigEndianDecode) {
  const auto b = bytes({0x12, 0x34, 0x56, 0x78});
  EXPECT_EQ(std::start_lifetime_as<be32>(b.data())->value(), 0x12345678u);
  EXPECT_EQ(be64::from(0x0102030405060708ull).value(), 0x0102030405060708ull);
  EXPECT_EQ((BigEndian<std::uint64_t, 6>::from(0xAABBCCDDEEFFull).value()), 0xAABBCCDDEEFFull);
}

TEST(Wire, AlphaTrimsTrailingSpaces) {
  EXPECT_EQ(Stock::from("AAPL").view(), "AAPL");
  EXPECT_EQ(Stock::from("BRK A").view(), "BRK A");
  EXPECT_EQ(Stock::from("").view(), "");
}

// Hand-assembled from the byte offsets in the ITCH 5.0 specification, not
// from our encoder, so it checks the overlay layout independently.
TEST(Wire, AddOrderGoldenBytes) {
  const auto raw = bytes({
      'A',                                // 0  message type
      0x00, 0x0D,                         // 1  stock locate = 13
      0x00, 0x00,                         // 3  tracking number
      0x1F, 0x1A, 0x8C, 0x42, 0x63, 0x00, // 5  timestamp (6 bytes)
      0, 0, 0, 0, 0, 0, 0x30, 0x39,       // 11 order reference = 12345
      'B',                                // 19 buy/sell
      0x00, 0x00, 0x01, 0x2C,             // 20 shares = 300
      'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ', // 24 stock
      0x00, 0x2C, 0x7B, 0x5C,             // 32 price = 2915164 -> $291.5164
  });
  ASSERT_EQ(raw.size(), sizeof(AddOrder));
  const auto& m = *std::start_lifetime_as<AddOrder>(raw.data());
  EXPECT_EQ(m.header.message_type.value(), 'A');
  EXPECT_EQ(m.header.locate(), 13);
  EXPECT_EQ(m.header.timestamp(), 0x1F1A8C426300ull);
  EXPECT_EQ(m.order_reference_number.value(), 12345u);
  EXPECT_EQ(m.buy_sell_indicator.value(), 'B');
  EXPECT_EQ(m.shares.value(), 300u);
  EXPECT_EQ(m.stock.view(), "AAPL");
  EXPECT_EQ(m.price.value(), 2915164u);
}

TEST(Wire, TimestampUsesAllSixBytes) {
  const std::uint64_t ts = 57'599'999'999'999ull;  // 15:59:59.999999999
  const auto h = encode::header('S', 0, ts, 0xFFFF);  // tracking must not leak in
  EXPECT_EQ(h.timestamp(), ts);
}

TEST(Wire, LengthTableCoversAllTypes) {
  EXPECT_EQ(kMessageLength['A'], 36);
  EXPECT_EQ(kMessageLength['P'], 44);
  EXPECT_EQ(kMessageLength['I'], 50);
  EXPECT_EQ(kMessageLength['Z'], 0);
  int known = 0;
  for (auto len : kMessageLength) known += len != 0;
  EXPECT_EQ(known, 23);
}

/// Records which handlers were called.
struct Recorder {
  std::vector<char> seen;
  std::uint64_t last_ref = 0;
  void on(const AddOrder& m) {
    seen.push_back('A');
    last_ref = m.order_reference_number;
  }
  void on(const OrderReplace& m) {
    seen.push_back('U');
    last_ref = m.new_order_reference_number;
  }
  void on(const SystemEvent&) { seen.push_back('S'); }
};

TEST(Dispatch, CallsOnlyImplementedHandlers) {
  std::vector<std::byte> feed;
  encode::append(feed, encode::system_event(1, 'O'));
  encode::append(feed, encode::add_order(1, 2, 77, 'S', 100, "MSFT", 1500000));
  encode::append(feed, encode::del(1, 3, 77));  // no handler: silently skipped
  encode::append(feed, encode::replace(1, 4, 77, 78, 200, 1500100));
  Recorder r;
  const auto st = replay(feed, r);
  EXPECT_EQ(st.messages, 4u);
  EXPECT_EQ(st.unknown_type, 0u);
  EXPECT_EQ(st.bad_length, 0u);
  EXPECT_EQ(r.seen, (std::vector<char>{'S', 'A', 'U'}));
  EXPECT_EQ(r.last_ref, 78u);
}

TEST(Dispatch, RejectsBadLengthAndUnknownType) {
  Recorder r;
  auto short_add = bytes({'A', 0, 1});
  EXPECT_EQ(dispatch(short_add.data(), short_add.size(), r), Status::bad_length);
  auto unknown = bytes({'Z', 0, 0, 0, 0});
  EXPECT_EQ(dispatch(unknown.data(), unknown.size(), r), Status::unknown_type);
  EXPECT_TRUE(r.seen.empty());
}

TEST(Framing, IgnoresTruncatedTail) {
  std::vector<std::byte> feed;
  encode::append(feed, encode::system_event(1, 'O'));
  encode::append(feed, encode::system_event(2, 'S'));
  feed.resize(feed.size() - 3);  // cut the last message short
  Recorder r;
  const auto st = replay(feed, r);
  EXPECT_EQ(st.messages, 1u);
  EXPECT_EQ(st.truncated_tail_bytes, 2u + sizeof(SystemEvent) - 3);
}

TEST(Framing, EveryEncoderRoundTripsThroughDispatch) {
  std::vector<std::byte> feed;
  encode::append(feed, encode::stock_directory(5, 1, "QQQ"));
  encode::append(feed, encode::add_order_mpid(5, 1, 1, 'B', 100, "QQQ", 2000000, "GSCO"));
  encode::append(feed, encode::executed(5, 1, 1, 50, 9));
  encode::append(feed, encode::executed_with_price(5, 1, 1, 10, 10, 2000100));
  encode::append(feed, encode::cancel(5, 1, 1, 10));
  encode::append(feed, encode::trade(5, 1, 'B', 100, "QQQ", 2000000, 11));
  encode::append(feed, encode::cross(5, 1, 5000, "QQQ", 2000000, 12, 'C'));
  struct Null {} n;
  const auto st = replay(feed, n);
  EXPECT_EQ(st.messages, 7u);
  EXPECT_EQ(st.bad_length + st.unknown_type, 0u);
}

}  // namespace
