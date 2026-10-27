#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Profile-guided build: instrument, train on real data, rebuild with the profile.
#
#   scripts/pgo_build.sh ITCH_FILE [TRAIN_MESSAGES]
#
# Training replays the first TRAIN_MESSAGES (default 30 M) of the capture
# through the single-threaded book and the pipeline. Evaluating on the same
# day overlaps with the training set; that is stated wherever PGO numbers are
# reported (docs/RESULTS.md).
set -euo pipefail
cd "$(dirname "$0")/.."
FILE="${1:?usage: pgo_build.sh ITCH_FILE [TRAIN_MESSAGES]}"
TRAIN="${2:-30000000}"
CPUS="${CPUS:-1,2,3}"
BIN=build/release

rm -rf "${BIN}/pgo-profile"
cmake --preset release -DITCH_PGO=generate >/dev/null
cmake --build --preset release --target itch_replay itch_bench
"${BIN}/itch_replay" "$FILE" --mode=inline --limit="$TRAIN" --cpus="$CPUS" >/dev/null
"${BIN}/itch_replay" "$FILE" --mode=pipeline --limit="$TRAIN" --cpus="$CPUS" >/dev/null
cmake --preset release -DITCH_PGO=use >/dev/null
cmake --build --preset release --target itch_replay itch_bench
echo "PGO build ready (trained on ${TRAIN} messages of $(basename "$FILE"))"
