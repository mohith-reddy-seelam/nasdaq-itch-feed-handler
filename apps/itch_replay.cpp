// SPDX-License-Identifier: MIT
//
// Replays a NASDAQ TotalView-ITCH 5.0 capture (BinaryFILE format).
//
//   itch_replay FILE --mode=stats
//       message counts by type, time span, raw decode rate
//   itch_replay FILE --mode=inline [--out=DIR]
//       single-threaded full-depth book build; per-message-type apply latency;
//       top symbols with volume, VWAP and the official open/close cross
//   itch_replay FILE --mode=noise [--out=DIR]
//       OS-noise probe on the three pipeline CPUs (FILE is ignored)
//   itch_replay FILE --mode=pipeline [--pace=none|rate:N|replay:SPEED] [--out=DIR]
//       feed -> book -> strategy on three pinned cores, latency per stage
//
// Common: --limit=N (messages), --cpus=FEED,BOOK[,BOOK...],STRATEGY, --top=N
//   (more than one BOOK cpu = the book is sharded by instrument across them)
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <meta>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "book/book_builder.hpp"
#include "describe.hpp"
#include "feed/histogram.hpp"
#include "feed/pipeline.hpp"
#include "itch/parser.hpp"
#include "mapped_file.hpp"
#include "platform.hpp"

namespace {

using Clock = std::chrono::steady_clock;

/// Message-type names, generated from the struct names by C++26 reflection:
/// kTypeName['A'] == "AddOrder".
const auto kTypeName = [] {
  std::array<std::string_view, 256> names{};
  [&]<class... Ms>(std::tuple<Ms...>*) {
    ((names[static_cast<unsigned char>(Ms::kType)] = std::meta::identifier_of(^^Ms)), ...);
  }(static_cast<itch::AllMessages*>(nullptr));
  return names;
}();

struct Options {
  std::string file;
  std::string mode = "inline";
  std::string pace = "none";
  std::string out;
  std::string window;  ///< "HH:MM:SS-HH:MM:SS" exchange time
  std::uint64_t limit = 0;
  std::vector<int> cpus{1, 2, 3};  ///< feed, book shard(s)..., strategy
  int top = 15;
  bool prefetch = true;
};

/// Run summary, serialised to summary.json by reflection.
struct Summary {
  std::string file;
  std::string mode;
  std::string pacing;
  std::uint64_t messages = 0;
  std::uint64_t bytes = 0;
  double seconds = 0;
  double messages_per_second = 0;
  double gigabytes_per_second = 0;
  std::uint64_t unknown_type = 0;
  std::uint64_t bad_length = 0;
  std::uint64_t unknown_order_refs = 0;
  std::uint64_t level_mismatches = 0;
  std::uint64_t peak_live_orders = 0;
  std::uint64_t order_map_rehashes = 0;
  std::uint64_t top_of_book_changes = 0;
  std::uint64_t measured_messages = 0;  ///< inside the --window (pipeline mode)
  std::uint64_t book_shards = 1;
  std::string window;
  std::uint64_t first_timestamp_ns = 0;
  std::uint64_t last_timestamp_ns = 0;
};

std::string hhmmss(std::uint64_t ns) {
  const auto s = ns / 1'000'000'000;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu.%06llu",
                static_cast<unsigned long long>(s / 3600),
                static_cast<unsigned long long>(s / 60 % 60),
                static_cast<unsigned long long>(s % 60),
                static_cast<unsigned long long>(ns / 1000 % 1'000'000));
  return buf;
}

double px(std::uint32_t p) { return p * itch::kPrice4Scale; }

// ---- output helpers ---------------------------------------------------------------

struct LatencyRow {
  std::string stage;
  const feed::LogHistogram* h;
};

