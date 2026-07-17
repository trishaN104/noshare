# SPDX-License-Identifier: MIT
#
# Dependency-free SVG chart generator for the benchmark CSVs. Reads
# docs/results/{scaling,latency_cdf}.csv and writes matching .svg files that
# render directly in GitHub markdown. Uses only the Python standard library so
# there is nothing to install and nothing to break.
#
# Usage: python tools/plot/plot.py [results_dir]

import csv
import math
import os
import sys

W, H = 720, 400
ML, MR, MT, MB = 70, 30, 40, 55  # margins
PW, PH = W - ML - MR, H - MT - MB

AXIS = "#444"
GRID = "#e6e6e6"
TEXT = "#222"
SERIES = ["#1f77b4", "#d62728", "#2ca02c"]


def _x(frac):
    return ML + frac * PW


def _y(frac):
    return MT + (1.0 - frac) * PH


def _header(title, xlabel, ylabel):
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
        f'viewBox="0 0 {W} {H}" font-family="Segoe UI, Arial, sans-serif">',
        f'<rect width="{W}" height="{H}" fill="white"/>',
        f'<text x="{W/2}" y="24" text-anchor="middle" font-size="16" '
        f'fill="{TEXT}">{title}</text>',
        f'<line x1="{ML}" y1="{MT}" x2="{ML}" y2="{MT+PH}" stroke="{AXIS}"/>',
        f'<line x1="{ML}" y1="{MT+PH}" x2="{ML+PW}" y2="{MT+PH}" stroke="{AXIS}"/>',
        f'<text x="{ML+PW/2}" y="{H-12}" text-anchor="middle" font-size="12" '
        f'fill="{TEXT}">{xlabel}</text>',
        f'<text x="18" y="{MT+PH/2}" text-anchor="middle" font-size="12" '
        f'fill="{TEXT}" transform="rotate(-90 18 {MT+PH/2})">{ylabel}</text>',
    ]


def _ygrid(ymax):
    out = []
    for i in range(6):
        frac = i / 5.0
        yy = _y(frac)
        out.append(f'<line x1="{ML}" y1="{yy}" x2="{ML+PW}" y2="{yy}" '
                   f'stroke="{GRID}"/>')
        out.append(f'<text x="{ML-8}" y="{yy+4}" text-anchor="end" '
                   f'font-size="11" fill="{TEXT}">{ymax*frac:.0f}</text>')
    return out


def _legend(items):
    out = []
    x = ML + 10
    y = MT + 14
    for label, color in items:
        out.append(f'<rect x="{x}" y="{y-9}" width="12" height="12" '
                   f'fill="{color}"/>')
        out.append(f'<text x="{x+18}" y="{y+1}" font-size="12" '
                   f'fill="{TEXT}">{label}</text>')
        x += 30 + 8 * len(label)
    return out


def plot_scaling(path, out):
    rows = list(csv.DictReader(open(path)))
    if not rows:
        return
    threads = [int(r["threads"]) for r in rows]
    lf = [float(r["lockfree_mps"]) / 1e6 for r in rows]
    mx = [float(r["mutex_mps"]) / 1e6 for r in rows]
    ymax = max(max(lf), max(mx)) * 1.1
    n = len(rows)

    s = _header("Throughput scaling: lock-free vs mutex",
                "producer/consumer pairs", "throughput (M msgs/s)")
    s += _ygrid(ymax)

    def bars(vals, color, offset, wfrac):
        out = []
        slot = PW / n
        bw = slot * wfrac
        for i, v in enumerate(vals):
            cx = ML + slot * (i + 0.5) + offset
            h = (v / ymax) * PH
            out.append(f'<rect x="{cx-bw/2}" y="{MT+PH-h}" width="{bw}" '
                       f'height="{h}" fill="{color}"/>')
        return out

    s += bars(lf, SERIES[0], -PW / n * 0.18, 0.30)
    s += bars(mx, SERIES[1], +PW / n * 0.18, 0.30)

    slot = PW / n
    for i, t in enumerate(threads):
        cx = ML + slot * (i + 0.5)
        s.append(f'<text x="{cx}" y="{MT+PH+18}" text-anchor="middle" '
                 f'font-size="11" fill="{TEXT}">{t}x{t}</text>')

    s += _legend([("lock-free MPMC", SERIES[0]), ("mutex + queue", SERIES[1])])
    s.append("</svg>")
    open(out, "w").write("\n".join(s))
    print("wrote", out)


def plot_latency_cdf(path, out):
    rows = list(csv.DictReader(open(path)))
    if not rows:
        return
    pcts = [float(r["percentile"]) for r in rows]
    ns = [float(r["latency_ns"]) for r in rows]
    ymax = max(ns) * 1.1 if max(ns) > 0 else 1.0

    s = _header("One-way hand-off latency (SPSC, low load)",
                "percentile (nines scale)", "latency (ns)")
    s += _ygrid(ymax)

    # Map percentile p to a "nines" scale so the tail is visible.
    def xfrac(p):
        p = min(p, 99.999)
        v = -math.log10(max(1.0 - p / 100.0, 1e-5))  # p50->0.30 ... p99.999->5
        return v / 5.0

    pts = [f"{_x(xfrac(p)):.1f},{_y(y/ymax):.1f}" for p, y in zip(pcts, ns)]
    s.append(f'<polyline fill="none" stroke="{SERIES[0]}" stroke-width="2" '
             f'points="{" ".join(pts)}"/>')

    for p in (50, 90, 99, 99.9, 99.99):
        s.append(f'<text x="{_x(xfrac(p))}" y="{MT+PH+18}" '
                 f'text-anchor="middle" font-size="11" fill="{TEXT}">'
                 f'p{p}</text>')

    s.append("</svg>")
    open(out, "w").write("\n".join(s))
    print("wrote", out)


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "docs/results"
    scaling = os.path.join(d, "scaling.csv")
    cdf = os.path.join(d, "latency_cdf.csv")
    if os.path.exists(scaling):
        plot_scaling(scaling, os.path.join(d, "scaling.svg"))
    if os.path.exists(cdf):
        plot_latency_cdf(cdf, os.path.join(d, "latency_cdf.svg"))


if __name__ == "__main__":
    main()
