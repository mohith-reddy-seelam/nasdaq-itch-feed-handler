// SPDX-License-Identifier: MIT
//
// Message dispatch and NASDAQ "BinaryFILE" framing.
//
// dispatch() validates the length for the type byte, views the bytes in place
// as the message struct (std::start_lifetime_as: no copy, no aliasing UB), and
// calls handler.on(msg). A handler implements on() only for the messages it
// cares about; for the rest the call is compiled out entirely, so a book
// builder pays nothing for NOII or MWCB messages beyond the switch.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <array>
#include <span>

#include "itch/messages.hpp"

namespace itch {

template <class H, class M>
concept Handles = requires(H& h, const M& m) { h.on(m); };

enum class Status : std::uint8_t { ok, unknown_type, bad_length };

template <WireMessage M, class H>
[[gnu::always_inline]] inline Status deliver(const std::byte* p, std::size_t len, H& h) {
  if (len != sizeof(M)) [[unlikely]] return Status::bad_length;
  if constexpr (Handles<H, M>) h.on(*std::start_lifetime_as<M>(p));
  return Status::ok;
}

/// Decodes one message (without its length prefix) and hands it to `h`.
template <class H>
[[gnu::always_inline]] inline Status dispatch(const std::byte* p, std::size_t len, H& h) {
  if (len == 0) [[unlikely]] return Status::bad_length;
  // Ordered roughly by frequency in a real NASDAQ day (A, D, U, E, X first),
  // although the compiler turns this into a jump table either way.
  switch (static_cast<char>(p[0])) {
    case AddOrder::kType: return deliver<AddOrder>(p, len, h);
    case OrderDelete::kType: return deliver<OrderDelete>(p, len, h);
    case OrderReplace::kType: return deliver<OrderReplace>(p, len, h);
    case OrderExecuted::kType: return deliver<OrderExecuted>(p, len, h);
    case OrderCancel::kType: return deliver<OrderCancel>(p, len, h);
    case AddOrderMpid::kType: return deliver<AddOrderMpid>(p, len, h);
    case OrderExecutedWithPrice::kType: return deliver<OrderExecutedWithPrice>(p, len, h);
    case Trade::kType: return deliver<Trade>(p, len, h);
    case Noii::kType: return deliver<Noii>(p, len, h);
    case CrossTrade::kType: return deliver<CrossTrade>(p, len, h);
    case BrokenTrade::kType: return deliver<BrokenTrade>(p, len, h);
    case SystemEvent::kType: return deliver<SystemEvent>(p, len, h);
    case StockDirectory::kType: return deliver<StockDirectory>(p, len, h);
    case StockTradingAction::kType: return deliver<StockTradingAction>(p, len, h);
    case RegShoRestriction::kType: return deliver<RegShoRestriction>(p, len, h);
    case MarketParticipantPosition::kType: return deliver<MarketParticipantPosition>(p, len, h);
    case MwcbDeclineLevel::kType: return deliver<MwcbDeclineLevel>(p, len, h);
    case MwcbStatus::kType: return deliver<MwcbStatus>(p, len, h);
    case IpoQuotingPeriodUpdate::kType: return deliver<IpoQuotingPeriodUpdate>(p, len, h);
    case LuldAuctionCollar::kType: return deliver<LuldAuctionCollar>(p, len, h);
    case OperationalHalt::kType: return deliver<OperationalHalt>(p, len, h);
    case RetailPriceImprovement::kType: return deliver<RetailPriceImprovement>(p, len, h);
    case DirectListingPriceDiscovery::kType:
      return deliver<DirectListingPriceDiscovery>(p, len, h);
    default: return Status::unknown_type;
  }
}

/// Counts of what a replay saw. Unknown/bad messages are counted, never fatal:
/// a feed handler must not die on one malformed message.
struct ReplayStats {
  std::uint64_t messages = 0;
  std::uint64_t bytes = 0;
  std::uint64_t unknown_type = 0;
  std::uint64_t bad_length = 0;
  std::uint64_t truncated_tail_bytes = 0;  ///< partial message at end of buffer
  std::array<std::uint64_t, 256> by_type{};
};

/// NASDAQ BinaryFILE: each message is preceded by its length as a 2-byte
/// big-endian integer. This is the format of the historical files NASDAQ
/// publishes, and of most captures.
class FrameReader {
 public:
  explicit FrameReader(std::span<const std::byte> data) noexcept : data_(data) {}

  /// Next message (without its length prefix), or an empty span at the end.
  [[nodiscard]] std::span<const std::byte> next() noexcept {
    if (pos_ + 2 > data_.size()) return {};
    const std::size_t len = (std::to_integer<std::size_t>(data_[pos_]) << 8) |
                            std::to_integer<std::size_t>(data_[pos_ + 1]);
    if (pos_ + 2 + len > data_.size()) return {};
    const auto msg = data_.subspan(pos_ + 2, len);
    pos_ += 2 + len;
    return msg;
  }

  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }

 private:
  std::span<const std::byte> data_;
  std::size_t pos_ = 0;
};

/// Replays a whole BinaryFILE buffer through `h`. `limit` caps the number of
/// messages (0 = all).
template <class H>
ReplayStats replay(std::span<const std::byte> file, H& h, std::uint64_t limit = 0) {
  ReplayStats st;
  FrameReader reader(file);
  for (auto msg = reader.next(); !msg.empty(); msg = reader.next()) {
    ++st.by_type[std::to_integer<unsigned char>(msg[0])];
    switch (dispatch(msg.data(), msg.size(), h)) {
      case Status::ok: break;
      case Status::unknown_type: ++st.unknown_type; break;
      case Status::bad_length: ++st.bad_length; break;
    }
    ++st.messages;
    st.bytes += msg.size() + 2;
    if (limit != 0 && st.messages == limit) break;
  }
  if (limit == 0) st.truncated_tail_bytes = reader.remaining();
  return st;
}

/// A handler that can be told about a message before it is delivered.
template <class H>
concept Prefetching = requires(const H& h, const std::byte* p, std::size_t n) { h.prefetch(p, n); };

/// replay() with a software-pipelined lookahead: message i + Lookahead is
/// framed and prefetched while message i is applied, so the cache misses of
/// future messages overlap with the work on the current one. Same results as
/// replay(), byte for byte.
template <std::size_t Lookahead = 8, Prefetching H>
ReplayStats replay_prefetched(std::span<const std::byte> file, H& h, std::uint64_t limit = 0) {
  ReplayStats st;
  FrameReader reader(file);
  std::array<std::span<const std::byte>, Lookahead> window;
  std::size_t queued = 0;
  for (; queued < Lookahead; ++queued) {
    window[queued] = reader.next();
    if (window[queued].empty()) break;
    h.prefetch(window[queued].data(), window[queued].size());
  }
  for (std::size_t i = 0; queued != 0; i = (i + 1) % Lookahead) {
    const auto msg = window[i];
    ++st.by_type[std::to_integer<unsigned char>(msg[0])];
    switch (dispatch(msg.data(), msg.size(), h)) {
      case Status::ok: break;
      case Status::unknown_type: ++st.unknown_type; break;
      case Status::bad_length: ++st.bad_length; break;
    }
    ++st.messages;
    st.bytes += msg.size() + 2;
    if (limit != 0 && st.messages == limit) break;
    window[i] = reader.next();
    if (window[i].empty()) --queued;
    else h.prefetch(window[i].data(), window[i].size());
  }
  if (limit == 0) st.truncated_tail_bytes = reader.remaining();
  return st;
}

}  // namespace itch