void print_latency(const std::vector<LatencyRow>& rows) {
  const double ns = 1.0 / harness::tsc_ghz();
  std::printf("\n%-26s %12s %9s %9s %9s %10s %10s %10s\n", "latency (ns)", "count", "p50", "p90",
              "p99", "p99.9", "p99.99", "max");
  for (const auto& r : rows) {
    if (r.h->count() == 0) continue;
    std::printf("%-26s %12llu %9.0f %9.0f %9.0f %10.0f %10.0f %10.0f\n", r.stage.c_str(),
                static_cast<unsigned long long>(r.h->count()), r.h->quantile(0.5) * ns,
                r.h->quantile(0.9) * ns, r.h->quantile(0.99) * ns, r.h->quantile(0.999) * ns,
                r.h->quantile(0.9999) * ns, r.h->quantile(1.0) * ns);
  }
}

void write_latency(const std::filesystem::path& dir, const std::vector<LatencyRow>& rows) {
  const double ns = 1.0 / harness::tsc_ghz();
  std::filesystem::create_directories(dir / "hist");
  std::ofstream f(dir / "latency_summary.csv");
  f << "stage,count,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns\n";
  for (const auto& r : rows) {
    if (r.h->count() == 0) continue;
    f << r.stage << ',' << r.h->count() << ',' << r.h->quantile(0.5) * ns << ','
      << r.h->quantile(0.9) * ns << ',' << r.h->quantile(0.99) * ns << ','
      << r.h->quantile(0.999) * ns << ',' << r.h->quantile(0.9999) * ns << ','
      << r.h->quantile(1.0) * ns << '\n';
    std::ofstream h(dir / "hist" / (r.stage + ".csv"));
    r.h->write_csv(h, ns);
  }
}

void print_counts(const itch::ReplayStats& st) {
  std::printf("\n%-30s %4s %14s %8s\n", "message type", "", "count", "share");
  std::vector<int> types;
  for (int t = 0; t < 256; ++t)
    if (st.by_type[t] != 0) types.push_back(t);
  std::ranges::sort(types, [&](int a, int b) { return st.by_type[a] > st.by_type[b]; });
  for (int t : types) {
    const auto name = kTypeName[t].empty() ? std::string_view("(unknown)") : kTypeName[t];
    std::printf("%-30.*s '%c' %14llu %7.2f%%\n", static_cast<int>(name.size()), name.data(),
                static_cast<char>(t), static_cast<unsigned long long>(st.by_type[t]),
                100.0 * static_cast<double>(st.by_type[t]) / static_cast<double>(st.messages));
  }
}

void write_counts(const std::filesystem::path& dir, const itch::ReplayStats& st) {
  std::ofstream f(dir / "message_counts.csv");
  f << "type,name,count\n";
  for (int t = 0; t < 256; ++t)
    if (st.by_type[t] != 0) f << static_cast<char>(t) << ',' << kTypeName[t] << ',' << st.by_type[t] << '\n';
}

