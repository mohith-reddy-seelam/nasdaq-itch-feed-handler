// SPDX-License-Identifier: MIT
//
// Three pinned threads connected by wait-free SPSC rings:
//
//   [feed] --RawEvent--> [book] --TopEvent--> [strategy]
//
//   feed      frames messages out of the (memory-mapped) capture and stamps
//             each with its arrival time. With pacing, the stamp is the
//             *scheduled* arrival time (coordinated-omission safe: if a
//             downstream stage backs up, the delay is charged to every
//             message that should have arrived meanwhile).
//   book      decodes the message in place and applies it to the full-depth
//             book; when the instrument's best bid/offer changes, forwards a
//             TopEvent. Only ~1 in 3 book events changes the top.
//   strategy  computes per-instrument signals from the new top of book:
//             microprice, queue imbalance and Cont-Kukanov-Stoikov order-flow
//             imbalance (OFI).
//
// Measured with the TSC: arrival -> book applied, arrival -> signal computed.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

#include "book/book_builder.hpp"
#include "feed/histogram.hpp"
#include "itch/parser.hpp"
#include "platform.hpp"
#include "spsc/ring_buffer.hpp"

namespace feed {

/// One framed message, pointing into the capture buffer (zero copy).
struct RawEvent {
  const std::byte* msg;
  std::uint32_t len;
  std::uint64_t arrival_tsc;
};

/// A top-of-book change for one instrument.
struct TopEvent {
  std::uint64_t arrival_tsc;
  std::uint64_t exchange_ns;
  book::Top top;
  std::uint16_t locate;
  bool last = false;  ///< end-of-stream marker
};

/// Signals the strategy stage maintains per instrument.
struct Signals {
  double microprice = 0;      ///< size-weighted mid: leans toward the thin side
  double imbalance = 0;       ///< (bid_qty - ask_qty) / (bid_qty + ask_qty), in [-1, 1]
  double ofi = 0;             ///< cumulative order-flow imbalance, shares
  std::uint64_t updates = 0;
  std::uint64_t locked_or_crossed = 0;  ///< bid >= ask: expected only around crosses
  book::Top prev{};
};

enum class Pacing : std::uint8_t {
  none,    ///< as fast as possible: measures throughput and queueing
  rate,    ///< fixed messages per second
  replay,  ///< exchange timestamps, time-compressed by `speed`
};

struct PipelineConfig {
  int feed_cpu = 1, strategy_cpu = 3;
  std::vector<int> book_cpus{2};  ///< one per book shard
  Pacing pacing = Pacing::none;
  double rate_per_s = 1e6;
  double speed = 1.0;
  std::uint64_t limit = 0;  ///< messages, 0 = all
  /// Measurement window in exchange time (ns since midnight). Messages before
  /// `window_from_ns` are replayed flat out to build the book and are not
  /// measured; the queues then drain, and pacing starts at the window.
  std::uint64_t window_from_ns = 0;
  std::uint64_t window_to_ns = ~std::uint64_t{0};
};

/// Each stage writes only its own block, and blocks are 128-byte aligned, so
/// per-message bookkeeping never false-shares a cache line across cores.
struct PipelineResult {
  alignas(128) itch::ReplayStats replay;  ///< written by feed (+ error counts by book)
  std::uint64_t measured = 0;              ///< messages inside the window
  alignas(128) LogHistogram arrival_to_book;    ///< written by book: every message
  alignas(128) LogHistogram arrival_to_signal;  ///< written by strategy: every top change
  std::uint64_t top_events = 0;
  alignas(128) double seconds = 0;
  std::vector<std::uint64_t> shard_messages;  ///< load balance across book shards
};

namespace detail {

/// Cont, Kukanov & Stoikov (2014): signed change in queue size at the touch.
inline double ofi_increment(const book::Top& prev, const book::Top& cur) {
  double e = 0;
  if (cur.bid_price >= prev.bid_price) e += static_cast<double>(cur.bid_shares);
  if (cur.bid_price <= prev.bid_price) e -= static_cast<double>(prev.bid_shares);
  if (cur.ask_price <= prev.ask_price) e -= static_cast<double>(cur.ask_shares);
  if (cur.ask_price >= prev.ask_price) e += static_cast<double>(prev.ask_shares);
  return e;
}

inline void update_signals(Signals& s, const book::Top& t) {
  if (t.bid_shares != 0 && t.ask_shares != 0) {
    const double bq = static_cast<double>(t.bid_shares), aq = static_cast<double>(t.ask_shares);
    s.microprice = (t.bid_price * aq + t.ask_price * bq) / (aq + bq) * itch::kPrice4Scale;
    s.imbalance = (bq - aq) / (bq + aq);
    if (t.bid_price >= t.ask_price) ++s.locked_or_crossed;
  }
  if (s.updates != 0) s.ofi += ofi_increment(s.prev, t);
  s.prev = t;
  ++s.updates;
}

}  // namespace detail

/// Read-only view over the per-shard books with the same accessors as one
/// BookBuilder, so reporting code does not care how many shards there are.
template <class Book>
class ShardedBookView {
 public:
  static constexpr std::size_t kMaxLocate = Book::kMaxLocate;
  explicit ShardedBookView(const std::vector<std::unique_ptr<Book>>& books) : books_(&books) {}

