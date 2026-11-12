#!/usr/bin/env python3
"""
analyze.py — turn the raw files produced by run_benchmarks.sh into the
markdown tables that appear in benchmark/README.md.

Reads:
    results/wrk_<tag>.txt        wrk key=value output
    results/cpu_<tag>.json        CPU/RSS samples of the balancer process
    results/ts_<tag>/*.csv        per-second request-rate time series
    results/latency_*.json        sequential latency probe
    results/churn_*.json          connection churn
    results/verify_*.json         correctness / distribution

Writes:
    results/SUMMARY.md            the markdown tables
    results/summary.json          the same data, machine readable

Usage:
    python3 analyze.py [--results DIR] [--out FILE]
"""

import argparse
import glob
import json
import os
import re
from collections import defaultdict

KV = re.compile(r"^([A-Za-z0-9_.]+)\s*=\s*(.*)$")


def parse_wrk(path):
    d = {}
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            m = KV.match(line)
            if m:
                d[m.group(1)] = m.group(2)
    # wrk also prints a native block; keep the numbers we care about
    return d


def f(d, key, default=0.0):
    try:
        return float(d.get(key, default))
    except (TypeError, ValueError):
        return default


def i(d, key, default=0):
    try:
        return int(float(d.get(key, default)))
    except (TypeError, ValueError):
        return default


def load_json(path):
    try:
        with open(path) as fh:
            return json.load(fh)
    except (OSError, json.JSONDecodeError):
        return None


def ts_stats(tag_dir):
    """Aggregate the per-second time series -> (mean, max, min) req/s."""
    buckets = defaultdict(int)
    for path in glob.glob(os.path.join(tag_dir, "ts_*.csv")):
        with open(path) as fh:
            next(fh, None)
            for line in fh:
                parts = line.strip().split(",")
                if len(parts) < 3:
                    continue
                try:
                    buckets[int(parts[0])] += int(parts[2])
                except ValueError:
                    continue
    if not buckets:
        return None
    vals = list(buckets.values())
    return {
        "seconds": len(vals),
        "mean": sum(vals) / len(vals),
        "max": max(vals),
        "min": min(vals),
        "series": vals,
    }