template <class Book>
void report_symbols(const Book& b, const std::vector<feed::Signals>* signals, int top,
                    const std::filesystem::path* dir) {
  std::vector<std::uint16_t> locs;
  for (std::size_t i = 1; i < Book::kMaxLocate; ++i)
    if (b.symbol(static_cast<std::uint16_t>(i)).traded_shares != 0)
      locs.push_back(static_cast<std::uint16_t>(i));
  std::ranges::sort(locs, [&](auto x, auto y) {
    return b.symbol(x).traded_notional > b.symbol(y).traded_notional;
  });

  std::printf("\n%-8s %14s %10s %10s %10s %11s %11s %12s %8s %8s\n", "symbol", "volume",
              "VWAP", "open X", "close X", "bid", "ask", "top chg", "levels", "OFI/vol");
  for (int i = 0; i < top && i < static_cast<int>(locs.size()); ++i) {
    const auto loc = locs[i];
    const auto& s = b.symbol(loc);
    const auto& t = b.top(loc);
    const double ofi = signals ? (*signals)[loc].ofi : 0;
    std::printf("%-8.*s %14llu %10.4f %10.4f %10.4f %11.4f %11.4f %12llu %8zu %8.3f\n",
                static_cast<int>(s.symbol().size()), s.symbol().data(),
                static_cast<unsigned long long>(s.traded_shares),
                s.traded_notional / static_cast<double>(s.traded_shares), px(s.open_cross_price),
                px(s.close_cross_price), px(t.bid_price), px(t.ask_price),
                static_cast<unsigned long long>(s.top_changes),
                b.book(loc).bids.depth() + b.book(loc).asks.depth(),
                ofi / static_cast<double>(s.traded_shares));
  }
  if (dir == nullptr) return;
  std::ofstream f(*dir / "symbols.csv");
  f << "symbol,locate,traded_shares,vwap,open_cross,close_cross,close_cross_shares,top_changes,"
       "book_events,final_bid,final_ask,ofi\n";
  for (auto loc : locs) {
    const auto& s = b.symbol(loc);
    const auto& t = b.top(loc);
    f << s.symbol() << ',' << loc << ',' << s.traded_shares << ','
      << s.traded_notional / static_cast<double>(s.traded_shares) << ',' << px(s.open_cross_price)
      << ',' << px(s.close_cross_price) << ',' << s.close_cross_shares << ',' << s.top_changes
      << ',' << s.book_events << ',' << px(t.bid_price) << ',' << px(t.ask_price) << ','
      << (signals ? (*signals)[loc].ofi : 0) << '\n';
  }
}

// ---- modes ------------------------------------------------------------------------

/// Counts messages without touching a book: the decode + framing floor.
struct NullHandler {};

void run_stats(std::span<const std::byte> data, const Options& o, Summary& sum) {
  NullHandler h;
  const auto t0 = Clock::now();
  const auto st = itch::replay(data, h, o.limit);
  sum.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
  sum.messages = st.messages;
  sum.bytes = st.bytes;
  sum.unknown_type = st.unknown_type;
  sum.bad_length = st.bad_length;
  print_counts(st);
  if (st.truncated_tail_bytes)
    std::printf("\n%llu bytes of a truncated message at the end were ignored\n",
                static_cast<unsigned long long>(st.truncated_tail_bytes));
  if (!o.out.empty()) write_counts(o.out, st);
}

/// Wraps the book: times each book update (two TSC reads) into a per-type
/// histogram, and forwards prefetch hints.
struct TimedBook {
  book::FastBook& book;
  std::vector<feed::LogHistogram>& by_type;

  template <class M>
    requires itch::Handles<book::FastBook, M>
  void on(const M& m) {
    const std::uint64_t a = harness::tsc_now();
    book.on(m);
    by_type[static_cast<unsigned char>(M::kType)].record(harness::tsc_now() - a);
  }
  void prefetch(const std::byte* p, std::size_t n) const noexcept { book.prefetch(p, n); }
};

