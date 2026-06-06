#!/usr/bin/env python3
"""Summarize TorrentShopNX local-download stability metrics from a log file."""

from __future__ import annotations

import argparse
import re
from pathlib import Path

PATTERNS = {
    "stalled_transitions": "-> Stalled",
    "stall_levels": "scheduler: stall level=",
    "stall_recovered": "scheduler: stall recovered",
    "slow_peer_isolations": "ISOLATED slow peer",
    "piece_wait_state": "PIECE_WAIT_STATE",
    "no_peer_on_piece": "NO_PEER_ON_PIECE",
    "all_choked": "ALL_CHOKED",
    "no_active_download": "NO_ACTIVE_DOWNLOAD",
    "slow_delivery": "SLOW_DELIVERY",
    "buffer_waits": "installer: buffer wait",
    "partial_read_advance": "collector: partial read advance",
    "starvation_recoveries": "scheduler: STARVATION recovery",
    "stall_recoveries_immediate": "backend/local: STALL_RECOVERY",
    "latency_entries": "LATENCY_MODE enter",
    "latency_exits": "LATENCY_MODE exit",
}

SPEED_RE = re.compile(r"(?:source_speed|install_speed|dl_rate|speed)=(\d+)KB/s")
WAIT_RE = re.compile(r"wait_ms=(\d+)")
SUMMARY_RE = re.compile(r"SESSION_SUMMARY\s+(.*)$")
KV_RE = re.compile(r"([a-zA-Z_]+)=([^\s]+)")


def percentile(values: list[int], pct: float) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    idx = min(len(ordered) - 1, max(0, round((pct / 100.0) * (len(ordered) - 1))))
    return ordered[idx]


def analyze(path: Path) -> dict[str, object]:
    counts = {name: 0 for name in PATTERNS}
    waits: list[int] = []
    speeds: list[int] = []
    summaries: list[dict[str, str]] = []

    for line in path.read_text(errors="replace").splitlines():
        for name, marker in PATTERNS.items():
            if marker in line:
                counts[name] += 1

        if "wait_ms=" in line:
            match = WAIT_RE.search(line)
            if match:
                waits.append(int(match.group(1)))

        for match in SPEED_RE.finditer(line):
            speeds.append(int(match.group(1)))

        summary_match = SUMMARY_RE.search(line)
        if summary_match:
            summaries.append(dict(KV_RE.findall(summary_match.group(1))))

    return {
        "counts": counts,
        "waits": waits,
        "speeds": speeds,
        "summaries": summaries,
    }


def print_report(path: Path, data: dict[str, object]) -> None:
    counts: dict[str, int] = data["counts"]  # type: ignore[assignment]
    waits: list[int] = data["waits"]  # type: ignore[assignment]
    speeds: list[int] = data["speeds"]  # type: ignore[assignment]
    summaries: list[dict[str, str]] = data["summaries"]  # type: ignore[assignment]

    print(f"log={path}")
    print("counts:")
    for name in sorted(counts):
        print(f"  {name}: {counts[name]}")

    print("wait_ms:")
    print(f"  count: {len(waits)}")
    print(f"  p50: {percentile(waits, 50)}")
    print(f"  p95: {percentile(waits, 95)}")
    print(f"  max: {max(waits) if waits else 0}")

    print("speed_kbps:")
    print(f"  samples: {len(speeds)}")
    print(f"  p50: {percentile(speeds, 50)}")
    print(f"  p95: {percentile(speeds, 95)}")
    print(f"  max: {max(speeds) if speeds else 0}")

    if summaries:
        print("session_summaries:")
        for idx, summary in enumerate(summaries, start=1):
            fields = " ".join(f"{key}={value}" for key, value in sorted(summary.items()))
            print(f"  [{idx}] {fields}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logfile", nargs="?", default="log.txt", type=Path)
    args = parser.parse_args()

    if not args.logfile.exists():
        parser.error(f"log file not found: {args.logfile}")

    print_report(args.logfile, analyze(args.logfile))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
