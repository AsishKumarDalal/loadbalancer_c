#!/usr/bin/env python3
"""
cpu_sampler.py — sample CPU / memory / fd usage of one or more processes
while a benchmark is running.

`perf`, `pidstat` and `sar` are NOT available in the benchmark container, so
this samples /proc directly:

  * CPU%        from /proc/<pid>/stat fields 14 (utime) and 15 (stime)
                 expressed in clock ticks, normalised by the elapsed wall time
                 and the number of online CPUs (so 100% == one fully busy core)
  * RSS         from /proc/<pid>/statm field 2 (resident pages)
  * VmHWM/VmRSS from /proc/<pid>/status (peak RSS)
  * Threads     from /proc/<pid>/status  (Threads:)
  * FDs         from /proc/<pid>/fd

Usage:
    python3 cpu_sampler.py --pid 1234 --out sample.json --interval 0.25 &
    ... run benchmark ...
    kill %1

Output (JSON):
    {
      "pid": 1234,
      "interval_s": 0.25,
      "n_cpus": 4,
      "samples": [
         {"t": 0.25, "cpu_pct": 88.4, "cpu_pct_of_machine": 22.1,
          "rss_mb": 12.3, "hwm_mb": 20.1, "threads": 5, "fds": 42}
      ],
      "summary": {"cpu_pct_mean": ..., "cpu_pct_max": ..., "cpu_pct_of_machine_mean": ...,
                  "rss_mb_max": ..., "fds_max": ...}
    }
"""

import argparse
import json
import os
import signal
import sys
import time

# On SIGTERM/SIGINT we stop sampling and still write the report.
_STOP = {"flag": False}


def _handle_stop(signum, frame):
    del signum, frame
    _STOP["flag"] = True


signal.signal(signal.SIGTERM, _handle_stop)
signal.signal(signal.SIGINT, _handle_stop)


def read_stat(pid):
    """Return (utime_ticks, stime_ticks, rss_pages, total_pages) or None."""
    try:
        with open(f"/proc/{pid}/stat", "rb") as fh:
            data = fh.read()
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return None

    # The comm field may contain spaces/parens -> split on the LAST ')'.
    close = data.rfind(b")")
    if close < 0:
        return None
    rest = data[close + 2:].split()

    # rest[0] is field 3 (state). Fields 14/15 -> rest[11]/rest[12].
    try:
        utime = int(rest[11])
        stime = int(rest[12])
    except (IndexError, ValueError):
        return None

    rss_pages = 0
    total_pages = 0
    try:
        with open(f"/proc/{pid}/statm", "rb") as fh:
            parts = fh.read().split()
        total_pages = int(parts[0])
        rss_pages = int(parts[1])
    except (FileNotFoundError, ProcessLookupError, PermissionError, IndexError, ValueError):
        pass
    return utime, stime, rss_pages, total_pages


def read_status(pid):
    """Return dict with Threads, VmRSS, VmHWM (kB)."""
    out = {"threads": 0, "vmrss_kb": 0, "vmhwm_kb": 0}
    try:
        with open(f"/proc/{pid}/status", "rb") as fh:
            for line in fh:
                if line.startswith(b"Threads:"):
                    out["threads"] = int(line.split()[1])
                elif line.startswith(b"VmRSS:"):
                    out["vmrss_kb"] = int(line.split()[1])
                elif line.startswith(b"VmHWM:"):
                    out["vmhwm_kb"] = int(line.split()[1])
    except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError):
        pass
    return out


def count_fds(pid):
    try:
        return len(os.listdir(f"/proc/{pid}/fd"))
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--interval", type=float, default=0.25)
    ap.add_argument("--out", default="-")
    ap.add_argument("--label", default="")
    args = ap.parse_args()

    pid = args.pid
    interval = args.interval
    n_cpus = os.cpu_count() or 1
    clk_tck = os.sysconf("SC_CLK_TCK")
    page_size = os.sysconf("SC_PAGE_SIZE")

    samples = []
    prev = read_stat(pid)
    if prev is None:
        print(f"[cpu_sampler] pid {pid} not found", file=sys.stderr)
        sys.exit(1)
    prev_ticks = prev[0] + prev[1]
    prev_time = time.monotonic()
    t0 = prev_time

    while not _STOP["flag"]:
        time.sleep(interval)
        if _STOP["flag"]:
            break
        cur = read_stat(pid)
        if cur is None:
            break
        now = time.monotonic()
        cur_ticks = cur[0] + cur[1]
        dt = now - prev_time
        if dt <= 0:
            continue
        ticks_per_sec = (cur_ticks - prev_ticks) / clk_tck
        cpu_pct = (ticks_per_sec / dt) * 100.0
        st = read_status(pid)
        samples.append({
            "t": round(now - t0, 3),
            "cpu_pct": round(cpu_pct, 2),
            "cpu_pct_of_machine": round(cpu_pct / n_cpus, 2),
            "rss_mb": round(st["vmrss_kb"] / 1024.0, 2),
            "hwm_mb": round(st["vmhwm_kb"] / 1024.0, 2),
            "rss_from_statm_mb": round(cur[2] * page_size / (1024.0 * 1024.0), 2),
            "threads": st["threads"],
            "fds": count_fds(pid),
        })
        prev_ticks = cur_ticks
        prev_time = now

    cpus = [s["cpu_pct"] for s in samples]
    res = {
        "label": args.label,
        "pid": pid,
        "interval_s": interval,
        "n_cpus": n_cpus,
        "samples": samples,
        "summary": {
            "cpu_pct_mean": round(sum(cpus) / len(cpus), 2) if cpus else 0,
            "cpu_pct_max": round(max(cpus), 2) if cpus else 0,
            "cpu_pct_of_machine_mean": round(sum(cpus) / len(cpus) / n_cpus, 2) if cpus else 0,
            "rss_mb_max": max((s["rss_mb"] for s in samples), default=0),
            "hwm_mb_max": max((s["hwm_mb"] for s in samples), default=0),
            "threads_max": max((s["threads"] for s in samples), default=0),
            "fds_max": max((s["fds"] for s in samples), default=0),
            "n_samples": len(samples),
        },
    }
    text = json.dumps(res, indent=2)
    if args.out == "-":
        print(text)
    else:
        with open(args.out, "w") as fh:
            fh.write(text)


if __name__ == "__main__":
    main()