/// Single thread: decode + full-depth book, timing every book update by type.
void run_inline(std::span<const std::byte> data, const Options& o, Summary& sum) {
  auto book = std::make_unique<book::FastBook>(1 << 23);
  // One histogram per type byte: 256 x 10.5 KB, too big for a 1 MB Windows stack.
  std::vector<feed::LogHistogram> by_type(256);
  TimedBook timed{*book, by_type};

  const auto t0 = Clock::now();
  const itch::ReplayStats st = o.prefetch ? itch::replay_prefetched(data, timed, o.limit)
                                          : itch::replay(data, timed, o.limit);
  sum.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
  std::printf("software prefetch: %s\n", o.prefetch ? "on (8-message lookahead)" : "off");
  sum.messages = st.messages;
  sum.bytes = st.bytes;
  sum.unknown_type = st.unknown_type;
  sum.bad_length = st.bad_length;
  sum.unknown_order_refs = book->stats().unknown_order;
  sum.level_mismatches = book->stats().level_mismatch;
  sum.peak_live_orders = book->stats().peak_live_orders;
  sum.order_map_rehashes = book->order_map_rehashes();

  std::uint64_t tops = 0;
  for (std::size_t i = 0; i < book::FastBook::kMaxLocate; ++i)
    tops += book->symbol(static_cast<std::uint16_t>(i)).top_changes;
  sum.top_of_book_changes = tops;

  print_counts(st);
  std::vector<LatencyRow> rows;
  for (char t : {'A', 'F', 'E', 'C', 'X', 'D', 'U', 'P', 'Q'}) {
    rows.push_back({std::string("apply_") + std::string(kTypeName[static_cast<unsigned char>(t)]),
                    &by_type[static_cast<unsigned char>(t)]});
  }
  print_latency(rows);
  const std::filesystem::path out(o.out);
  report_symbols(*book, nullptr, o.top, o.out.empty() ? nullptr : &out);
  if (!o.out.empty()) {
    write_counts(o.out, st);
    write_latency(o.out, rows);
  }
}

void run_pipeline(std::span<const std::byte> data, const Options& o, Summary& sum) {
  feed::PipelineConfig cfg;
  cfg.feed_cpu = o.cpus.front();
  cfg.strategy_cpu = o.cpus.back();
  cfg.book_cpus.assign(o.cpus.begin() + 1, o.cpus.end() - 1);
  cfg.limit = o.limit;
  if (o.pace.starts_with("rate:")) {
    cfg.pacing = feed::Pacing::rate;
    cfg.rate_per_s = std::stod(o.pace.substr(5));
  } else if (o.pace.starts_with("replay:")) {
    cfg.pacing = feed::Pacing::replay;
    cfg.speed = std::stod(o.pace.substr(7));
  }

  if (!o.window.empty()) {
    unsigned h1, m1, s1, h2, m2, s2;
    if (std::sscanf(o.window.c_str(), "%u:%u:%u-%u:%u:%u", &h1, &m1, &s1, &h2, &m2, &s2) != 6) {
      std::fprintf(stderr, "bad --window, expected HH:MM:SS-HH:MM:SS\n");
      std::exit(2);
    }
    cfg.window_from_ns = (h1 * 3600ull + m1 * 60ull + s1) * 1'000'000'000ull;
    cfg.window_to_ns = (h2 * 3600ull + m2 * 60ull + s2) * 1'000'000'000ull;
  }

  auto pipe = std::make_unique<feed::Pipeline<>>(cfg.book_cpus.size(), 1 << 23);
  const auto r = pipe->run(data, cfg);
  sum.seconds = r.seconds;
  sum.messages = r.replay.messages;
  sum.bytes = r.replay.bytes;
  sum.unknown_type = r.replay.unknown_type;
  sum.bad_length = r.replay.bad_length;
  sum.unknown_order_refs = pipe->book().stats().unknown_order;
  sum.level_mismatches = pipe->book().stats().level_mismatch;
  sum.peak_live_orders = pipe->book().stats().peak_live_orders;
  sum.order_map_rehashes = pipe->book().order_map_rehashes();
  sum.top_of_book_changes = r.top_events;
  sum.measured_messages = r.measured;
  sum.book_shards = pipe->shards();
  if (pipe->shards() > 1) {
    std::printf("book shards: %zu, messages per shard:", pipe->shards());
    for (auto m : r.shard_messages)
      std::printf(" %.1f%%", 100.0 * static_cast<double>(m) / static_cast<double>(r.replay.messages));
    std::printf("\n");
  }

  const std::vector<LatencyRow> rows = {{"arrival_to_book", &r.arrival_to_book},
                                        {"arrival_to_signal", &r.arrival_to_signal}};
  print_latency(rows);
  std::vector<feed::Signals> sig(book::FastBook::kMaxLocate);
  for (std::size_t i = 0; i < sig.size(); ++i) sig[i] = pipe->signals(static_cast<std::uint16_t>(i));
  std::filesystem::path out(o.out);
  report_symbols(pipe->book(), &sig, o.top, o.out.empty() ? nullptr : &out);
  if (!o.out.empty()) {
    write_counts(o.out, r.replay);
    write_latency(o.out, rows);
  }
}

