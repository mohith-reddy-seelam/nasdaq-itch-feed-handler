#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Full evaluation on one NASDAQ day:
#
#   scripts/run_all.sh ITCH_FILE [RESULTS_NAME]
#   scripts/run_all.sh /d/market-data/nasdaq/12302019.NASDAQ_ITCH50.bin nasdaq_20191230
#
# Produces results/NAME/{noise,stats,inline,pipeline_*}/, bench.json, plots/
# and SUMMARY.md. Pipeline threads run on CPUs 1,2,3 (override with CPUS=a,b,c);
# CPU 0 is left to the OS.
set -euo pipefail
cd "$(dirname "$0")/.."

FILE="${1:?usage: run_all.sh ITCH_FILE [RESULTS_NAME]}"
NAME="${2:-$(basename "$FILE" .bin)}"
OUT="results/${NAME}"
CPUS="${CPUS:-1,2,3}"
BENCH_GB="${BENCH_GB:-3}"  # how much of the day the data-structure benchmark replays
PY="${PYTHON:-$(command -v python3 || command -v python)}"
BIN=build/release
EXE=""; [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]] && EXE=".exe"
R="${BIN}/itch_replay${EXE}"

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

mkdir -p "$OUT"

step "build + unit tests"
cmake --preset release >/dev/null
cmake --build --preset release
"${BIN}/itch_tests${EXE}" --gtest_brief=1

step "differential test on the real capture (first 50 M messages)"
ITCH_FILE="$FILE" ITCH_DIFF_LIMIT=50000000 "${BIN}/itch_tests${EXE}" \
  --gtest_filter='Differential.RealNasdaqCapture' --gtest_brief=1 | tee "${OUT}/differential_test.log"

step "platform noise on CPUs ${CPUS}"
"$R" - --mode=noise --cpus="$CPUS" --out="${OUT}/noise" | tee "${OUT}/noise.txt"

step "message statistics"
"$R" "$FILE" --mode=stats --cpus="$CPUS" --out="${OUT}/stats" | tee "${OUT}/stats.txt"

step "single-threaded full-depth book build, per-message-type latency"
"$R" "$FILE" --mode=inline --cpus="$CPUS" --top=25 --out="${OUT}/inline" | tee "${OUT}/inline.txt"

step "pipeline, flat out (throughput)"
"$R" "$FILE" --mode=pipeline --pace=none --cpus="$CPUS" --top=25 \
  --out="${OUT}/pipeline_max" | tee "${OUT}/pipeline_max.txt"

step "pipeline, opening auction replayed in real time (09:29:00-09:32:00)"
"$R" "$FILE" --mode=pipeline --pace=replay:1 --window=09:29:00-09:32:00 --cpus="$CPUS" \
  --top=25 --out="${OUT}/pipeline_open" | tee "${OUT}/pipeline_open.txt"

step "pipeline, closing cross replayed in real time (15:59:00-16:01:00)"
"$R" "$FILE" --mode=pipeline --pace=replay:1 --window=15:59:00-16:01:00 --cpus="$CPUS" \
  --top=25 --out="${OUT}/pipeline_close" | tee "${OUT}/pipeline_close.txt"

step "data-structure benchmarks (first ${BENCH_GB} GB of the day)"
"${BIN}/itch_bench${EXE}" --itch_file="$FILE" --max_gb="$BENCH_GB" --reps=3 \
  --benchmark_out="${OUT}/bench.json" --benchmark_out_format=json | tee "${OUT}/bench.txt"

if command -v perf >/dev/null && [[ -z "$EXE" ]]; then
  step "perf flame graph of the single-threaded book build (first 50 M messages)"
  [[ -d third_party/FlameGraph ]] ||
    git clone -q --depth 1 https://github.com/brendangregg/FlameGraph.git third_party/FlameGraph
  book_cpu=$(echo "$CPUS" | cut -d, -f2)
  sudo perf record -q -F 4999 -g -C "$book_cpu" -o "${OUT}/perf.data" -- \
    "$R" "$FILE" --mode=inline --limit=50000000 --cpus="$CPUS" > /dev/null
  sudo perf script -i "${OUT}/perf.data" 2>/dev/null |
    third_party/FlameGraph/stackcollapse-perf.pl > "${OUT}/book_build.folded"
  third_party/FlameGraph/flamegraph.pl --title "Full-depth book build, real NASDAQ data" \
    --countname samples "${OUT}/book_build.folded" > "${OUT}/book_build_flamegraph.svg"
  sudo rm -f "${OUT}/perf.data"
fi

step "plots + summary"
"$PY" tools/analyze.py "$OUT"
echo "done: ${OUT}/SUMMARY.md"
