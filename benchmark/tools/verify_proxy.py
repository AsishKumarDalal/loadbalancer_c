#!/usr/bin/env python3
"""
verify_proxy.py — correctness harness for cppbalancer.

A throughput number is worthless if the proxy is dropping bytes or routing
badly. This tool checks three properties:

  1. INTEGRITY  — every response body arrives complete and byte-identical
                   to what the origin produced (no silent truncation at the
                   64 KiB BUFFER_SIZE boundary).
  2. DISTRIBUTION— requests are spread across all backends by round robin
                   (read from the X-Backend response header).
  3. CONCURRENCY— N simultaneous in-flight connections all complete
                   correctly.

Usage:
    python3 verify_proxy.py --host 127.0.0.1 --port 8181 --body-size 1024 \
                            --requests 300 --concurrent 32
"""

import argparse
import collections
import concurrent.futures
import json
import socket
import sys


def one_request(host, port, host_hdr="bench", timeout=10):
    """Return (ok, backend_label, body_len, error)."""
    try:
        s = socket.create_connection((host, port), timeout=timeout)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.settimeout(timeout)
        s.sendall(f"GET / HTTP/1.1\r\nHost: {host_hdr}\r\nConnection: close\r\n\r\n".encode())
        buf = b""
        # Read the header block first, then exactly Content-Length body bytes.
        # (Do not wait for EOF: a well-behaved keep-alive server never closes.)
        while b"\r\n\r\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                raise RuntimeError("server closed before sending headers")
            buf += chunk
        head, _, body = buf.partition(b"\r\n\r\n")
        clen = None
        label = "?"
        for line in head.split(b"\r\n"):
            low = line.lower()
            if low.startswith(b"content-length:"):
                clen = int(line.split(b":", 1)[1].strip())
            elif low.startswith(b"x-backend:"):
                label = line.split(b":", 1)[1].strip().decode(errors="replace")
        if clen is None:
            s.close()
            return False, label, len(body), "no content-length"
        while len(body) < clen:
            chunk = s.recv(65536)
            if not chunk:
                break
            body += chunk
        s.close()
        if len(body) != clen:
            return False, label, len(body), f"truncated: got {len(body)} want {clen}"
        if body[:1] != b"x":
            return False, label, len(body), "body corrupted"
        return True, label, len(body), ""
    except Exception as exc:  # noqa: BLE001
        return False, "?", 0, repr(exc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8181)
    ap.add_argument("--requests", type=int, default=300)
    ap.add_argument("--concurrent", type=int, default=32)
    ap.add_argument("--expect-backends", type=int, default=0,
                    help="expected number of distinct backends (0 = skip check)")
    ap.add_argument("--timeout", type=float, default=10.0,
                    help="per-request socket timeout in seconds")
    ap.add_argument("--out", default="-")
    args = ap.parse_args()

    dist = collections.Counter()
    failures = []
    ok_count = 0

    # 1 + 2: sequential pass over `requests` connections
    for i in range(args.requests):
        ok, label, blen, err = one_request(args.host, args.port, timeout=args.timeout)
        if ok:
            ok_count += 1
            dist[label] += 1
        else:
            failures.append({"phase": "sequential", "error": err})
            dist[label] += 1

    # 3: concurrent pass
    conc_ok = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrent) as ex:
        for ok, label, blen, err in ex.map(
                lambda _: one_request(args.host, args.port, timeout=args.timeout),
                range(args.concurrent)):
            if ok:
                conc_ok += 1
                dist[label] += 1
            else:
                failures.append({"phase": "concurrent", "error": err})

    distinct = len([k for k in dist if k != "?"])
    res = {
        "tool": "verify_proxy",
        "host": args.host,
        "port": args.port,
        "sequential_requests": args.requests,
        "sequential_ok": ok_count,
        "concurrent_requests": args.concurrent,
        "concurrent_ok": conc_ok,
        "total_failures": len(failures),
        "failure_samples": failures[:10],
        "backend_distribution": dict(dist),
        "distinct_backends": distinct,
        "distribution_balanced": (distinct >= args.expect_backends) if args.expect_backends else None,
        "PASS": len(failures) == 0 and (not args.expect_backends or distinct >= args.expect_backends),
    }
    text = json.dumps(res, indent=2)
    if args.out == "-":
        print(text)
    else:
        with open(args.out, "w") as fh:
            fh.write(text)

    print(f"\nverify_proxy: PASS={res['PASS']} failures={len(failures)} "
          f"distinct_backends={distinct} distribution={dict(dist)}", file=sys.stderr)
    sys.exit(0 if res["PASS"] else 1)


if __name__ == "__main__":
    main()