/// Platform noise ("hiccup meter"): a pinned thread spins on the TSC and
/// records every gap longer than 1 us, i.e. every time the OS took the core.
/// This is the floor under every tail percentile the pipeline reports.
void run_noise(const Options& o) {
  std::vector<std::unique_ptr<feed::LogHistogram>> hists;
  std::vector<LatencyRow> rows;
  const std::uint64_t threshold = harness::ns_to_cycles(1000);
  const std::uint64_t duration = harness::ns_to_cycles(5e9);
  std::printf("%-8s %12s %12s%s", "cpu", "gaps/s", "stolen %", "\n");
  for (int cpu : o.cpus) {
    auto h = std::make_unique<feed::LogHistogram>();
    std::uint64_t stolen = 0;
    std::thread probe([&] {
      harness::pin_current_thread(cpu);
      harness::raise_current_thread_priority();
      const std::uint64_t start = harness::tsc_now();
      for (std::uint64_t prev = start; prev - start < duration;) {
        const std::uint64_t now = harness::tsc_now();
        if (now - prev > threshold) {
          h->record(now - prev);
          stolen += now - prev;
        }
        prev = now;
      }
    });
    probe.join();
    std::printf("%-8d %12.0f %11.2f%%%s", cpu, static_cast<double>(h->count()) / 5.0,
                100.0 * static_cast<double>(stolen) / static_cast<double>(duration), "\n");
    rows.push_back({"noise_cpu" + std::to_string(cpu), h.get()});
    hists.push_back(std::move(h));
  }
  print_latency(rows);
  if (!o.out.empty()) write_latency(o.out, rows);
}

