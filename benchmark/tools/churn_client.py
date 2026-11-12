#!/usr/bin/env python3
"""
churn_client.py — connection-churn load generator for cppbalancer.

wrk keeps connections alive, so it measures *steady-state request rate* on
established connections. A load balancer also has to survive the connection
*setup/teardown* path: accept() -> connect() to a backend -> HTTP round trip
-> close(). This client hammers exactly that path.

Every iteration:
    TCP connect -> send one HTTP request -> read the full response -> close

It is deliberately simple (blocking sockets, one thread per worker) because
the goal is to drive the *balancer's* connection path, not to build a
world-class load generator. Throughput numbers from this tool are therefore
a lower bound; wrk is used for the headline throughput figures.

Usage:
    python3 churn_client.py --host 127.0.0.1 --port 8181 --duration 10 \
                            --threads 4 --keepalive 0
"""

import argparse
import json
import socket
import statistics
import threading
import time


def worker(host, port, deadline, counter, lock, latencies, errors, body_expect):
    while time.monotonic() < deadline:
        try:
            s = socket.create_connection((host, port), timeout=5)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            req = b"GET / HTTP/1.1\r\nHost: bench\r\nConnection: close\r\n\r\n"
            t0 = time.perf_counter()
            s.sendall(req)
            chunks = []
            # Read until Content-Length bytes have arrived (or EOF).
            need = None
            while True:
                b = s.recv(65536)
                if not b:
                    break
                chunks.append(b)
                blob = b"".join(chunks)
                if need is None:
                    if b"\r\n\r\n" in blob:
                        head = blob.split(b"\r\n\r\n", 1)[0]
                        need = 0
                        for line in head.split(b"\r\n"):
                            if line.lower().startswith(b"content-length:"):
                                need = int(line.split(b":", 1)[1].strip())
                        if need == 0:
                            break
                elif len(blob.split(b"\r\n\r\n", 1)[1]) >= need:
                    break
            t1 = time.perf_counter()
            s.close()
            resp = b"".join(chunks)
            if body_expect and not resp.endswith(body_expect):
                with lock:
                    errors["bad_response"] += 1
            with lock:
                counter[0] += 1
                latencies.append((t1 - t0) * 1000.0)
        except Exception:
            with lock:
                errors["exception"] += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8181)
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--warmup", type=float, default=1.0)
    ap.add_argument("--expect-body", default="")
    ap.add_argument("--out", default="-")
    args = ap.parse_args()

    # Warm-up phase (not measured) so that page-cache / JIT-free warmup of the
    # backend and the balancer's per-connection maps do not skew the numbers.
    if args.warmup > 0:
        counter = [0]
        lock = threading.Lock()
        lat, errs = [], {}
        ts = []
        for i in range(args.threads):
            t = threading.Thread(target=worker,
                                 args=(args.host, args.port, time.monotonic() + args.warmup,
                                       counter, lock, lat, errs, b""))
            t.start()
            ts.append(t)
        for t in ts:
            t.join()

    counter = [0]
    lock = threading.Lock()
    latencies = []
    errors = {"exception": 0, "bad_response": 0}

    deadline = time.monotonic() + args.duration
    t_start = time.monotonic()
    threads = []
    for i in range(args.threads):
        t = threading.Thread(target=worker,
                             args=(args.host, args.port, deadline, counter, lock,
                                   latencies, errors,
                                   args.expect_body.encode() if args.expect_body else b""))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()
    elapsed = time.monotonic() - t_start

    latencies.sort()

    def pct(p):
        if not latencies:
            return 0.0
        k = (len(latencies) - 1) * p / 100.0
        f = int(k)
        c = min(f + 1, len(latencies) - 1)
        return latencies[f] + (latencies[c] - latencies[f]) * (k - f)

    res = {
        "tool": "churn_client",
        "host": args.host,
        "port": args.port,
        "threads": args.threads,
        "duration_s": round(elapsed, 3),
        "connections": counter[0],
        "conn_per_sec": round(counter[0] / elapsed, 2) if elapsed else 0,
        "errors": errors,
        "latency_ms": {
            "min": round(min(latencies), 3) if latencies else 0,
            "mean": round(statistics.fmean(latencies), 3) if latencies else 0,
            "p50": round(pct(50), 3),
            "p90": round(pct(90), 3),
            "p99": round(pct(99), 3),
            "max": round(max(latencies), 3) if latencies else 0,
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
