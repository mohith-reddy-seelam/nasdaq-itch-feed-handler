// SPDX-License-Identifier: MIT
//
// Deterministic synthetic market-by-order feed in ITCH 5.0 BinaryFILE format.
//
// Not a market simulator, a protocol-valid stress source with the statistical
// shape that matters to a book builder:
//   * many symbols, activity Zipf-distributed across them;
//   * adds placed a geometric number of ticks from the mid (most near the touch);
//   * most orders die by cancel/delete, as in real markets; some execute
//     against the touch, some are replaced;
//   * a drifting mid, with orders that would cross removed as it moves;
//   * every message type a book cares about (A F E C X D U P Q), plus S and R.
// The same seed always produces the same bytes, so tests and benchmarks are
// reproducible without a multi-GB download.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "itch/encode.hpp"

namespace itch::synthetic {

struct Config {
  std::uint64_t messages = 1'000'000;
  std::uint16_t symbols = 200;
  std::uint64_t seed = 42;
};

class Generator {
 public:
  explicit Generator(Config cfg) : cfg_(cfg), rng_(cfg.seed) {
    for (std::uint16_t i = 0; i < cfg_.symbols; ++i) {
      Sym s;
      s.locate = static_cast<std::uint16_t>(i + 1);
      s.name = "SYN" + std::to_string(i);
      s.mid = 100 * (1000 + static_cast<std::uint32_t>(rng_() % 50'000));  // $10 - $510
      syms_.push_back(std::move(s));
    }
    // Zipf(1) weights: symbol k gets activity proportional to 1/(k+1).
    std::vector<double> w(cfg_.symbols);
    for (std::size_t k = 0; k < w.size(); ++k) w[k] = 1.0 / static_cast<double>(k + 1);
    pick_symbol_ = std::discrete_distribution<int>(w.begin(), w.end());
  }

  /// Generates the whole feed into `out` (BinaryFILE framing).
  void generate(std::vector<std::byte>& out) {
    out.reserve(out.size() + cfg_.messages * 34);
    emit(out, encode::system_event(ts_, 'O'));
    emit(out, encode::system_event(ts_, 'S'));
    for (const auto& s : syms_) emit(out, encode::stock_directory(s.locate, ts_, s.name));
    emit(out, encode::system_event(ts_, 'Q'));
    while (count_ + syms_.size() + 3 < cfg_.messages) step(out);
    for (const auto& s : syms_)
      emit(out, encode::cross(s.locate, ts_, 1000, s.name, s.mid, ++match_, 'C'));
    emit(out, encode::system_event(ts_, 'M'));
    emit(out, encode::system_event(ts_, 'E'));
  }

 private:
  struct Live {
    std::uint64_t ref;
    std::uint32_t price;
    std::uint32_t shares;
    char side;
  };
  struct Sym {
    std::uint16_t locate;
    std::string name;
    std::uint32_t mid;
    std::vector<Live> live;
  };
  static constexpr std::uint32_t kTick = 100;  // $0.01 in Price(4)

  template <class M>
  void emit(std::vector<std::byte>& out, const M& m) {
    encode::append(out, m);
    ++count_;
  }

  double uniform() { return std::uniform_real_distribution<double>(0, 1)(rng_); }

  void step(std::vector<std::byte>& out) {
    ts_ += 1 + static_cast<std::uint64_t>(std::exponential_distribution<double>(1.0 / 2000)(rng_));
    Sym& s = syms_[static_cast<std::size_t>(pick_symbol_(rng_))];
    const double r = uniform();
    // Cap resting orders per symbol (~400) so the book stays realistically deep.
    if (s.live.size() < 8 || (r < 0.42 && s.live.size() < 400)) add(out, s);
    else if (r < 0.72) remove(out, s);
    else if (r < 0.78) partial_cancel(out, s);
    else if (r < 0.86) replace(out, s);
    else if (r < 0.97) execute(out, s);
    else if (r < 0.99) hidden_trade(out, s);
    else move_mid(out, s);
  }

  std::uint32_t round_lot() { return 100 * (1 + static_cast<std::uint32_t>(rng_() % 10)); }

  void add(std::vector<std::byte>& out, Sym& s) {
    const char side = (rng_() & 1) ? 'B' : 'S';
    const auto ticks = 1 + std::geometric_distribution<std::uint32_t>(0.35)(rng_);
    const std::uint32_t price = side == 'B' ? s.mid - ticks * kTick : s.mid + ticks * kTick;
    const Live o{next_ref_++, price, round_lot(), side};
    if (uniform() < 0.1)
      emit(out, encode::add_order_mpid(s.locate, ts_, o.ref, side, o.shares, s.name, price, "SYNM"));
    else
      emit(out, encode::add_order(s.locate, ts_, o.ref, side, o.shares, s.name, price));
    s.live.push_back(o);
  }

  std::size_t random_order(const Sym& s) { return rng_() % s.live.size(); }

  void erase_at(Sym& s, std::size_t i) {
    s.live[i] = s.live.back();
    s.live.pop_back();
  }

  void remove(std::vector<std::byte>& out, Sym& s) {
    const std::size_t i = random_order(s);
    emit(out, encode::del(s.locate, ts_, s.live[i].ref));
    erase_at(s, i);
  }

  void partial_cancel(std::vector<std::byte>& out, Sym& s) {
    const std::size_t i = random_order(s);
    if (s.live[i].shares <= 100) return remove(out, s);
    emit(out, encode::cancel(s.locate, ts_, s.live[i].ref, 100));
    s.live[i].shares -= 100;
  }

  void replace(std::vector<std::byte>& out, Sym& s) {
    const std::size_t i = random_order(s);
    Live& o = s.live[i];
    std::uint32_t price = o.price + ((rng_() & 1) ? kTick : 0) - ((rng_() & 1) ? kTick : 0);
    // Never move an order through the mid.
    if (o.side == 'B') price = std::min(price, s.mid - kTick);
    else price = std::max(price, s.mid + kTick);
    const Live n{next_ref_++, price, round_lot(), o.side};
    emit(out, encode::replace(s.locate, ts_, o.ref, n.ref, n.shares, n.price));
    o = n;
  }

  /// Executes against the best order on a random side (an incoming marketable order).
  void execute(std::vector<std::byte>& out, Sym& s) {
    const char side = (rng_() & 1) ? 'B' : 'S';
    std::size_t best = s.live.size();
    for (std::size_t i = 0; i < s.live.size(); ++i) {
      if (s.live[i].side != side) continue;
      if (best == s.live.size() ||
          (side == 'B' ? s.live[i].price > s.live[best].price : s.live[i].price < s.live[best].price))
        best = i;
    }
    if (best == s.live.size()) return add(out, s);
    Live& o = s.live[best];
    const std::uint32_t qty = std::min<std::uint32_t>(o.shares, round_lot());
    if (uniform() < 0.05)
      emit(out, encode::executed_with_price(s.locate, ts_, o.ref, qty, ++match_, o.price));
    else
      emit(out, encode::executed(s.locate, ts_, o.ref, qty, ++match_));
    o.shares -= qty;
    if (o.shares == 0) erase_at(s, best);
  }

  void hidden_trade(std::vector<std::byte>& out, Sym& s) {
    emit(out, encode::trade(s.locate, ts_, (rng_() & 1) ? 'B' : 'S', round_lot(), s.name, s.mid,
                            ++match_));
  }

  /// Moves the mid one tick; deletes resting orders that would now cross it.
  void move_mid(std::vector<std::byte>& out, Sym& s) {
    s.mid = (rng_() & 1) ? s.mid + kTick : std::max<std::uint32_t>(s.mid - kTick, 10 * kTick);
    for (std::size_t i = 0; i < s.live.size();) {
      const Live& o = s.live[i];
      const bool crosses = o.side == 'B' ? o.price >= s.mid : o.price <= s.mid;
      if (crosses) {
        emit(out, encode::del(s.locate, ts_, o.ref));
        erase_at(s, i);
      } else {
        ++i;
      }
    }
  }

  Config cfg_;
  std::mt19937_64 rng_;
  std::vector<Sym> syms_;
  std::discrete_distribution<int> pick_symbol_;
  std::uint64_t ts_ = 34'200'000'000'000ull;  // 09:30:00 in ns since midnight
  std::uint64_t next_ref_ = 1;
  std::uint64_t match_ = 0;
  std::uint64_t count_ = 0;
};

/// Convenience: a complete synthetic feed as one buffer.
inline std::vector<std::byte> make_feed(Config cfg) {
  std::vector<std::byte> out;
  Generator(cfg).generate(out);
  return out;
}

}  // namespace itch::synthetic
