// SPDX-License-Identifier: MIT
//
// Builds ITCH 5.0 messages. Used by the unit tests and the synthetic feed
// generator; the production path only ever decodes.
#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "itch/messages.hpp"

namespace itch::encode {

inline Header header(char type, std::uint16_t locate, std::uint64_t timestamp_ns,
                     std::uint16_t tracking = 0) {
  Header h{};
  h.message_type = Char::from(type);
  h.stock_locate = be16::from(locate);
  h.tracking_number = be16::from(tracking);
  const auto ts = BigEndian<std::uint64_t, 6>::from(timestamp_ns);
  std::memcpy(h.timestamp_raw.data(), ts.bytes.data(), 6);
  return h;
}

/// Raw bytes of any wire struct (ITCH message or MoldUDP64 header).
template <class M>
  requires std::is_trivially_copyable_v<M> && (alignof(M) == 1)
std::span<const std::byte> bytes_of(const M& m) {
  return {reinterpret_cast<const std::byte*>(&m), sizeof(M)};
}

/// Appends `m` in BinaryFILE framing: [u16 big-endian length][message].
template <WireMessage M>
void append(std::vector<std::byte>& out, const M& m) {
  out.push_back(static_cast<std::byte>(sizeof(M) >> 8));
  out.push_back(static_cast<std::byte>(sizeof(M) & 0xFF));
  const auto b = bytes_of(m);
  out.insert(out.end(), b.begin(), b.end());
}

inline SystemEvent system_event(std::uint64_t ts, char code) {
  SystemEvent m{};
  m.header = header(SystemEvent::kType, 0, ts);
  m.event_code = Char::from(code);
  return m;
}

inline StockDirectory stock_directory(std::uint16_t locate, std::uint64_t ts,
                                      std::string_view stock) {
  StockDirectory m{};
  m.header = header(StockDirectory::kType, locate, ts);
  m.stock = Stock::from(stock);
  m.market_category = Char::from('Q');
  m.financial_status_indicator = Char::from('N');
  m.round_lot_size = be32::from(100);
  m.round_lots_only = Char::from('N');
  m.issue_classification = Char::from('C');
  m.issue_sub_type = Alpha<2>::from("Z");
  m.authenticity = Char::from('P');
  m.short_sale_threshold_indicator = Char::from('N');
  m.ipo_flag = Char::from(' ');
  m.luld_reference_price_tier = Char::from('1');
  m.etp_flag = Char::from('N');
  m.inverse_indicator = Char::from('N');
  return m;
}

inline AddOrder add_order(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, char side,
                          std::uint32_t shares, std::string_view stock, std::uint32_t price) {
  AddOrder m{};
  m.header = header(AddOrder::kType, locate, ts);
  m.order_reference_number = be64::from(ref);
  m.buy_sell_indicator = Char::from(side);
  m.shares = be32::from(shares);
  m.stock = Stock::from(stock);
  m.price = be32::from(price);
  return m;
}

inline AddOrderMpid add_order_mpid(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                                   char side, std::uint32_t shares, std::string_view stock,
                                   std::uint32_t price, std::string_view mpid) {
  AddOrderMpid m{};
  m.header = header(AddOrderMpid::kType, locate, ts);
  m.order_reference_number = be64::from(ref);
  m.buy_sell_indicator = Char::from(side);
  m.shares = be32::from(shares);
  m.stock = Stock::from(stock);
  m.price = be32::from(price);
  m.attribution = Alpha<4>::from(mpid);
  return m;
}

inline OrderExecuted executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                              std::uint32_t shares, std::uint64_t match) {
  OrderExecuted m{};
  m.header = header(OrderExecuted::kType, locate, ts);
  m.order_reference_number = be64::from(ref);
  m.executed_shares = be32::from(shares);
  m.match_number = be64::from(match);
  return m;
}

inline OrderExecutedWithPrice executed_with_price(std::uint16_t locate, std::uint64_t ts,
                                                  std::uint64_t ref, std::uint32_t shares,
                                                  std::uint64_t match, std::uint32_t price) {
  OrderExecutedWithPrice m{};
  m.header = header(OrderExecutedWithPrice::kType, locate, ts);
  m.order_reference_number = be64::from(ref);
  m.executed_shares = be32::from(shares);
  m.match_number = be64::from(match);
  m.printable = Char::from('Y');
  m.execution_price = be32::from(price);
  return m;
}

inline OrderCancel cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                          std::uint32_t shares) {
  OrderCancel m{};
  m.header = header(OrderCancel::kType, locate, ts);
  m.order_reference_number = be64::from(ref);
  m.cancelled_shares = be32::from(shares);
  return m;
}

inline OrderDelete del(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref) {
  OrderDelete m{};
  m.header = header(OrderDelete::kType, locate, ts);
  m.order_reference_number = be64::from(ref);
  return m;
}

inline OrderReplace replace(std::uint16_t locate, std::uint64_t ts, std::uint64_t old_ref,
                            std::uint64_t new_ref, std::uint32_t shares, std::uint32_t price) {
  OrderReplace m{};
  m.header = header(OrderReplace::kType, locate, ts);
  m.original_order_reference_number = be64::from(old_ref);
  m.new_order_reference_number = be64::from(new_ref);
  m.shares = be32::from(shares);
  m.price = be32::from(price);
  return m;
}

inline Trade trade(std::uint16_t locate, std::uint64_t ts, char side, std::uint32_t shares,
                   std::string_view stock, std::uint32_t price, std::uint64_t match) {
  Trade m{};
  m.header = header(Trade::kType, locate, ts);
  m.order_reference_number = be64::from(0);
  m.buy_sell_indicator = Char::from(side);
  m.shares = be32::from(shares);
  m.stock = Stock::from(stock);
  m.price = be32::from(price);
  m.match_number = be64::from(match);
  return m;
}

inline CrossTrade cross(std::uint16_t locate, std::uint64_t ts, std::uint64_t shares,
                        std::string_view stock, std::uint32_t price, std::uint64_t match,
                        char cross_type) {
  CrossTrade m{};
  m.header = header(CrossTrade::kType, locate, ts);
  m.shares = be64::from(shares);
  m.stock = Stock::from(stock);
  m.cross_price = be32::from(price);
  m.match_number = be64::from(match);
  m.cross_type = Char::from(cross_type);
  return m;
}

}  // namespace itch::encode