def cpu_summary(tag, results_dir="."):
    d = load_json(os.path.join(results_dir, f"cpu_{tag}.json"))
    if not d:
        return None
    return d.get("summary", {})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default="results")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    R = args.results
    out_file = args.out or os.path.join(R, "SUMMARY.md")

    rows = []
    for path in sorted(glob.glob(os.path.join(R, "wrk_*.txt"))):
        tag = os.path.basename(path)[4:-4]
        d = parse_wrk(path)
        if "requests_per_sec" not in d:
            continue
        cpu = cpu_summary(tag, R) or {}
        ts = ts_stats(os.path.join(R, f"ts_{tag}")) or {}
        rows.append({
            "tag": tag,
            "requests": i(d, "requests_total"),
            "rps": f(d, "requests_per_sec"),
            "mb_s": f(d, "transfer_MB_s"),
            "p50": i(d, "p50"),
            "p90": i(d, "p90"),
            "p99": i(d, "p99"),
            "p999": i(d, "p99_9"),
            "max": i(d, "max"),
            "errs": (i(d, "socket_errors_connect") + i(d, "socket_errors_read")
                     + i(d, "socket_errors_write") + i(d, "socket_errors_status")
                     + i(d, "socket_errors_timeout")),
            "cpu_mean": cpu.get("cpu_pct_mean", 0),
            "cpu_max": cpu.get("cpu_pct_max", 0),
            "cpu_machine": cpu.get("cpu_pct_of_machine_mean", 0),
            "rss_max": cpu.get("rss_mb_max", 0),
            "fds_max": cpu.get("fds_max", 0),
            "threads": cpu.get("threads_max", 0),
            "ts_max": ts.get("max", 0),
            "ts_min": ts.get("min", 0),
        })

    lat = {}
    for name in ("S4_direct", "S4_proxied"):
        lat[name] = load_json(os.path.join(R, f"latency_{name}.json"))

    churn = {}
    for name in ("S5_direct", "S5_proxied"):
        churn[name] = load_json(os.path.join(R, f"churn_{name}.json"))

    ver = {n: load_json(os.path.join(R, f"verify_{n}.json")) for n in ("S8", "S10")}

    L = []
    L.append("# cppbalancer — benchmark summary (auto-generated)\n")
    L.append(f"Generated from raw files in `{R}/` by `analyze.py`.\n")

    L.append("\n## Throughput / latency / CPU\n")
    L.append("| scenario | req/s | MB/s | p50 µs | p90 µs | p99 µs | p99.9 µs | max µs | errors | CPU % (of 1 core) | CPU % of box | µs CPU/req | RSS MB | fds | req/s per sec (min-max) |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        us_cpu = ((r['cpu_mean'] / 100.0) / r['rps'] * 1e6) if (r['cpu_mean'] and r['rps']) else 0
        rng = (f"{r['ts_min']:,.0f}–{r['ts_max']:,.0f}" if r['ts_max'] else "–")
        L.append("| {tag} | {rps:,.0f} | {mb_s:.1f} | {p50} | {p90} | {p99} | {p999} | {max} | {errs} | {cpu_mean} | {cpu_machine} | {us_cpu:.1f} | {rss_max} | {fds_max} | {rng} |".format(us_cpu=us_cpu, rng=rng, **r))

    if lat.get("S4_direct") and lat.get("S4_proxied"):
        d = lat["S4_direct"]["latency_ms"]
        p = lat["S4_proxied"]["latency_ms"]
        L.append("\n## Sequential single-connection latency (S4)\n")
        L.append("| metric | direct to backend | through balancer | added by proxy |")
        L.append("|---|---|---|---|")
        for key, label in (("min", "min"), ("mean", "mean"), ("p50", "p50"),
                           ("p90", "p90"), ("p99", "p99"), ("p99_9", "p99.9"), ("max", "max")):
            L.append(f"| {label} (ms) | {d[key]:.4f} | {p[key]:.4f} | +{p[key]-d[key]:.4f} |")

    if churn.get("S5_direct") and churn.get("S5_proxied"):
        d = churn["S5_direct"]
        p = churn["S5_proxied"]
        L.append("\n## Connection churn (S5)\n")
        L.append("| metric | direct to backend | through balancer |")
        L.append("|---|---|---|")
        L.append(f"| connections | {d['connections']:,} | {p['connections']:,} |")
        L.append(f"| connections/s | {d['conn_per_sec']:,.0f} | {p['conn_per_sec']:,.0f} |")
        L.append(f"| errors | {d['errors']} | {p['errors']} |")
        for key in ("min", "mean", "p50", "p90", "p99", "max"):
            L.append(f"| latency {key} (ms) | {d['latency_ms'][key]:.3f} | {p['latency_ms'][key]:.3f} |")

    for name, d in ver.items():
        if not d:
            continue
        L.append(f"\n## Correctness ({name})\n")
        L.append("| check | value |")
        L.append("|---|---|")
        L.append(f"| PASS | {d['PASS']} |")
        L.append(f"| failures | {d['total_failures']} |")
        L.append(f"| sequential requests ok | {d['sequential_ok']}/{d['sequential_requests']} |")
        L.append(f"| concurrent requests ok | {d['concurrent_ok']}/{d['concurrent_requests']} |")
        L.append(f"| distinct backends | {d['distinct_backends']} |")
        L.append(f"| distribution | {d['backend_distribution']} |")

    with open(out_file, "w") as fh:
        fh.write("\n".join(L) + "\n")

    with open(os.path.join(R, "summary.json"), "w") as fh:
        json.dump({"wrk": rows, "latency": lat, "churn": churn, "verify": ver}, fh, indent=2)

    print("\n".join(L))
    print(f"\n[analyze] wrote {out_file} and {os.path.join(R, 'summary.json')}")


if __name__ == "__main__":
    main()
