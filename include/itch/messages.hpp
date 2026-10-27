// SPDX-License-Identifier: MIT
//
// All 23 message types of NASDAQ TotalView-ITCH 5.0, as zero-copy overlays.
//
// Field names and order follow the specification ("Nasdaq TotalView-ITCH 5.0",
// section 1 "Message Formats"). Each struct's sizeof() is checked against the
// message length table in the spec at the bottom of this file, so a layout
// mistake is a compile error, not a corrupted book at 09:30.
#pragma once

#include <cstdint>
#include <cstring>
#include <concepts>
#include <tuple>
#include <type_traits>

#include "itch/wire.hpp"

namespace itch {

/// Common to every message: type, stock locate, tracking number, timestamp.
struct Header {
  Char message_type;
  be16 stock_locate;     ///< per-day integer id of the instrument (0 = none)
  be16 tracking_number;  ///< NASDAQ internal
  std::array<std::byte, 6> timestamp_raw;

  /// Nanoseconds since midnight. The field is 6 bytes; we load the 8 bytes
  /// starting at tracking_number (still inside the header) with one MOV +
  /// BSWAP and mask off the top 16 bits, instead of six byte loads.
  [[nodiscard]] std::uint64_t timestamp() const noexcept {
    std::uint64_t v;
    std::memcpy(&v, reinterpret_cast<const std::byte*>(&tracking_number), 8);
    if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
    return v & 0xFFFF'FFFF'FFFFull;
  }
  [[nodiscard]] std::uint16_t locate() const noexcept { return stock_locate.value(); }
};
static_assert(sizeof(Header) == 11);

// ---- system and reference data ----------------------------------------------

struct SystemEvent {
  static constexpr char kType = 'S';
  Header header;
  Char event_code;  ///< O start of messages, S start of system hours, Q start of market
                    ///< hours, M end of market hours, E end of system hours, C end of messages
};

struct StockDirectory {
  static constexpr char kType = 'R';
  Header header;
  Stock stock;
  Char market_category;
  Char financial_status_indicator;
  be32 round_lot_size;
  Char round_lots_only;
  Char issue_classification;
  Alpha<2> issue_sub_type;
  Char authenticity;
  Char short_sale_threshold_indicator;
  Char ipo_flag;
  Char luld_reference_price_tier;
  Char etp_flag;
  be32 etp_leverage_factor;
  Char inverse_indicator;
};

struct StockTradingAction {
  static constexpr char kType = 'H';
  Header header;
  Stock stock;
  Char trading_state;  ///< H halted, P paused, Q quotation only, T trading
  Char reserved;
  Alpha<4> reason;
};

struct RegShoRestriction {
  static constexpr char kType = 'Y';
  Header header;
  Stock stock;
  Char reg_sho_action;
};

struct MarketParticipantPosition {
  static constexpr char kType = 'L';
  Header header;
  Alpha<4> mpid;
  Stock stock;
  Char primary_market_maker;
  Char market_maker_mode;
  Char market_participant_state;
};

struct MwcbDeclineLevel {
  static constexpr char kType = 'V';
  Header header;
  Price8 level1;
  Price8 level2;
  Price8 level3;
};

struct MwcbStatus {
  static constexpr char kType = 'W';
  Header header;
  Char breached_level;
};

struct IpoQuotingPeriodUpdate {
  static constexpr char kType = 'K';
  Header header;
  Stock stock;
  be32 ipo_quotation_release_time;
  Char ipo_quotation_release_qualifier;
  Price4 ipo_price;
};

struct LuldAuctionCollar {
  static constexpr char kType = 'J';
  Header header;
  Stock stock;
  Price4 auction_collar_reference_price;
  Price4 upper_auction_collar_price;
  Price4 lower_auction_collar_price;
  be32 auction_collar_extension;
};

struct OperationalHalt {
  static constexpr char kType = 'h';
  Header header;
  Stock stock;
  Char market_code;
  Char operational_halt_action;
};

// ---- order book (market by order) ---------------------------------------------

struct AddOrder {
  static constexpr char kType = 'A';
  Header header;
  be64 order_reference_number;
  Char buy_sell_indicator;  ///< 'B' or 'S'
  be32 shares;
  Stock stock;
  Price4 price;
};

struct AddOrderMpid {
  static constexpr char kType = 'F';
  Header header;
  be64 order_reference_number;
  Char buy_sell_indicator;
  be32 shares;
  Stock stock;
  Price4 price;
  Alpha<4> attribution;
};

struct OrderExecuted {
  static constexpr char kType = 'E';
  Header header;
  be64 order_reference_number;
  be32 executed_shares;
  be64 match_number;
};

struct OrderExecutedWithPrice {
  static constexpr char kType = 'C';
  Header header;
  be64 order_reference_number;
  be32 executed_shares;
  be64 match_number;
  Char printable;
  Price4 execution_price;
};

struct OrderCancel {
  static constexpr char kType = 'X';
  Header header;
  be64 order_reference_number;
  be32 cancelled_shares;
};

struct OrderDelete {
  static constexpr char kType = 'D';
  Header header;
  be64 order_reference_number;
};

struct OrderReplace {
  static constexpr char kType = 'U';
  Header header;
  be64 original_order_reference_number;
  be64 new_order_reference_number;
  be32 shares;
  Price4 price;
};

// ---- trades and auctions --------------------------------------------------------

/// Execution against a non-displayed order: does not touch the visible book.
struct Trade {
  static constexpr char kType = 'P';
  Header header;
  be64 order_reference_number;
  Char buy_sell_indicator;
  be32 shares;
  Stock stock;
  Price4 price;
  be64 match_number;
};

/// Opening / closing / IPO / halt cross. Cross type 'O' open, 'C' close.
struct CrossTrade {
  static constexpr char kType = 'Q';
  Header header;
  be64 shares;
  Stock stock;
  Price4 cross_price;
  be64 match_number;
  Char cross_type;
};

struct BrokenTrade {
  static constexpr char kType = 'B';
  Header header;
  be64 match_number;
};

/// Net Order Imbalance Indicator, published ahead of the crosses.
struct Noii {
  static constexpr char kType = 'I';
  Header header;
  be64 paired_shares;
  be64 imbalance_shares;
  Char imbalance_direction;
  Stock stock;
  Price4 far_price;
  Price4 near_price;
  Price4 current_reference_price;
  Char cross_type;
  Char price_variation_indicator;
};

struct RetailPriceImprovement {
  static constexpr char kType = 'N';
  Header header;
  Stock stock;
  Char interest_flag;
};

/// Added to ITCH 5.0 in 2020; absent from older files, handled for completeness.
struct DirectListingPriceDiscovery {
  static constexpr char kType = 'O';
  Header header;
  Stock stock;
  Char open_eligibility_status;
  Price4 minimum_allowable_price;
  Price4 maximum_allowable_price;
  Price4 near_execution_price;
  be64 near_execution_time;
  Price4 lower_price_range_collar;
  Price4 upper_price_range_collar;
};

/// Every message type, in spec order. Drives the length table, the tests and
/// the per-type statistics.
using AllMessages =
    std::tuple<SystemEvent, StockDirectory, StockTradingAction, RegShoRestriction,
               MarketParticipantPosition, MwcbDeclineLevel, MwcbStatus, IpoQuotingPeriodUpdate,
               LuldAuctionCollar, OperationalHalt, AddOrder, AddOrderMpid, OrderExecuted,
               OrderExecutedWithPrice, OrderCancel, OrderDelete, OrderReplace, Trade, CrossTrade,
               BrokenTrade, Noii, RetailPriceImprovement, DirectListingPriceDiscovery>;

// ---- layout checks against the specification's length table ----------------------

static_assert(sizeof(SystemEvent) == 12);
static_assert(sizeof(StockDirectory) == 39);
static_assert(sizeof(StockTradingAction) == 25);
static_assert(sizeof(RegShoRestriction) == 20);
static_assert(sizeof(MarketParticipantPosition) == 26);
static_assert(sizeof(MwcbDeclineLevel) == 35);
static_assert(sizeof(MwcbStatus) == 12);
static_assert(sizeof(IpoQuotingPeriodUpdate) == 28);
static_assert(sizeof(LuldAuctionCollar) == 35);
static_assert(sizeof(OperationalHalt) == 21);
static_assert(sizeof(AddOrder) == 36);
static_assert(sizeof(AddOrderMpid) == 40);
static_assert(sizeof(OrderExecuted) == 31);
static_assert(sizeof(OrderExecutedWithPrice) == 36);
static_assert(sizeof(OrderCancel) == 23);
static_assert(sizeof(OrderDelete) == 19);
static_assert(sizeof(OrderReplace) == 35);
static_assert(sizeof(Trade) == 44);
static_assert(sizeof(CrossTrade) == 40);
static_assert(sizeof(BrokenTrade) == 19);
static_assert(sizeof(Noii) == 50);
static_assert(sizeof(RetailPriceImprovement) == 20);
static_assert(sizeof(DirectListingPriceDiscovery) == 48);

/// Overlays must be viewable in place: byte-aligned, trivially copyable.
template <class M>
concept WireMessage = std::is_trivially_copyable_v<M> && alignof(M) == 1 && requires {
  { M::kType } -> std::convertible_to<char>;
};
static_assert([]<class... Ms>(std::tuple<Ms...>*) { return (WireMessage<Ms> && ...); }(
    static_cast<AllMessages*>(nullptr)));

/// Expected length of each message type, indexed by type byte (0 = unknown).
inline constexpr auto kMessageLength = [] {
  std::array<std::uint8_t, 256> t{};
  [&]<class... Ms>(std::tuple<Ms...>*) {
    ((t[static_cast<unsigned char>(Ms::kType)] = sizeof(Ms)), ...);
  }(static_cast<AllMessages*>(nullptr));
  return t;
}();

}  // namespace itch
