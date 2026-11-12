# Tests

Small, dependency-free scripts for manually verifying that `cppbalancer`
is proxying traffic correctly. They are for **interactive debugging**, not
for performance measurement — the benchmark suite lives in
[`../benchmark/`](../benchmark/) and uses a much faster C backend.

| File | Role |
|---|---|
| `test_server.py` | Minimal echo backend. Run one instance per backend port of the balancer's pool. |
| `test_client.py` | Minimal client: connects to the balancer, sends a message, prints the echo. |

---

## Quick start

```bash
# 1. build the balancer
g++ -O2 -std=c++17 -pthread -o cppbalancer ../cppbalancer.cpp

# 2. start one echo backend per backend of the default pool
python3 test_server.py --port 9007 &
python3 test_server.py --port 9003 &
python3 test_server.py --port 6789 &

# 3. start the balancer (defaults: port 8181, 4 workers)
./cppbalancer

# 4. talk to it
python3 test_client.py
```

Expected output:

```
Connected to load balancer at 127.0.0.1:8181
Sent: Hello Load Balancer!
Received: Hello from backend 9007
Backend 9007 received: Hello Load Balancer!
```

Because the balancer round-robins over the three backends, the greeting
alternates between `9007`, `9003` and `6789`.

---

## Pointing the balancer at different backends

The balancer's backend pool is configurable at start-up:

```bash
./cppbalancer --backends 10.0.0.5:8080,10.0.0.6:8080 --listen-port 80 --workers 2
```

or through the environment:

```bash
CPPBALANCER_BACKENDS=10.0.0.5:8080,10.0.0.6:8080 CPPBALANCER_WORKERS=2 ./cppbalancer
```

Run `./cppbalancer --help` for the full option list.

---

## Checking round-robin distribution

```bash
for i in $(seq 1 6); do
  python3 test_client.py | grep Received
done
```

Each run is handled by the next backend in the rotation.

---

## Verifying failover

```bash
# kill one backend — the health checker notices within 5 s (default)
kill %2

# the balancer keeps serving from the remaining two backends
python3 test_client.py
```

The health-check interval can be shortened for testing:

```bash
./cppbalancer --health-interval 2
```

---

## Note on correctness for large payloads

`test_server.py` is an echo server and therefore cannot be used to check
large-response handling. For that, use the integrity checker in the
benchmark suite, which is aware of `Content-Length`:

```bash
python3 ../benchmark/tools/verify_proxy.py --host 127.0.0.1 --port 8181 \
        --requests 200 --concurrent 32 --expect-backends 3
```

It asserts that every response body arrives complete and reports the
per-backend request distribution.