  [[nodiscard]] const Book& shard(std::uint16_t loc) const {
    return *(*books_)[loc % books_->size()];
  }
  [[nodiscard]] const auto& symbol(std::uint16_t loc) const { return shard(loc).symbol(loc); }
  [[nodiscard]] const auto& top(std::uint16_t loc) const { return shard(loc).top(loc); }
  [[nodiscard]] const auto& book(std::uint16_t loc) const { return shard(loc).book(loc); }
  [[nodiscard]] book::BookStats stats() const {
    book::BookStats t;
    for (const auto& b : *books_) {
      const auto& s = b->stats();
      t.adds += s.adds;
      t.executions += s.executions;
      t.cancels += s.cancels;
      t.deletes += s.deletes;
      t.replaces += s.replaces;
      t.unknown_order += s.unknown_order;
      t.level_mismatch += s.level_mismatch;
      t.peak_live_orders += s.peak_live_orders;  // sum of per-shard peaks (upper bound)
    }
    return t;
  }
  [[nodiscard]] std::size_t order_map_rehashes() const {
    std::size_t n = 0;
    for (const auto& b : *books_) n += b->order_map_rehashes();
    return n;
  }

 private:
  const std::vector<std::unique_ptr<Book>>* books_;
};

/// feed -> N book shards -> strategy. Instruments are partitioned across the
/// book threads by stock_locate (every ITCH message carries it, so an order's
/// whole lifecycle lands on one shard); each shard owns a private book and
/// order table, so the shards share nothing and scale with cores. N = 1 is
/// the plain three-stage pipeline.
template <class Book = book::FastBook>
class Pipeline {
 public:
  static constexpr std::size_t kRawRing = 1 << 16;
  static constexpr std::size_t kTopRing = 1 << 14;
  static constexpr std::size_t kMaxShards = 8;

  Pipeline(std::size_t shards, std::size_t expected_orders) : signals_(Book::kMaxLocate) {
    if (shards == 0 || shards > kMaxShards) throw std::invalid_argument("1..8 book shards");
    for (std::size_t i = 0; i < shards; ++i) {
      books_.push_back(std::make_unique<Book>(expected_orders));
      raw_.push_back(std::make_unique<spsc::RingBuffer<RawEvent>>(kRawRing));
      tops_.push_back(std::make_unique<spsc::RingBuffer<TopEvent>>(kTopRing));
    }
  }
  explicit Pipeline(std::size_t expected_orders = 1 << 22) : Pipeline(1, expected_orders) {}

