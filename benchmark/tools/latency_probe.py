#!/usr/bin/env python3
"""
latency_probe.py — single-connection, sequential request-latency probe.

Purpose: measure the *added latency* of the proxy hop with as little noise
as possible.

  * one connection, kept alive
  * strictly sequential (request N+1 is only sent after response N arrives),
    so there is no queueing and no parallelism -> the number is the true
    per-request service time, not a throughput-derived number
  * warm-up requests are discarded
  * reports the full percentile range (p50/p90/p99/p99.9) plus min/max

Run it against the backend directly and against the balancer; the delta is
the cost of proxying one request.

Usage:
    python3 latency_probe.py --host 127.0.0.1 --port 8181 --samples 2000
"""

import argparse
import json
import socket
import statistics
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8181)
    ap.add_argument("--samples", type=int, default=2000)
    ap.add_argument("--warmup", type=int, default=200)
    ap.add_argument("--out", default="-")
    args = ap.parse_args()

    s = socket.create_connection((args.host, args.port), timeout=10)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    req = b"GET / HTTP/1.1\r\nHost: bench\r\nConnection: keep-alive\r\n\r\n"

    lat = []
    total = args.warmup + args.samples
    for i in range(total):
        t0 = time.perf_counter()
        s.sendall(req)
        # Read until the full response is in. The backend sends a fixed body,
        # so we parse Content-Length.
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                raise RuntimeError("backend closed connection")
            buf += chunk
        head, _, rest = buf.partition(b"\r\n\r\n")
        clen = 0
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                clen = int(line.split(b":")[1].strip())
        while len(rest) < clen:
            chunk = s.recv(65536)
            if not chunk:
                raise RuntimeError("backend closed connection mid-body")
            rest += chunk
        t1 = time.perf_counter()
        if i >= args.warmup:
            lat.append((t1 - t0) * 1000.0)

    s.close()
    lat.sort()

    def pct(p):
        if not lat:
            return 0.0
        k = (len(lat) - 1) * p / 100.0
        f = int(k)
        c = min(f + 1, len(lat) - 1)
        return lat[f] + (lat[c] - lat[f]) * (k - f)

    res = {
        "tool": "latency_probe",
        "host": args.host,
        "port": args.port,
        "samples": len(lat),
        "warmup": args.warmup,
        "latency_ms": {
            "min": round(min(lat), 4),
            "mean": round(statistics.fmean(lat), 4),
            "p50": round(pct(50), 4),
            "p90": round(pct(90), 4),
            "p99": round(pct(99), 4),
            "p99_9": round(pct(99.9), 4),
            "max": round(max(lat), 4),
            "stdev": round(statistics.pstdev(lat), 4),
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
