// SPDX-License-Identifier: MIT
//
// Writes a synthetic ITCH 5.0 BinaryFILE capture (see include/itch/synthetic.hpp).
//
//   itch_gen OUT.bin [--messages=N] [--symbols=S] [--seed=X]
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>

#include "itch/parser.hpp"
#include "itch/synthetic.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: itch_gen OUT.bin [--messages=N] [--symbols=S] [--seed=X]\n");
    return 2;
  }
  itch::synthetic::Config cfg;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto val = [&](std::string_view key) { return std::string(a.substr(key.size())); };
    if (a.starts_with("--messages=")) cfg.messages = std::stoull(val("--messages="));
    else if (a.starts_with("--symbols=")) cfg.symbols = static_cast<std::uint16_t>(std::stoul(val("--symbols=")));
    else if (a.starts_with("--seed=")) cfg.seed = std::stoull(val("--seed="));
    else {
      std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
      return 2;
    }
  }
  const auto feed = itch::synthetic::make_feed(cfg);
  std::ofstream(argv[1], std::ios::binary)
      .write(reinterpret_cast<const char*>(feed.data()), static_cast<std::streamsize>(feed.size()));
  struct Null {} counter;
  const auto written = itch::replay(feed, counter).messages;  // actual, not requested
  std::printf("wrote %s: %llu messages, %.1f MB\n", argv[1],
              static_cast<unsigned long long>(written), static_cast<double>(feed.size()) / 1e6);
  return 0;
}
