#!/usr/bin/env python3
"""
test_server.py — minimal echo backend used for manual smoke-testing of
cppbalancer.

It is *not* used by the benchmark suite (the benchmark uses the much faster
benchmark/backends/http_backend.c so that the balancer, not the backend, is
the component under observation). This script exists for interactive
debugging: start it on each of the balancer's backend ports and watch the
traffic flow.

The balancer's default backend pool is 127.0.0.1:9007, 127.0.0.1:9003 and
127.0.0.1:6789, so run three instances:

    python3 test_server.py --port 9007
    python3 test_server.py --port 9003
    python3 test_server.py --port 6789

Original upstream behaviour (single hard-coded port, greeting then echo) is
preserved; only the port/host/bind address became configurable.
"""

import argparse
import socket
import sys


def main():
    ap = argparse.ArgumentParser(description="Minimal echo backend for cppbalancer")
    ap.add_argument("--port", type=int, default=6789, help="port to listen on (default 6789)")
    ap.add_argument("--host", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    args = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((args.host, args.port))
    s.listen()

    print(f"Backend server listening on port {args.port}", flush=True)

    try:
        while True:
            conn, addr = s.accept()
            print(f"Connection from {addr}", flush=True)
            conn.sendall(f"Hello from backend {args.port}\n".encode())
            try:
                while True:
                    data = conn.recv(1024)
                    if not data:
                        break
                    conn.sendall(f"Backend {args.port} received: ".encode() + data)
            except OSError:
                pass
            conn.close()
    except KeyboardInterrupt:
        print("\nShutting down", file=sys.stderr)
    finally:
        s.close()


if __name__ == "__main__":
    main()
