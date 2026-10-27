// SPDX-License-Identifier: MIT
//
// Throughput of each stage and each data-structure choice, on one dataset:
//
//   --itch_file=PATH   a NASDAQ ITCH 5.0 BinaryFILE capture (real data), or
//   (default)          a 20 M-message synthetic feed (include/itch/synthetic.hpp)
//   --max_gb=G         use only the first G GB of the capture
//
// Every book variant replays the identical bytes; construction (allocating and
// clearing the order table) is excluded from the timing.
#include <benchmark/benchmark.h>

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "book/book_builder.hpp"
#include "describe.hpp"
#include "itch/mold.hpp"
#include "itch/parser.hpp"
#include "itch/synthetic.hpp"
#include "mapped_file.hpp"
#include "platform.hpp"

namespace {

std::unique_ptr<harness::MappedFile> g_file;
std::vector<std::byte> g_synthetic;
std::span<const std::byte> g_data;
std::uint64_t g_messages = 0;
std::size_t g_expected_orders = 1 << 22;

struct NullHandler {};

/// Lookahead = 0: plain replay; otherwise software-pipelined prefetch that far ahead.
template <class Book, std::size_t Lookahead = 0>
void bm_book(benchmark::State& state) {
  std::uint64_t peak = 0, unknown = 0;
  for (auto _ : state) {
    state.PauseTiming();
    auto book = std::make_unique<Book>(g_expected_orders);
    state.ResumeTiming();
    itch::ReplayStats st;
    if constexpr (Lookahead != 0) st = itch::replay_prefetched<Lookahead>(g_data, *book);
    else st = itch::replay(g_data, *book);
    benchmark::DoNotOptimize(st.messages);
    state.PauseTiming();
    peak = book->stats().peak_live_orders;
    unknown = book->stats().unknown_order + book->stats().level_mismatch;
    book.reset();
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(g_messages));
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(g_data.size()));
  state.counters["ns_per_msg"] = benchmark::Counter(
      static_cast<double>(g_messages), benchmark::Counter::kIsIterationInvariantRate |
                                           benchmark::Counter::kInvert);
  state.counters["peak_orders"] = static_cast<double>(peak);
  if (unknown != 0) state.SkipWithError("book integrity error");
}

void bm_decode(benchmark::State& state) {
  NullHandler h;
  for (auto _ : state) {
    auto st = itch::replay(g_data, h);
    benchmark::DoNotOptimize(st.messages);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(g_messages));
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(g_data.size()));
  state.counters["ns_per_msg"] = benchmark::Counter(
      static_cast<double>(g_messages), benchmark::Counter::kIsIterationInvariantRate |
                                           benchmark::Counter::kInvert);
}

