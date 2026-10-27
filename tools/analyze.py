#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Plots and SUMMARY.md for a results directory written by scripts/run_all.sh.

    python tools/analyze.py results/nasdaq_20191230
"""
from __future__ import annotations

import csv
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.ticker as mticker  # noqa: E402

SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
INK, INK_2, GRID, SURFACE, OTHER = "#0b0b0b", "#52514e", "#e4e3df", "#fcfcfb", "#8a8984"
plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": GRID, "axes.labelcolor": INK_2, "text.color": INK,
    "xtick.color": INK_2, "ytick.color": INK_2, "axes.grid": True, "grid.color": GRID,
    "grid.linewidth": 0.8, "axes.spines.top": False, "axes.spines.right": False,
    "font.size": 10, "axes.titlesize": 12, "axes.titleweight": "bold",
    "axes.titlelocation": "left", "legend.frameon": False, "lines.linewidth": 2,
})


def read_csv(p: Path) -> list[dict]:
    if not p.exists():
        return []
    with p.open(newline="") as f:
        return list(csv.DictReader(f))


def read_json(p: Path) -> dict:
    return json.loads(p.read_text()) if p.exists() else {}


def fmt_ns(v: float, _=None) -> str:
    if v >= 1e6:
        return f"{v / 1e6:g} ms"
    if v >= 1e3:
        return f"{v / 1e3:g} µs"
    return f"{v:g} ns"


def ns(v) -> str:
    v = float(v)
    if v >= 1e6:
        return f"{v / 1e6:.2f} ms"
    if v >= 1e4:
        return f"{v / 1e3:.1f} µs"
    if v >= 1e3:
        return f"{v / 1e3:.2f} µs"
    return f"{v:.0f} ns"


def md_table(header, rows) -> str:
    out = ["| " + " | ".join(header) + " |",
           "|" + "|".join("---" if i == 0 else "---:" for i in range(len(header))) + "|"]
    return "\n".join(out + ["| " + " | ".join(map(str, r)) + " |" for r in rows])


def subtitle(machine: dict) -> str:
    return f"{machine.get('cpu', '?')} · {machine.get('os', '?')} · {machine.get('compiler', '?')}"


def titled(ax, title, machine):
    ax.set_title(title, pad=24)
    ax.text(0, 1.02, subtitle(machine), transform=ax.transAxes, fontsize=8, color=INK_2)


def hbar(ax, labels, values, colors, fmt):
    y = list(range(len(labels)))[::-1]
    ax.barh(y, values, color=colors, height=0.6)
    ax.set_yticks(y)
    ax.set_yticklabels(labels)
    ax.grid(axis="y", visible=False)
    for yi, v in zip(y, values):
        ax.annotate(fmt(v), (v, yi), xytext=(6, -3), textcoords="offset points", fontsize=8)
    ax.set_xlim(right=max(values) * 1.18)


# ---- plots ------------------------------------------------------------------------

def plot_message_mix(counts, machine, out: Path):
    rows = sorted(counts, key=lambda r: -int(r["count"]))[:12]
    total = sum(int(r["count"]) for r in counts)
    fig, ax = plt.subplots(figsize=(9, 4.6))
    hbar(ax, [f"{r['name']} ('{r['type']}')" for r in rows],
         [100 * int(r["count"]) / total for r in rows], [SERIES[0]] * len(rows),
         lambda v: f"{v:.1f} %")
    ax.set_xlabel("share of all messages")
    titled(ax, f"Message mix: {total / 1e6:.0f} M ITCH messages", machine)
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)


def plot_apply_latency(rows, machine, out: Path):
    rows = [r for r in rows if r["stage"].startswith("apply_") and int(r["count"]) > 1000]
    labels = [r["stage"].removeprefix("apply_") for r in rows]
    fig, axes = plt.subplots(1, 2, figsize=(11, 4), sharey=True)
    for ax, col, name, color in zip(axes, ["p50_ns", "p99_ns"], ["p50", "p99"], SERIES[:2]):
        hbar(ax, labels, [float(r[col]) for r in rows], [color] * len(rows),
             lambda v: f"{v:.0f} ns")
        ax.set_title(f"{name}", fontsize=11)
        ax.set_xlabel("ns per message (decode + book update + timer)")
    fig.suptitle("Book update cost by message type (single thread, full day)", x=0.01, y=1.04,
                 ha="left", fontweight="bold")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)


def plot_variants(bench, machine, out: Path):
    med = {}
    for b in bench.get("benchmarks", []):
        if b.get("aggregate_name") == "median":
            med[b["run_name"].split("/iterations")[0]] = b["items_per_second"] / 1e6
    if not med:
        return
    order = [k for k in med if k.startswith("decode/")] + sorted(
        (k for k in med if k.startswith("book/")), key=lambda k: -med[k])
    # Gray: decode-only floors; blue: the fastest book; orange: the other books.
    n_decode = sum(k.startswith("decode/") for k in order)
    n_book = len(order) - n_decode
    colors = [OTHER] * n_decode + [SERIES[0]] + [SERIES[1]] * (n_book - 1)
    fig, ax = plt.subplots(figsize=(9, 4.4))
    hbar(ax, order, [med[k] for k in order], colors, lambda v: f"{v:.1f} M/s")
    ax.set_xlabel("messages per second, millions (median of repetitions); gray = decode only")
    titled(ax, "Book build throughput by data structure, real NASDAQ data", machine)
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)


def percentile_curve(hist_rows):
    total = sum(int(r["count"]) for r in hist_rows)
    xs, ys, seen = [], [], 0
    for r in hist_rows:
        seen += int(r["count"])
        q = seen / total
        xs.append(-math.log10(max(1 - q, 1e-7)))
        ys.append(float(r["bucket_hi_ns"]))
    return xs, ys


NINES = [0, 90, 99, 99.9, 99.99, 99.999]


def plot_pipeline(runs, machine, out: Path):
    fig, ax = plt.subplots(figsize=(9, 5))
    i = 0
    for label, d, stage in runs:
        rows = read_csv(d / "hist" / f"{stage}.csv")
        if not rows:
            continue
        xs, ys = percentile_curve(rows)
        ax.plot(xs, ys, color=SERIES[i], label=label)
        i += 1
    ticks = [-math.log10(1 - n / 100) if n else 0 for n in NINES]
    ax.set_xticks(ticks)
    ax.set_xticklabels([f"p{n:g}" if n else "min" for n in NINES])
    ax.set_xlim(0, ticks[-1])
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(fmt_ns))
    ax.set_xlabel("percentile")
    ax.set_ylabel("latency from feed arrival")
    ax.legend(loc="upper left")
    titled(ax, "Pipeline latency, NASDAQ replayed in real time", machine)
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)


# ---- summary ------------------------------------------------------------------------

def main():
    d = Path(sys.argv[1])
    plots = d / "plots"
    plots.mkdir(parents=True, exist_ok=True)
    machine = read_json(d / "inline" / "machine.json") or read_json(d / "stats" / "machine.json")
    counts = read_csv(d / "stats" / "message_counts.csv")
    inline_lat = read_csv(d / "inline" / "latency_summary.csv")
    bench = read_json(d / "bench.json")

    if counts:
        plot_message_mix(counts, machine, plots / "message_mix.png")
    if inline_lat:
        plot_apply_latency(inline_lat, machine, plots / "apply_latency.png")
    if bench:
        plot_variants(bench, machine, plots / "book_variants.png")
    # Prefer the latency layout (every thread on an isolated core) if it was run.
    def pick(name):
        lat = d / f"{name}_latency_layout"
        return lat if lat.exists() else d / name
    plot_pipeline([("closing cross 15:59-16:01, feed -> signal", pick("pipeline_close"), "arrival_to_signal"),
                   ("opening auction 09:29-09:32, feed -> signal", pick("pipeline_open"), "arrival_to_signal"),
                   ("opening auction, feed -> book", pick("pipeline_open"), "arrival_to_book")],
                  machine, plots / "pipeline_latency.png")

    L = [f"# Results: {d.name}", "", "_Generated by `tools/analyze.py`._", ""]
    if machine:
        L += ["## Machine", "", md_table(["", ""], [[k, v] for k, v in machine.items()]), ""]
    for name in ["stats", "inline", "pipeline_max", "pipeline_open", "pipeline_close",
                 "pipeline_open_latency_layout", "pipeline_close_latency_layout"]:
        s = read_json(d / name / "summary.json")
        if not s:
            continue
        L += [f"## `{name}`", "", md_table(["", ""], [[k, v] for k, v in s.items()]), ""]
        lat = read_csv(d / name / "latency_summary.csv")
        if lat:
            L += [md_table(["stage", "count", "p50", "p90", "p99", "p99.9", "p99.99", "max"],
                           [[r["stage"], f"{int(r['count']):,}", ns(r["p50_ns"]), ns(r["p90_ns"]),
                             ns(r["p99_ns"]), ns(r["p999_ns"]), ns(r["p9999_ns"]), ns(r["max_ns"])]
                            for r in lat]), ""]
    noise = read_csv(d / "noise" / "latency_summary.csv")
    if noise:
        L += ["## Platform noise (gaps > 1 µs on the pipeline CPUs)", "",
              md_table(["cpu", "gaps", "p50", "p99", "max"],
                       [[r["stage"], r["count"], ns(r["p50_ns"]), ns(r["p99_ns"]), ns(r["max_ns"])]
                        for r in noise]), ""]
    if bench:
        rows = defaultdict(list)
        for b in bench.get("benchmarks", []):
            if b.get("run_type") == "iteration":
                rows[b["run_name"].split("/iterations")[0]].append(b["items_per_second"] / 1e6)
        L += ["## Throughput by data structure (M msg/s)", "",
              md_table(["benchmark", "median", "min", "max"],
                       [[k, f"{statistics.median(v):.1f}", f"{min(v):.1f}", f"{max(v):.1f}"]
                        for k, v in sorted(rows.items(), key=lambda kv: -statistics.median(kv[1]))]), ""]
    sym = read_csv(d / "inline" / "symbols.csv")
    if sym:
        sym.sort(key=lambda r: -float(r["traded_shares"]) * float(r["vwap"]))
        L += ["## Top instruments by traded value", "",
              md_table(["symbol", "volume", "VWAP", "open cross", "close cross", "top-of-book changes"],
                       [[r["symbol"], f"{int(r['traded_shares']):,}", f"{float(r['vwap']):.4f}",
                         r["open_cross"], r["close_cross"], f"{int(r['top_changes']):,}"]
                        for r in sym[:20]]), ""]
    (d / "SUMMARY.md").write_text("\n".join(L), encoding="utf-8")
    print(f"wrote {d / 'SUMMARY.md'} and {plots}")


if __name__ == "__main__":
    main()