  /// cfg.book_cpus should have one CPU per shard.
  PipelineResult run(std::span<const std::byte> capture, const PipelineConfig& cfg) {
    PipelineResult r;
    const std::size_t n = books_.size();
    std::vector<ShardResult> shard_results(n);
    std::atomic<std::size_t> ready{0};

    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < n; ++i) {
      threads.emplace_back([&, i] {
        harness::pin_current_thread(cfg.book_cpus[i % cfg.book_cpus.size()]);
        harness::raise_current_thread_priority();
        ready.fetch_add(1);
        book_loop(i, shard_results[i]);
      });
    }
    threads.emplace_back([&] {
      harness::pin_current_thread(cfg.strategy_cpu);
      harness::raise_current_thread_priority();
      ready.fetch_add(1);
      strategy_loop(r);
    });

    harness::pin_current_thread(cfg.feed_cpu);
    harness::raise_current_thread_priority();
    // The first tsc_ghz() call calibrates the TSC (~300 ms of sleeping); do it
    // before the clock starts, not inside the feed loop.
    (void)harness::tsc_ghz();
    while (ready.load() < n + 1) std::this_thread::yield();

    const auto t0 = std::chrono::steady_clock::now();
    feed_loop(capture, cfg, r);
    for (auto& t : threads) t.join();
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    for (const auto& s : shard_results) {
      r.arrival_to_book.merge(s.arrival_to_book);
      r.replay.unknown_type += s.unknown_type;
      r.replay.bad_length += s.bad_length;
      r.shard_messages.push_back(s.messages);
    }
    return r;
  }

  [[nodiscard]] ShardedBookView<Book> book() const { return ShardedBookView<Book>(books_); }
  [[nodiscard]] const Signals& signals(std::uint16_t locate) const { return signals_[locate]; }
  [[nodiscard]] std::size_t shards() const noexcept { return books_.size(); }

 private:
  /// Per-shard counters, each in its own cache lines (written only by that shard).
  struct alignas(128) ShardResult {
    LogHistogram arrival_to_book;
    std::uint64_t unknown_type = 0, bad_length = 0, messages = 0;
  };

  bool all_queues_empty() const {
    for (std::size_t i = 0; i < books_.size(); ++i)
      if (!raw_[i]->empty_approx() || !tops_[i]->empty_approx()) return false;
    return true;
  }

  void feed_loop(std::span<const std::byte> capture, const PipelineConfig& cfg,
                 PipelineResult& r) {
    itch::FrameReader reader(capture);
    const std::size_t shards = books_.size();
    const double ghz = harness::tsc_ghz();
    const double cycles_per_msg = cfg.pacing == Pacing::rate ? ghz * 1e9 / cfg.rate_per_s : 0;
    const bool windowed = cfg.window_from_ns != 0;
    std::uint64_t start = 0, first_exchange_ns = 0;
    std::uint64_t n = 0, in_window = 0;
    bool started = false;

    for (auto msg = reader.next(); !msg.empty(); msg = reader.next()) {
      const auto& header = *std::start_lifetime_as<itch::Header>(msg.data());
      const std::uint64_t ts = header.timestamp();
      if (ts > cfg.window_to_ns) break;
      std::uint64_t arrival = 0;  // 0 = not measured (catch-up phase)
      if (!windowed || ts >= cfg.window_from_ns) {
        if (!started) {
          // Let the books finish the catch-up backlog before the clock starts,
          // so the window measures steady-state latency, not leftover queueing.
          while (!all_queues_empty()) {
          }
          start = harness::tsc_now() + harness::ns_to_cycles(1e5);
          first_exchange_ns = ts;
          started = true;
        }
        if (cfg.pacing == Pacing::none) {
          arrival = harness::tsc_now();
        } else {
          arrival = cfg.pacing == Pacing::rate
                        ? start + static_cast<std::uint64_t>(static_cast<double>(in_window) *
                                                             cycles_per_msg)
                        : start + static_cast<std::uint64_t>(
                                      static_cast<double>(ts - first_exchange_ns) / cfg.speed * ghz);
          while (harness::tsc_now() < arrival) {
          }
        }
        ++in_window;
      }
      ++r.replay.by_type[std::to_integer<unsigned char>(msg[0])];
      r.replay.bytes += msg.size() + 2;
      const RawEvent ev{msg.data(), static_cast<std::uint32_t>(msg.size()), arrival};
      auto& ring = *raw_[shards == 1 ? 0 : header.locate() % shards];
      while (!ring.try_push(ev)) {
      }
      if (++n == cfg.limit) break;
    }
    r.replay.messages = n;
    r.measured = in_window;
    for (auto& ring : raw_)
      while (!ring->try_push(RawEvent{nullptr, 0, 0})) {
      }
  }

