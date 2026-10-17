#!/usr/bin/env python3
"""Summarize comparable Beer D3D12 submission intervals from a runtime log.

This deliberately groups samples by workload counts. It prevents startup,
empty submissions, or a different draw count from being compared as though
it were the same frame workload.
"""

from __future__ import annotations

import argparse
import re
import statistics
from pathlib import Path

PERF_RE = re.compile(
    r"\[D3D12 PERF\].*?interval-ms=(?P<ms>[0-9.]+).*?"
    r"dispatch=(?P<dispatch>\d+) draw=(?P<draw>\d+) "
    r"draw-indexed=(?P<indexed>\d+)"
    r"(?:.*?vk-host-ms=(?P<compute_host>\d+)/(?P<graphics_host>\d+) "
    r"unaccounted-ms=(?P<unaccounted>\d+))?"
)


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    return ordered[int((len(ordered) - 1) * fraction)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--dispatch", type=int, required=True)
    parser.add_argument("--draw", type=int, required=True)
    parser.add_argument("--draw-indexed", type=int, action="append", required=True)
    parser.add_argument("--max-median-ms", type=float)
    parser.add_argument("--minimum-samples", type=int, default=5)
    args = parser.parse_args()

    accepted_indexed = set(args.draw_indexed)
    values: list[float] = []
    compute_host: list[float] = []
    graphics_host: list[float] = []
    unaccounted: list[float] = []
    with args.log.open(errors="replace") as stream:
        for line in stream:
            match = PERF_RE.search(line)
            if not match:
                continue
            if int(match["dispatch"]) != args.dispatch:
                continue
            if int(match["draw"]) != args.draw:
                continue
            if int(match["indexed"]) not in accepted_indexed:
                continue
            values.append(float(match["ms"]))
            if match["compute_host"] is not None:
                compute_host.append(float(match["compute_host"]))
                graphics_host.append(float(match["graphics_host"]))
                unaccounted.append(float(match["unaccounted"]))

    if len(values) < args.minimum_samples:
        print(
            f"insufficient samples: got {len(values)}, "
            f"need {args.minimum_samples}"
        )
        return 2

    median = statistics.median(values)
    print(
        f"samples={len(values)} min={min(values):.3f}ms "
        f"median={median:.3f}ms mean={statistics.fmean(values):.3f}ms "
        f"p90={percentile(values, 0.90):.3f}ms max={max(values):.3f}ms"
    )
    if compute_host:
        print(
            "median breakdown: "
            f"compute={statistics.median(compute_host):.3f}ms "
            f"graphics={statistics.median(graphics_host):.3f}ms "
            f"unaccounted={statistics.median(unaccounted):.3f}ms"
        )
    if args.max_median_ms is not None and median > args.max_median_ms:
        print(
            f"regression: median {median:.3f}ms exceeds "
            f"{args.max_median_ms:.3f}ms"
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
