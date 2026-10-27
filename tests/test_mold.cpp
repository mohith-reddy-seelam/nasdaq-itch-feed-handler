// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <vector>

#include "itch/encode.hpp"
#include "itch/mold.hpp"

namespace {

using namespace itch;

/// N add-order messages packed into MoldUDP64 packets of a few messages each.
std::vector<std::vector<std::byte>> packets(int n, std::size_t max_payload = 120) {
  std::vector<std::vector<std::byte>> out;
  mold::Packetizer pk("SESSION001", 1, max_payload);
  auto emit = [&](std::span<const std::byte> p) { out.emplace_back(p.begin(), p.end()); };
  for (int i = 1; i <= n; ++i) {
    const auto m = encode::add_order(1, static_cast<std::uint64_t>(i),
                                     static_cast<std::uint64_t>(i), 'B', 100, "X", 10000);
    pk.add(encode::bytes_of(m), emit);
  }
  pk.flush(emit);
  return out;
}

std::uint64_t ref_of(std::span<const std::byte> msg) {
  return std::start_lifetime_as<AddOrder>(msg.data())->order_reference_number;
}

TEST(Mold, InOrderDeliveryMatchesSequenceNumbers) {
  const auto pk = packets(10);
  ASSERT_GT(pk.size(), 2u);  // several messages per packet, several packets
  mold::Receiver rx;
  std::vector<std::uint64_t> refs;
  for (const auto& p : pk)
    rx.on_packet(p, [&](std::span<const std::byte> m, std::uint64_t seq) {
      EXPECT_EQ(ref_of(m), seq);  // message i was built with ref i
      refs.push_back(seq);
    });
  EXPECT_EQ(refs.size(), 10u);
  EXPECT_EQ(rx.expected(), 11u);
  EXPECT_TRUE(rx.gaps().empty());
}

TEST(Mold, DroppedPacketIsReportedAsGap) {
  auto pk = packets(10);
  const auto& dropped = pk[1];
  const auto& h = *std::start_lifetime_as<mold::Header>(dropped.data());
  const std::uint64_t first = h.sequence_number, count = h.message_count;

  mold::Receiver rx;
  std::uint64_t delivered = 0;
  for (std::size_t i = 0; i < pk.size(); ++i)
    if (i != 1) rx.on_packet(pk[i], [&](auto, auto) { ++delivered; });
  ASSERT_EQ(rx.gaps().size(), 1u);
  EXPECT_EQ(rx.gaps()[0].first, first);
  EXPECT_EQ(rx.gaps()[0].count, count);
  EXPECT_EQ(delivered, 10 - count);
  EXPECT_EQ(rx.stats().gap_messages, count);
}

TEST(Mold, DuplicatePacketIsNotRedelivered) {
  const auto pk = packets(6);
  mold::Receiver rx;
  std::uint64_t delivered = 0;
  for (const auto& p : pk) rx.on_packet(p, [&](auto, auto) { ++delivered; });
  rx.on_packet(pk[0], [&](auto, auto) { ++delivered; });  // late retransmission
  EXPECT_EQ(delivered, 6u);
  EXPECT_GT(rx.stats().duplicate_messages, 0u);
  EXPECT_TRUE(rx.gaps().empty());
}

TEST(Mold, HeartbeatAheadOfUsRevealsGap) {
  mold::Header hb{};
  hb.session = Alpha<10>::from("SESSION001");
  hb.sequence_number = be64::from(5);  // next message will be #5
  hb.message_count = be16::from(0);
  mold::Receiver rx;  // expecting #1
  rx.on_packet(encode::bytes_of(hb), [](auto, auto) { FAIL(); });
  EXPECT_EQ(rx.stats().heartbeats, 1u);
  ASSERT_EQ(rx.gaps().size(), 1u);
  EXPECT_EQ(rx.gaps()[0].first, 1u);
  EXPECT_EQ(rx.gaps()[0].count, 4u);
}

TEST(Mold, EndOfSessionAndMalformed) {
  mold::Header eos{};
  eos.sequence_number = be64::from(1);
  eos.message_count = be16::from(mold::kEndOfSession);
  mold::Receiver rx;
  rx.on_packet(encode::bytes_of(eos), [](auto, auto) { FAIL(); });
  EXPECT_TRUE(rx.stats().end_of_session);

  auto pk = packets(3);
  pk[0].resize(pk[0].size() - 5);  // cut inside the last message
  rx.on_packet(std::span<const std::byte>(pk[0].data(), 10), [](auto, auto) {});
  EXPECT_EQ(rx.stats().malformed_packets, 1u);
}

}  // namespace
