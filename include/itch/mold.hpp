// SPDX-License-Identifier: MIT
//
// MoldUDP64: the transport NASDAQ uses for ITCH over UDP multicast.
//
//   packet  = header (20 bytes) + message_count × [u16 length][message]
//   header  = session (10 ASCII) + sequence_number (u64, of the first message)
//             + message_count (u16)
//
// UDP can drop, duplicate and reorder. The receiver tracks the next expected
// sequence number and, per message, either delivers it (in order), drops it
// (already seen), or records a gap (missed messages that must be requested
// from the re-request server before the book can be trusted again).
// message_count == 0 is a heartbeat carrying the next sequence number;
// message_count == 0xFFFF marks end of session.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "itch/wire.hpp"

namespace itch::mold {

struct Header {
  Alpha<10> session;
  be64 sequence_number;
  be16 message_count;
};
static_assert(sizeof(Header) == 20);

inline constexpr std::uint16_t kEndOfSession = 0xFFFF;

/// A range of sequence numbers [first, first + count) that was never received.
struct Gap {
  std::uint64_t first;
  std::uint64_t count;
};

struct Stats {
  std::uint64_t packets = 0;
  std::uint64_t messages_delivered = 0;
  std::uint64_t heartbeats = 0;
  std::uint64_t duplicate_messages = 0;
  std::uint64_t gaps = 0;
  std::uint64_t gap_messages = 0;
  std::uint64_t malformed_packets = 0;
  bool end_of_session = false;
};

class Receiver {
 public:
  explicit Receiver(std::uint64_t first_expected = 1) : expected_(first_expected) {}

  /// Processes one UDP payload; calls deliver(message_bytes, sequence_number)
  /// for every new, in-order message.
  template <class Deliver>
  void on_packet(std::span<const std::byte> packet, Deliver&& deliver) {
    ++stats_.packets;
    if (packet.size() < sizeof(Header)) [[unlikely]] {
      ++stats_.malformed_packets;
      return;
    }
    const auto& h = *std::start_lifetime_as<Header>(packet.data());
    const std::uint64_t first = h.sequence_number.value();
    const std::uint16_t count = h.message_count.value();

    if (count == kEndOfSession) {
      stats_.end_of_session = true;
      note_gap_until(first);
      return;
    }
    if (count == 0) {  // heartbeat: `first` is the next sequence number to come
      ++stats_.heartbeats;
      note_gap_until(first);
      return;
    }

    std::size_t pos = sizeof(Header);
    for (std::uint64_t seq = first; seq < first + count; ++seq) {
      if (pos + 2 > packet.size()) [[unlikely]] {
        ++stats_.malformed_packets;
        return;
      }
      const std::size_t len = (std::to_integer<std::size_t>(packet[pos]) << 8) |
                              std::to_integer<std::size_t>(packet[pos + 1]);
      pos += 2;
      if (pos + len > packet.size()) [[unlikely]] {
        ++stats_.malformed_packets;
        return;
      }
      const auto msg = packet.subspan(pos, len);
      pos += len;

      if (seq < expected_) {
        ++stats_.duplicate_messages;  // retransmission or a duplicated packet
        continue;
      }
      note_gap_until(seq);
      deliver(msg, seq);
      ++stats_.messages_delivered;
      expected_ = seq + 1;
    }
  }

  [[nodiscard]] std::uint64_t expected() const noexcept { return expected_; }
  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
  /// Gaps to request from the re-request server, oldest first.
  [[nodiscard]] const std::vector<Gap>& gaps() const noexcept { return gaps_; }

 private:
  void note_gap_until(std::uint64_t seq) {
    if (seq <= expected_) return;
    gaps_.push_back({expected_, seq - expected_});
    ++stats_.gaps;
    stats_.gap_messages += seq - expected_;
    // A production handler would buffer and wait for the retransmission; here
    // we skip ahead and leave the gap recorded for the caller to act on.
    expected_ = seq;
  }

  std::uint64_t expected_;
  Stats stats_;
  std::vector<Gap> gaps_;
};

/// Splits a sequence of messages into MoldUDP64 packets of at most
/// `max_payload` bytes (1400 keeps a packet inside a 1500-byte Ethernet MTU).
class Packetizer {
 public:
  explicit Packetizer(std::string_view session, std::uint64_t first_sequence = 1,
                      std::size_t max_payload = 1400)
      : next_seq_(first_sequence), max_payload_(max_payload) {
    session_.chars.fill(' ');
    for (std::size_t i = 0; i < session.size() && i < 10; ++i) session_.chars[i] = session[i];
  }

  /// Appends a message; calls emit(packet_bytes) whenever a packet fills up.
  template <class Emit>
  void add(std::span<const std::byte> msg, Emit&& emit) {
    if (!packet_.empty() && packet_.size() + 2 + msg.size() > max_payload_) flush(emit);
    if (packet_.empty()) start_packet();
    packet_.push_back(static_cast<std::byte>(msg.size() >> 8));
    packet_.push_back(static_cast<std::byte>(msg.size() & 0xFF));
    packet_.insert(packet_.end(), msg.begin(), msg.end());
    ++count_;
  }

  template <class Emit>
  void flush(Emit&& emit) {
    if (count_ == 0) return;
    auto& h = *reinterpret_cast<Header*>(packet_.data());
    h.message_count = be16::from(static_cast<std::uint16_t>(count_));
    emit(std::span<const std::byte>(packet_));
    next_seq_ += count_;
    packet_.clear();
    count_ = 0;
  }

 private:
  void start_packet() {
    packet_.resize(sizeof(Header));
    auto& h = *reinterpret_cast<Header*>(packet_.data());
    h.session = session_;
    h.sequence_number = be64::from(next_seq_);
  }

  Alpha<10> session_{};
  std::uint64_t next_seq_;
  std::size_t max_payload_;
  std::vector<std::byte> packet_;
  std::size_t count_ = 0;
};

}  // namespace itch::mold