bool take(std::string_view a, std::string_view name, std::string& v) {
  const std::string p = "--" + std::string(name) + "=";
  if (!a.starts_with(p)) return false;
  v = a.substr(p.size());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string v;
    const std::string_view a = argv[i];
    if (take(a, "mode", v)) o.mode = v;
    else if (take(a, "pace", v)) o.pace = v;
    else if (take(a, "out", v)) o.out = v;
    else if (take(a, "window", v)) o.window = v;
    else if (a == "--no-prefetch") o.prefetch = false;
    else if (take(a, "limit", v)) o.limit = std::stoull(v);
    else if (take(a, "top", v)) o.top = std::stoi(v);
    else if (take(a, "cpus", v)) {
      o.cpus.clear();
      for (std::size_t pos = 0; pos <= v.size();) {
        const std::size_t comma = std::min(v.find(',', pos), v.size());
        o.cpus.push_back(std::stoi(v.substr(pos, comma - pos)));
        pos = comma + 1;
      }
      if (o.cpus.size() < 3) {
        std::fprintf(stderr, "--cpus needs FEED,BOOK[,BOOK...],STRATEGY\n");
        return 2;
      }
    }
    else if (!a.starts_with("--") && o.file.empty()) o.file = a;
    else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return 2;
    }
  }
  if (o.file.empty()) {
    std::fprintf(stderr,
                 "usage: itch_replay FILE [--mode=stats|inline|pipeline] "
                 "[--pace=none|rate:N|replay:SPEED] [--limit=N] [--cpus=F,B[,B..],S] [--out=DIR]\n");
    return 2;
  }
  if (!o.out.empty()) std::filesystem::create_directories(o.out);

  harness::raise_process_priority();
  std::printf("TSC calibrated at %.4f GHz\n", harness::tsc_ghz());  // before any timing
  if (o.mode != "pipeline") {
    harness::pin_current_thread(o.cpus[1]);
    harness::raise_current_thread_priority();
  }

  if (o.mode == "noise") {  // does not need the capture
    run_noise(o);
    return 0;
  }

  harness::MappedFile file(o.file);
  const auto data = file.bytes();
  std::printf("%s: %.2f GB, mode=%s\n", o.file.c_str(), static_cast<double>(data.size()) / 1e9,
              o.mode.c_str());
  file.advise_sequential();
  const auto warm0 = Clock::now();
  const std::size_t checksum = file.prefault();
  std::printf("prefaulted in %.1f s (checksum %zu)\n",
              std::chrono::duration<double>(Clock::now() - warm0).count(), checksum);

  // First and last exchange timestamps: the time span the capture covers.
  Summary sum;
  sum.file = std::filesystem::path(o.file).filename().string();
  sum.mode = o.mode;
  sum.pacing = o.mode == "pipeline" ? o.pace : "none";
  sum.window = o.window;
  {
    itch::FrameReader r(data);
    if (auto m = r.next(); !m.empty())
      sum.first_timestamp_ns = std::start_lifetime_as<itch::Header>(m.data())->timestamp();
  }

  if (o.mode == "stats") run_stats(data, o, sum);
  else if (o.mode == "inline") run_inline(data, o, sum);
  else if (o.mode == "pipeline") run_pipeline(data, o, sum);
  else {
    std::fprintf(stderr, "unknown mode %s\n", o.mode.c_str());
    return 2;
  }

  sum.messages_per_second = static_cast<double>(sum.messages) / sum.seconds;
  sum.gigabytes_per_second = static_cast<double>(sum.bytes) / sum.seconds / 1e9;
  {
    // Walk to the last complete message for the end timestamp.
    itch::FrameReader r(data);
    std::span<const std::byte> last;
    std::uint64_t n = 0;
    for (auto m = r.next(); !m.empty(); m = r.next()) {
      last = m;
      if (++n == sum.messages) break;
    }
    if (!last.empty()) sum.last_timestamp_ns = std::start_lifetime_as<itch::Header>(last.data())->timestamp();
  }

  std::printf("\n%llu messages (%s .. %s exchange time) in %.2f s: %.1f M msg/s, %.2f GB/s\n",
              static_cast<unsigned long long>(sum.messages), hhmmss(sum.first_timestamp_ns).c_str(),
              hhmmss(sum.last_timestamp_ns).c_str(), sum.seconds, sum.messages_per_second / 1e6,
              sum.gigabytes_per_second);
  if (sum.measured_messages != 0 && !o.window.empty())
    std::printf("latency measured on %llu messages inside window %s\n",
                static_cast<unsigned long long>(sum.measured_messages), o.window.c_str());
  std::printf("integrity: unknown types %llu, bad lengths %llu, unknown order refs %llu, "
              "level mismatches %llu\n",
              static_cast<unsigned long long>(sum.unknown_type),
              static_cast<unsigned long long>(sum.bad_length),
              static_cast<unsigned long long>(sum.unknown_order_refs),
              static_cast<unsigned long long>(sum.level_mismatches));
  if (sum.peak_live_orders)
    std::printf("peak live orders %llu, order-map rehashes %llu, top-of-book changes %llu\n",
                static_cast<unsigned long long>(sum.peak_live_orders),
                static_cast<unsigned long long>(sum.order_map_rehashes),
                static_cast<unsigned long long>(sum.top_of_book_changes));

  if (!o.out.empty()) {
    auto m = harness::machine_info();
    m.producer_cpu = o.cpus[0];
    m.consumer_cpu = o.cpus[1];
    m.priority = "HIGH_PRIORITY_CLASS / THREAD_PRIORITY_TIME_CRITICAL";
    std::ofstream(std::filesystem::path(o.out) / "machine.json") << harness::to_json(m);
    std::ofstream(std::filesystem::path(o.out) / "summary.json") << harness::to_json(sum);
  }
  return 0;
}
