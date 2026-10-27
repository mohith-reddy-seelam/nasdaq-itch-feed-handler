// SPDX-License-Identifier: MIT
//
// Wire-level building blocks for NASDAQ TotalView-ITCH 5.0.
//
// ITCH is big-endian and unaligned. Every field type here is a byte array
// (alignof == 1), so a message struct built from them has *exactly* the wire
// layout: sizeof(AddOrder) == 36, no packing pragmas, no padding. A received
// buffer is then viewed in place with std::start_lifetime_as (C++23), and a
// field is decoded only when it is read (one load + one BSWAP). Nothing is
// copied or parsed up front: this is the zero-copy "overlay" style used by
// production feed handlers.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace itch {

/// Big-endian unsigned integer of N bytes, stored as raw bytes.
template <class T, std::size_t N = sizeof(T)>
struct BigEndian {
  static_assert(std::is_unsigned_v<T> && N <= sizeof(T));
  std::array<std::byte, N> bytes;

  [[nodiscard]] constexpr T value() const noexcept {
    if constexpr (N == sizeof(T)) {
      // memcpy into a register-sized value compiles to a single MOV + BSWAP.
      T v;
      std::memcpy(&v, bytes.data(), N);
      if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
      return v;
    } else {
      T v = 0;
      for (std::byte b : bytes) v = static_cast<T>((v << 8) | std::to_integer<T>(b));
      return v;
    }
  }
  constexpr operator T() const noexcept { return value(); }  // NOLINT: intentional

  /// Encodes `v` (used by the test encoder and the synthetic generator).
  [[nodiscard]] static constexpr BigEndian from(T v) noexcept {
    BigEndian out{};
    for (std::size_t i = 0; i < N; ++i)
      out.bytes[N - 1 - i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    return out;
  }
};

using u8 = std::uint8_t;
using be16 = BigEndian<std::uint16_t>;
using be32 = BigEndian<std::uint32_t>;
using be64 = BigEndian<std::uint64_t>;

/// Single ASCII character field (message type, side, flags).
struct Char {
  std::byte raw;
  [[nodiscard]] constexpr char value() const noexcept { return static_cast<char>(raw); }
  constexpr operator char() const noexcept { return value(); }  // NOLINT: intentional
  [[nodiscard]] static constexpr Char from(char c) noexcept { return Char{static_cast<std::byte>(c)}; }
};

/// Fixed-width, right-space-padded ASCII field ("AAPL    ").
template <std::size_t N>
struct Alpha {
  std::array<char, N> chars;
  [[nodiscard]] constexpr std::string_view view() const noexcept {
    std::string_view s(chars.data(), N);
    const auto end = s.find_last_not_of(' ');
    return end == std::string_view::npos ? std::string_view{} : s.substr(0, end + 1);
  }
  [[nodiscard]] static constexpr Alpha from(std::string_view s) noexcept {
    Alpha out{};
    out.chars.fill(' ');
    for (std::size_t i = 0; i < s.size() && i < N; ++i) out.chars[i] = s[i];
    return out;
  }
};

using Stock = Alpha<8>;

/// Price(4): unsigned integer with 4 implied decimal places ($123.4500 -> 1234500).
using Price4 = be32;
/// Price(8): 8 implied decimal places (used only by MWCB decline levels).
using Price8 = be64;

inline constexpr double kPrice4Scale = 1e-4;

static_assert(alignof(be64) == 1 && sizeof(be64) == 8);
static_assert(alignof(Stock) == 1 && sizeof(Stock) == 8);

}  // namespace itch
