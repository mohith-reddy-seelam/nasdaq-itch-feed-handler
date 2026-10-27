#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Downloads one full day of NASDAQ TotalView-ITCH 5.0 (market by order, every
# order event on every NASDAQ-listed and -traded instrument) from NASDAQ's
# public sample-data server, and decompresses it.
#
#   scripts/fetch_nasdaq_day.sh [DATE] [DEST_DIR] [CONNECTIONS]
#   scripts/fetch_nasdaq_day.sh 12302019 /d/market-data/nasdaq 16
#
# The server throttles each connection (~90 KB/s), so the file is fetched as N
# byte ranges in parallel, each resumable, then concatenated. Sizes: ~3.5 GB
# compressed, ~9-12 GB uncompressed, ~300-400 M messages.
set -euo pipefail

DATE="${1:-12302019}"
DEST="${2:-/d/market-data/nasdaq}"
N="${3:-16}"
URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${DATE}.NASDAQ_ITCH50.gz"
GZ="${DEST}/${DATE}.NASDAQ_ITCH50.gz"
BIN="${DEST}/${DATE}.NASDAQ_ITCH50.bin"

mkdir -p "${DEST}/parts"
size=$(curl -sI "$URL" | tr -d '\r' | awk 'tolower($1)=="content-length:" {print $2}')
[[ -n "$size" ]] || { echo "cannot reach $URL" >&2; exit 1; }
echo "downloading ${DATE}: ${size} bytes in ${N} parallel ranges"

chunk=$(( (size + N - 1) / N ))
pids=()
for ((i = 0; i < N; i++)); do
  start=$(( i * chunk ))
  end=$(( start + chunk - 1 )); (( end >= size )) && end=$(( size - 1 ))
  part="${DEST}/parts/part$(printf %02d $i)"
  want=$(( end - start + 1 ))
  (
    # Resume: request only what this part is still missing.
    for attempt in $(seq 1 50); do
      have=$( [[ -f "$part" ]] && stat -c %s "$part" || echo 0 )
      (( have >= want )) && exit 0
      curl -s --retry 5 -r "$(( start + have ))-${end}" "$URL" >> "$part" || sleep 5
    done
    exit 1
  ) &
  pids+=($!)
done
for p in "${pids[@]}"; do wait "$p"; done

cat "${DEST}"/parts/part* > "$GZ"
actual=$(stat -c %s "$GZ")
[[ "$actual" == "$size" ]] || { echo "size mismatch: $actual != $size" >&2; exit 1; }
gzip -t "$GZ"
rm -rf "${DEST}/parts"
echo "decompressing to ${BIN}"
gzip -dc "$GZ" > "$BIN"
ls -la "$DEST"