  /// Drains up to kDrain events per synchronisation. Taking whatever is
  /// already queued adds no latency, and publishing the consumer index once
  /// per batch stops the producer's "is there space?" reads from bouncing the
  /// index's cache line on every message when the queue is busy.
  static constexpr std::size_t kDrain = 64;

  void book_loop(std::size_t shard, ShardResult& r) {
    Book& b = *books_[shard];
    auto& in = *raw_[shard];
    auto& out = *tops_[shard];
    std::array<RawEvent, kDrain> batch;
    for (bool done = false; !done;) {
      const std::size_t n = in.try_pop_n(batch);
      // Group prefetch: start every miss of the batch before applying any of it.
      if constexpr (itch::Prefetching<Book>) {
        for (std::size_t i = 0; i < n; ++i)
          if (batch[i].msg != nullptr) b.prefetch(batch[i].msg, batch[i].len);
      }
      for (std::size_t i = 0; i < n; ++i) {
        const RawEvent& ev = batch[i];
        if (ev.msg == nullptr) {
          done = true;
          break;
        }
        b.reset_change_flag();
        const auto status = itch::dispatch(ev.msg, ev.len, b);
        r.unknown_type += status == itch::Status::unknown_type;
        r.bad_length += status == itch::Status::bad_length;
        ++r.messages;
        if (ev.arrival_tsc != 0) {
          // Fenced read (RDTSCP): an unfenced RDTSC can execute before earlier
          // instructions and read a few ns *before* the feed's stamp.
          const std::uint64_t now = harness::tsc_end();
          r.arrival_to_book.record(now > ev.arrival_tsc ? now - ev.arrival_tsc : 0);
        }
        if (b.top_changed()) {
          const std::uint16_t loc = b.last_locate();
          const TopEvent te{ev.arrival_tsc,
                            std::start_lifetime_as<itch::Header>(ev.msg)->timestamp(),
                            b.top(loc), loc};
          while (!out.try_push(te)) {
          }
        }
      }
    }
    TopEvent end{};
    end.last = true;
    while (!out.try_push(end)) {
    }
  }

  /// Polls every shard's ring in turn until each has delivered its end marker.
  void strategy_loop(PipelineResult& r) {
    std::array<TopEvent, kDrain> batch;
    std::size_t open = tops_.size();
    std::vector<bool> finished(tops_.size(), false);
    while (open != 0) {
      for (std::size_t s = 0; s < tops_.size(); ++s) {
        if (finished[s]) continue;
        const std::size_t n = tops_[s]->try_pop_n(batch);
        for (std::size_t i = 0; i < n; ++i) {
          const TopEvent& te = batch[i];
          if (te.last) {
            finished[s] = true;
            --open;
            break;
          }
          detail::update_signals(signals_[te.locate], te.top);
          if (te.arrival_tsc != 0) {
            const std::uint64_t now = harness::tsc_end();
            r.arrival_to_signal.record(now > te.arrival_tsc ? now - te.arrival_tsc : 0);
          }
          ++r.top_events;
        }
      }
    }
  }

  std::vector<std::unique_ptr<Book>> books_;
  std::vector<Signals> signals_;
  std::vector<std::unique_ptr<spsc::RingBuffer<RawEvent>>> raw_;
  std::vector<std::unique_ptr<spsc::RingBuffer<TopEvent>>> tops_;
};

}  // namespace feed