/// MoldUDP64 receive path: sequence tracking + framing + dispatch.
void bm_mold(benchmark::State& state) {
  // Packetize up to 20 M messages once, as they would arrive on the wire.
  std::vector<std::byte> wire;
  std::vector<std::size_t> offsets;
  itch::mold::Packetizer pk("NASDAQ0001");
  itch::FrameReader r(g_data);
  std::uint64_t n = 0;
  auto emit = [&](std::span<const std::byte> p) {
    offsets.push_back(wire.size());
    wire.insert(wire.end(), p.begin(), p.end());
  };
  for (auto m = r.next(); !m.empty() && n < 20'000'000; m = r.next(), ++n) pk.add(m, emit);
  pk.flush(emit);
  offsets.push_back(wire.size());

  NullHandler h;
  for (auto _ : state) {
    itch::mold::Receiver rx;
    for (std::size_t i = 0; i + 1 < offsets.size(); ++i) {
      rx.on_packet(std::span(wire).subspan(offsets[i], offsets[i + 1] - offsets[i]),
                   [&](std::span<const std::byte> m, std::uint64_t) {
                     itch::dispatch(m.data(), m.size(), h);
                   });
    }
    benchmark::DoNotOptimize(rx.expected());
    if (rx.stats().gaps != 0) state.SkipWithError("unexpected gap");
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
  state.counters["packets"] = static_cast<double>(offsets.size() - 1);
}

template <class OrderMap, template <book::Side> class Levels>
using B = book::BookBuilder<OrderMap, Levels>;

template <class Book>
struct Variant {
  using type = Book;
  std::string_view name;
  std::string_view what;
};

inline constexpr std::tuple kVariants{
    Variant<B<book::DirectOrderMap4K, book::LinearLevels>>{"direct_linear_4k_pages",
                                                           "direct-mapped order table on 4 KB pages + vector levels"},
    Variant<B<book::DirectOrderMap, book::LinearLevels>>{"direct_linear",
                                                          "direct-mapped order table + vector levels, linear scan"},
    Variant<B<book::FlatOrderMap, book::LinearLevels>>{"fib_linear",
                                                       "Fibonacci-hashed order table + vector levels, linear scan"},
    Variant<B<book::DirectOrderMap, book::BinaryLevels>>{"direct_binary",
                                                         "direct-mapped order table + vector levels, binary search"},
    Variant<B<book::DirectOrderMap, book::MapLevels>>{"direct_stdmap",
                                                      "direct-mapped order table + std::map levels"},
    Variant<B<book::StdOrderMap, book::LinearLevels>>{"stdhash_linear",
                                                      "std::unordered_map orders + vector levels"},
    Variant<B<book::StdOrderMap, book::MapLevels>>{"reference",
                                                   "std::unordered_map + std::map (textbook)"},
};

void load(const std::string& path, double max_gb) {
  if (!path.empty()) {
    g_file = std::make_unique<harness::MappedFile>(path);
    g_data = g_file->bytes();
    const auto cap = static_cast<std::size_t>(max_gb * 1e9);
    if (max_gb > 0 && g_data.size() > cap) g_data = g_data.first(cap);
    std::fprintf(stderr, "prefaulting %.2f GB of %s ...\n", static_cast<double>(g_data.size()) / 1e9,
                 path.c_str());
    benchmark::DoNotOptimize(g_file->prefault());
    benchmark::AddCustomContext("dataset", path);
  } else {
    g_synthetic = itch::synthetic::make_feed({.messages = 20'000'000, .symbols = 2000, .seed = 1});
    g_data = g_synthetic;
    benchmark::AddCustomContext("dataset", "synthetic: 20M messages, 2000 symbols, seed 1");
  }
  NullHandler h;
  const auto st = itch::replay(g_data, h);
  g_messages = st.messages;
  g_data = g_data.first(st.bytes);  // drop a truncated tail, if any
  benchmark::AddCustomContext("dataset_messages", std::to_string(g_messages));
  benchmark::AddCustomContext("dataset_bytes", std::to_string(g_data.size()));
}

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  double max_gb = 0;
  int reps = 3;
  int out = 1;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a.starts_with("--itch_file=")) path = a.substr(12);
    else if (a.starts_with("--max_gb=")) max_gb = std::stod(std::string(a.substr(9)));
    else if (a.starts_with("--reps=")) reps = std::stoi(std::string(a.substr(7)));
    else argv[out++] = argv[i];
  }
  argc = out;
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;

  harness::raise_process_priority();
  harness::pin_current_thread(harness::logical_cpu_count() - 1);
  harness::raise_current_thread_priority();
  auto m = harness::machine_info();
  benchmark::AddCustomContext("cpu", m.cpu);
  benchmark::AddCustomContext("compiler", m.compiler);
  benchmark::AddCustomContext("build_flags", m.build_flags);
  load(path, max_gb);

  benchmark::RegisterBenchmark("decode/null_handler", bm_decode)
      ->Iterations(1)->Repetitions(reps)->UseRealTime()->Unit(benchmark::kMillisecond)
      ->DisplayAggregatesOnly(true);
  benchmark::RegisterBenchmark("decode/moldudp64", bm_mold)
      ->Iterations(1)->Repetitions(reps)->UseRealTime()->Unit(benchmark::kMillisecond)
      ->DisplayAggregatesOnly(true);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"  // GCC 16 false positive on template-for variables
  template for (constexpr auto v : kVariants) {
    using Book = typename std::remove_cvref_t<decltype(v)>::type;
    benchmark::AddCustomContext("book " + std::string(v.name), std::string(v.what));
    benchmark::RegisterBenchmark(("book/" + std::string(v.name)).c_str(), bm_book<Book>)
        ->Iterations(1)->Repetitions(reps)->UseRealTime()->Unit(benchmark::kMillisecond)
        ->DisplayAggregatesOnly(true);
  }
#pragma GCC diagnostic pop
  // The production configuration with software-pipelined prefetching at
  // several lookahead distances (too short: misses not hidden; too long:
  // prefetched lines evicted before use).
  auto prefetch_variant = [&]<std::size_t K>() {
    benchmark::RegisterBenchmark(("book/direct_linear_prefetch" + std::to_string(K)).c_str(),
                                 bm_book<book::FastBook, K>)
        ->Iterations(1)->Repetitions(reps)->UseRealTime()->Unit(benchmark::kMillisecond)
        ->DisplayAggregatesOnly(true);
  };
  prefetch_variant.template operator()<8>();
  prefetch_variant.template operator()<16>();
  prefetch_variant.template operator()<32>();
  prefetch_variant.template operator()<64>();

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
