#!/usr/bin/env python3

"""Compares the scenarios produced by run_potential_tests.sh.

For every access pattern it prints, per scenario, the latency of the FOLLOWER
pages (the ones the module can prefetch), the time of a round and the major
faults, plus the gain against `baseline` (positive = better than baseline).
 
Expected reading:
  * chain / stride : gain in follow_p50 / follow_p99 / avg_round_ms / majflt
                     after the warmup, when the prefetch works.
  * random / seq   : ~0 or slightly negative. They are the controls: there is no
                     pattern to learn (random) or the kernel readahead already
                     handles it (seq). A loss here is the overhead of the module.

Usage: python3 compare_results.py --dir ./datas_collected [--csv out.csv]
"""

import argparse
import csv
import re
import sys
from pathlib import Path

PATTERN_ORDER = ["chain", "stride", "random", "seq"]
METRICS = [
    ("follow_p50_us", "foll p50 us"),
    ("follow_p99_us", "foll p99 us"),
    ("avg_round_ms", "round ms"),
    ("avg_majflt", "majflt/round"),
]
KV = re.compile(r"(\w+)=([^\s]+)")



def read_summary(csv_path):
    """Returns the key=value dict of the '# summary' line, or None."""
    try:
        with open(csv_path, "r", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("# summary"):
                    out = {}
                    for key, value in KV.findall(line):
                        try:
                            out[key] = float(value)
                        except ValueError:
                            out[key] = value
                    return out
    except OSError:
        pass
    return None


def read_vm(vm_path):
    out = {}
    try:
        with open(vm_path, "r", encoding="utf-8") as fh:
            for line in fh:
                parts = line.split()
                if len(parts) == 2:
                    out[parts[0]] = float(parts[1])
    except OSError:
        pass
    return out


def read_proc_final(path):
    """Collector counters of the last /proc dump (live entries, GC, exits)."""
    out = {}
    try:
        with open(path, "r", encoding="utf-8") as fh:
            text = fh.read()
    except OSError:
        return out
    for prefix in ("# segments", "# live", "# gc", "# exit "):
        for line in text.splitlines():
            if line.startswith(prefix):
                for key, value in KV.findall(line):
                    try:
                        out[prefix.strip("# ") + "." + key] = float(value)
                    except ValueError:
                        pass
                break
    return out


def gain(base, value):
    """% improvement over the baseline (positive = lower value)."""
    if base is None or value is None or base == 0:
        return None
    return (base - value) / base * 100.0


def fmt(value, digits=1):
    return "-" if value is None else f"{value:.{digits}f}"


def load(root):
    data = {}
    for scen_dir in sorted(p for p in root.iterdir() if p.is_dir()):
        scen = {}
        for pattern in PATTERN_ORDER:
            summary = read_summary(scen_dir / f"{pattern}.csv")
            if summary is None:
                continue
            summary["vm"] = read_vm(scen_dir / f"{pattern}.vm")
            scen[pattern] = summary
        scen["_proc"] = read_proc_final(scen_dir / "final.proc")
        data[scen_dir.name] = scen
    return data


def print_table(data, rows):
    baseline = data.get("baseline", {})
 
    for pattern in PATTERN_ORDER:
        if not any(pattern in scen for scen in data.values()):
            continue
 
        print(f"\n=== pattern: {pattern} ===")
        header = f"{'scenario':<20}"
        for _, label in METRICS:
            header += f"{label:>14}{'gain%':>8}"
        header += f"{'sys majflt':>12}"
        print(header)
        print("-" * len(header))
 
        base = baseline.get(pattern, {})
        for name, scen in data.items():
            summary = scen.get(pattern)
            if summary is None:
                continue
 
            line = f"{name:<20}"
            record = {"scenario": name, "pattern": pattern}
            for key, _ in METRICS:
                value = summary.get(key)
                g = None if name == "baseline" else gain(base.get(key), value)
                line += f"{fmt(value):>14}{fmt(g):>8}"
                record[key] = value
                record[key + "_gain_pct"] = g
            sys_maj = summary["vm"].get("pgmajfault")
            line += f"{fmt(sys_maj, 0):>12}"
            record["sys_pgmajfault"] = sys_maj
            print(line)
            rows.append(record)


def print_overhead(data):
    """Cost of the module in the patterns where there is nothing to gain."""
    baseline = data.get("baseline", {})
    print("\n=== overhead on the controls (random / seq), vs baseline ===")
    for name, scen in data.items():
        if name == "baseline":
            continue
        parts = []
        for pattern in ("random", "seq"):
            summary = scen.get(pattern)
            base = baseline.get(pattern)
            if not summary or not base:
                continue
            g = gain(base.get("avg_round_ms"), summary.get("avg_round_ms"))
            if g is not None:
                parts.append(f"{pattern}: {-g:+.1f}% round time")
        if parts:
            print(f"{name:<20} " + "   ".join(parts))


def print_collector(data):
    print("\n=== collector state at the end of each scenario ===")
    for name, scen in data.items():
        proc = scen.get("_proc") or {}
        if not proc:
            continue
        print(
            f"{name:<20} segments_loaded={fmt(proc.get('segments.segments_loaded'), 0)} "
            f"live_pairs={fmt(proc.get('live.pairs'), 0)} "
            f"live_segments={fmt(proc.get('live.segments'), 0)} "
            f"gc_runs={fmt(proc.get('gc.runs'), 0)} "
            f"gc_pairs_removed={fmt(proc.get('gc.pairs_removed'), 0)} "
            f"purged_procs={fmt(proc.get('exit.purged_procs'), 0)}"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--dir", default="./datas_collected")
    parser.add_argument("--csv", default=None, help="also write the table as CSV")
    args = parser.parse_args()
 
    root = Path(args.dir)
    if not root.is_dir():
        print(f"directory not found: {root}", file=sys.stderr)
        return 1
 
    data = load(root)
    if not data:
        print("no scenario found", file=sys.stderr)
        return 1
    if "normal" not in data:
        print("[!] no 'normal' scenario: gains cannot be computed")
 
    rows = []
    print_table(data, rows)
    print_overhead(data)
    print_collector(data)
 
    out_csv = Path(args.csv) if args.csv else root / "summary.csv"
    if rows:
        with open(out_csv, "w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        print(f"\nCSV: {out_csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
