#!/usr/bin/env python3
"""
test_client.py — minimal client used for manual smoke-testing of
cppbalancer.

Connects to the balancer, sends a short message and prints whatever the
backend echoed back through the proxy.

    python3 test_client.py                       # defaults to 127.0.0.1:8181
    python3 test_client.py --host 127.0.0.1 --port 8181
"""

import argparse
import socket
import sys


def main():
    ap = argparse.ArgumentParser(description="Minimal client for cppbalancer")
    ap.add_argument("--host", default="127.0.0.1", help="load balancer address")
    ap.add_argument("--port", type=int, default=8181, help="load balancer port")
    ap.add_argument("--message", default="Hello Load Balancer!", help="message to send")
    args = ap.parse_args()

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((args.host, args.port))
        print(f"Connected to load balancer at {args.host}:{args.port}")

        s.sendall(args.message.encode())
        print(f"Sent: {args.message}")

        data = s.recv(4096)
        print(f"Received: {data.decode(errors='replace')}")


if __name__ == "__main__":
    try:
        main()
    except (ConnectionRefusedError, OSError) as exc:
        print(f"Connection failed: {exc}", file=sys.stderr)
        sys.exit(1)
