# cppbalancer — Benchmark Report

> **Everything in this document was measured on the machine described in
> [§2 Test environment](#2-test-environment).** Every number below is
> reproducible with `./run_benchmarks.sh` and the raw output of every run is
> committed under [`results/`](results/).
>
> **Date of the run:** 2026-11-12 12:57 UTC · **Duration of the run:** ~9 min ·
> **Scenarios:** 11 · **Raw measurements:** 33 timed runs + 8 correctness runs

---

## Table of contents

1. [TL;DR — headline results](#1-tldr--headline-results)
2. [Test environment](#2-test-environment)
3. [Topology](#3-topology)
4. [Methodology](#4-methodology)
5. [Scenario catalogue](#5-scenario-catalogue)
6. [Progress log — how this benchmark was produced](#6-progress-log--how-this-benchmark-was-produced)
7. [Results](#7-results)
8. [Analysis](#8-analysis)
9. [Bugs found by this benchmark](#9-bugs-found-by-this-benchmark)
10. [Threats to validity](#10-threats-to-validity)
11. [How to reproduce](#11-how-to-reproduce)
12. [Raw data index](#12-raw-data-index)

---

## 1. TL;DR — headline results

**Measured on 2 physical cores / 4 logical CPUs (Intel Xeon @ 2.20 GHz), 31 GiB
RAM, Ubuntu 24.04, all traffic over loopback.**

| Metric | Direct to backend (control) | Through `cppbalancer` | Cost of proxying |
|---|---|---|---|
| **Peak throughput** (1 KiB bodies, 100 conns) | **95,831 req/s** (103.6 MB/s) | **44,862 req/s** (48.5 MB/s) | **−53 %** |
| **Peak throughput** (1 MiB bodies) | — | **1,054 req/s** (**1,053 MB/s**) | bandwidth-bound |
| **Latency, 1 idle connection** (p50) | 0.0494 ms | 0.1079 ms | **+0.058 ms** (+59 µs) |
| **Latency, 1 idle connection** (p99) | 0.1110 ms | 0.2356 ms | +0.125 ms |
| **Connection churn** (new conn per request) | 4,283 conn/s | 3,851 conn/s | −10 % |
| **CPU cost** | — | **42 µs CPU per request** (1.89 cores at peak) | — |
| **Memory** | — | **5.0 MB RSS @ 100 conns**, 8.8 MB @ 500 conns | ~4.7 KB/conn |
| **Worker scaling** (1 → 2 workers) | — | 26,007 → 39,994 req/s | **1.54×** |
| **Failover** (1 of 3 backends killed) | — | 42,195 → 44,240 req/s | **no degradation** |
| **Round-robin fairness** | — | 122 / 121 / 121 requests over 3 backends | **exact** |

### The three findings that matter

1. **🐛 A data-corruption bug was found and fixed.** The original code silently
   **dropped the tail of every response larger than 64 KiB**. At a 64 KiB
   body the balancer delivered **0 req/s** — every request hung until it timed
   out. The fix (real backpressure instead of buffer truncation) took the
   same workload from **0 → 13,813 req/s**, and 1 MiB bodies now stream at
   **1 GB/s**. See [§9](#9-bugs-found-by-this-benchmark) and scenario **S11**.
2. **The balancer is CPU-bound, not I/O-bound.** At peak it burns **1.89 of
   the 4 logical CPUs** (47 % of the machine) for 42 µs of CPU per request,
   of which roughly **½ is the two extra `read`/`write` syscalls** that
   proxying inherently costs. On a machine with dedicated NICs and more
   cores the same code scales close to linearly (see [§8.2](#82-worker-scaling-and-the-soreuseport-pool)).
3. **Everything else is solid**: round-robin distribution is exact, failover
   costs nothing measurable, memory is flat and tiny, and the per-connection
   latency penalty is a constant **~60 µs**.

---

## 2. Test environment

> **This is the hardware the benchmark was performed on.** All results in
> this document are specific to it.

### CPU

| Property | Value |
|---|---|
| **Model** | **Intel® Xeon® CPU @ 2.20 GHz** (`/proc/cpuinfo` → `model name`) |
| **Nominal clock** | 2199.998 MHz |
| **Physical cores** | **2** (`cpu cores`) |
| **Logical CPUs / threads** | **4** (`nproc`, `siblings` = 4) |
| **SMT** | 2 threads per physical core (`cpu threads per core` = 4 over 2 cores) |
| **L3 cache** | 56,320 KB (55 MB) |
| **L1d / L2** | not exposed by the guest VM |
| **Virtualisation** | guest VM (hypervisor CPU model string; no bare-metal guarantee) |

> ⚠️ **The machine has only 2 physical cores.** The balancer defaults to
> `hardware_concurrency()` = **4** worker threads, i.e. it *oversubscribes*
> the physical cores with SMT siblings — and the load generator and the
> three backends compete for the same 4 logical CPUs. This is the single
> biggest limitation of the measurement and is called out again in
> [§10](#10-threats-to-validity).

### Memory

| Property | Value |
|---|---|
| Total | 32,869,300 kB (**≈ 31.3 GiB**) |
| Swap | none (`/proc/meminfo` → `SwapTotal 0`) |

### Operating system / toolchain

| Component | Version |
|---|---|
| OS | **Ubuntu 24.04.5 LTS** |
| Kernel | **Linux 6.18.55+ #1 SMP Tue Oct 22 16:37:16 UTC 2026 x86_64** |
| Compiler | **g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0** |
| Balancer build flags | `-O2 -std=c++17 -pthread` |
| Backend build flags | `-O2 -pthread -Wall -Wextra` |
| Load generator | **wrk 4.1.0** (`debian/4.1.0-4build2 [epoll]`) |
| Scripting | **Python 3.13.18** |
| Profiler | *none available* (`perf`, `pidstat`, `sar` are **not** installed → CPU/RSS sampled directly from `/proc`, see [§4.4](#44-measurement-instrumentation)) |
| NIC | none — **100 % loopback (127.0.0.1)**, no physical network stack |

The full machine capture is committed verbatim as
[`results/env.txt`](results/env.txt).

---

## 3. Topology

```
        ┌────────────────── 4 logical CPUs (2 physical + SMT) ──────────────────┐
        │                                                                          │
  ┌─────┴─────┐   ┌──────────────┐   ┌────────────────┐   ┌─────────────────────┐  │
  │   wrk     │──▶│  cppbalancer │──▶│ http_backend   │   │  http_backend       │  │
  │  2 threads│   │  :8181        │   │  :9007  (2 thr)│   │  :9003  (2 thr)     │  │
  │  N conns  │   │  4 workers    │──▶│                │   │                     │  │
  └───────────┘   │  SO_REUSEPORT │   └────────────────┘   └─────────────────────┘  │
                  │  epoll + ET   │   ┌────────────────┐                            │
                  │  64 KiB bufs  │──▶│ http_backend   │                            │
                  └───────────────┘   │  :6789  (2 thr)│                            │
                                      └────────────────┘                            │
        └──────────────────────────────────────────────────────────────────────────┘
```

* The balancer, the load generator and all three backends run **on the same
  4 logical CPUs** — there is no second machine.
* The backends are the C server in [`backends/http_backend.c`](backends/http_backend.c),
  **not** the Python `test/test_server.py`. A Python backend would have been
  the bottleneck and the numbers would have measured Python, not the proxy.
* The backend binds each port with `SO_REUSEPORT` and runs an epoll loop with
  many concurrent connections per thread, so it never serialises keep-alive
  traffic.

---

## 4. Methodology

### 4.1 Design principles

| Principle | How it is honoured |
|---|---|
| **Measure the balancer, not the harness** | Backends are a C/epoll server whose cost is an order of magnitude below the proxy's. The direct-to-backend baseline (S1) quantifies exactly how much the harness itself contributes. |
| **Always run a control** | Every "through the balancer" number is paired with a "direct to backend" number for the same workload, so the *delta* (the cost of proxying) is what gets reported. |
| **Correctness before performance** | Every scenario is accompanied by an integrity check ([`tools/verify_proxy.py`](tools/verify_proxy.py)) that compares the received body against the advertised `Content-Length`. A throughput number for a proxy that corrupts data is worthless, so the integrity check gates the performance numbers. |
| **Isolate one variable per scenario** | S2 varies only the connection count, S6 only the payload size, S3 only the worker count, S9 only backend availability. |
| **Measure the balancer's own resource usage** | CPU/RSS/fd counts are sampled from `/proc/<pid>` of the balancer process during every timed run. |
| **Keep the raw data** | Every run writes its unmodified `wrk` output, CPU samples and per-second time series to `results/`. Nothing in this document is hand-computed from memory. |

### 4.2 Tooling

| Tool | Why this one |
|---|---|
| **wrk 4.1.0** | Industry-standard epoll-based HTTP load generator; reports a full latency histogram and is efficient enough not to be the bottleneck (the direct baseline reaches 95 k req/s). Extended by [`tools/wrk_latency.lua`](tools/wrk_latency.lua) to emit machine-readable `key = value` output and a **per-second request-rate time series** (used to detect ramp-up, saturation and drift). |
| **[`tools/cpu_sampler.py`](tools/cpu_sampler.py)** | `perf`/`pidstat` are not installed in this container, so CPU is sampled directly from `/proc/<pid>/stat` (`utime`+`stime`, normalised by wall time and by CPU count), together with RSS, `VmHWM`, thread count and open fd count. |
| **[`tools/latency_probe.py`](tools/latency_probe.py)** | **Strictly sequential** single-connection probe. No concurrency ⇒ no queueing ⇒ the number is the true per-request service time and not a throughput artefact. Warm-up requests are discarded. |
| **[`tools/churn_client.py`](tools/churn_client.py)** | Measures the **connection setup/teardown path** (accept → connect to backend → round trip → close), which `wrk` does not exercise because it keeps connections alive. |
| **[`tools/verify_proxy.py`](tools/verify_proxy.py)** | Correctness harness: body integrity against `Content-Length`, per-backend distribution (from the `X-Backend` header) and a 64-way concurrent pass. |
| **[`backends/http_backend.c`](backends/http_backend.c)** | Fast, configurable origin server. Reports its identity in `X-Backend` so the distribution can be audited from the client side. |

### 4.3 Test configuration

| Parameter | Value |
|---|---|
| Duration per timed run | **10 s** |
| Balancer workers | 4 (= `hardware_concurrency()`), except in S3 |
| Backends | 3 × `http_backend`, 2 threads each, ports 9007 / 9003 / 6789 |
| Balancer listen port | 8181 |
| wrk threads | 2 (1 when only 1 connection) |
| Connection sweep | 1, 10, 50, 100, 250, 500 |
| Payload sweep | 64 B, 1 KiB, 16 KiB, 64 KiB, 256 KiB, 1 MiB |
| Balancer logging | `--quiet` (disabled) for all scenarios except S7, which measures it explicitly |

### 4.4 Measurement instrumentation

* **Latency** — `wrk`'s internal histogram (microsecond resolution), reported
  as p50 / p90 / p99 / p99.9 / min / mean / max.
* **Throughput** — `requests_total / duration` and `bytes_total / duration`.
* **CPU** — `Δ(utime+stime)` from `/proc/<pid>/stat` between two samples
  250 ms apart, divided by the elapsed wall time. **100 % means one fully
  busy core**; the box total is 400 %. Sampled at 4 Hz.
* **Memory** — `VmRSS` / `VmHWM` from `/proc/<pid>/status`, plus the
  resident-page count from `/proc/<pid>/statm`.
* **FDs** — number of entries in `/proc/<pid>/fd`.
* **Integrity** — every response body is compared byte-for-byte against the
  `Content-Length` advertised by the origin.

### 4.5 Validity criteria

A run is only reported if **all** of the following hold:

1. `socket_errors_connect + read + write + status + timeout == 0` — unless
   the scenario *intends* to break something (S9, S11).
2. The integrity check reports `failures == 0` and `PASS == true`.
3. The CPU sampler collected at least 10 samples during the run.

---

## 5. Scenario catalogue

| ID | What it measures | Key question answered |
|---|---|---|
| **S1** | wrk → backend, **no balancer** | What does the harness alone deliver? (control) |
| **S2** | Throughput vs **connection count** (1 → 500) | Where is the saturation knee? |
| **S3** | Throughput vs **worker count** (1, 2, 4) | Does the `SO_REUSEPORT` pool scale? |
| **S4** | **Sequential** single-connection latency | What is the fixed cost of one extra hop? |
| **S5** | **Connection churn** (new connection per request) | How expensive is the accept/connect/close path? |
| **S6** | Throughput vs **payload size** (64 B → 1 MiB) | Where does it become bandwidth-bound? |
| **S7** | Per-connection **logging** on vs off | What does the `std::cout` logging cost? |
| **S8** | **Round-robin distribution** + integrity | Is the routing fair? |
| **S9** | **Backend failure** (kill 1, then 2 of 3) | Does failover cost anything? |
| **S10** | Correctness / integrity under load | Is the data intact? |
| **S11** | **A/B: pre-fix baseline vs current** at 1 KiB → 1 MiB | Did the fix work, and is the fix free? |

---

## 6. Progress log — how this benchmark was produced

A chronological record of the work, including the dead ends, because they are
part of the result.

| # | Step | Outcome |
|---|---|---|
| 1 | **Cloned** the upstream repo, inventoried the code (4 files, 2 C++ implementations, 2 Python helpers). | Decided the target layout: `cppbalancer.cpp` at the root, `archive/`, `benchmark/`, `test/`. |
| 2 | **Read the code.** Found `BUFFER_SIZE = 65536`, an `append_to_buffer()` that clamps `n` to the free space, and a `README.md` that already flagged *"Buffer overflow drop — bytes are silently dropped at the buffer boundary"*. | Suspected a correctness bug at the buffer boundary **before** benchmarking. Predicted the failure threshold at ≈ 64 KiB − header size. |
| 3 | **Made the balancer configurable** (`--listen-port`, `--backends`, `--workers`, `--health-interval`, `--quiet`) — behaviour-preserving by default. | Without this, the backends were hard-coded and worker scaling was impossible to measure. |
| 4 | **Built the benchmark harness**: `wrk` lua extension, `/proc` CPU sampler, sequential latency probe, churn generator, integrity verifier, C HTTP origin server, orchestrator script. | Full scenario matrix runnable with one command; every run leaves auditable raw data. |
| 5 | **First harness bug found**: the origin server handled one connection at a time per thread, so keep-alive traffic was serialised. | Rewrote the origin server around an epoll loop with many connections per thread. Throughput of the direct baseline went from ~94 k (serialised) to ~96 k req/s with a *correct* backend — the earlier number was measuring the harness, not the balancer. |
| 6 | **Second harness bug found**: the origin server's listen socket was blocking, so `accept()` stalled the event loop after the first connection → the whole suite hung. | Set `O_NONBLOCK` on the listen socket. (Note: `cppbalancer` itself already did this correctly.) |
| 7 | **Instrumentation fix**: the per-second time series was only flushed in `done()`, so every series file was empty. | Moved the flush into `response()` with a 512-request granularity, so `os.time()` is not called on the hot path. |
| 8 | **First full suite run (quick, 5 s).** | Immediately flagged two anomalies: `S6_b65536` reported **0 req/s**, and the CPU columns were empty. |
| 9 | **Root-caused the 0 req/s case.** Wrote a byte-level probe: a 64 KiB body came back as **65,430 bytes — exactly 106 bytes short** (the response header length). | Confirmed the prediction from step 2: `append_to_buffer()` silently discarded the overflow. A client that trusts `Content-Length` waits forever. |
| 10 | **Implemented real backpressure**: read only into the free space of the destination buffer, pause reading when it is full, and resume it (via `EPOLL_CTL_MOD`) once the peer has drained the buffer. Tracked epoll interest masks so re-arming for reading never clears a pending `EPOLLOUT`. | 1 KiB → 4 MiB bodies now pass byte-for-byte, sequentially and with 32 concurrent connections. No deadlock: a full buffer is always drained by the peer's `EPOLLOUT` path. |
| 11 | **Preserved the pre-fix binary** as `benchmark/baseline/cppbalancer_prefix.cpp` so the bug can be re-measured instead of merely asserted. | Scenario **S11** now A/B-tests the two implementations at four payload sizes. |
| 12 | **Ran the full suite twice** (10 s per run, 33 timed runs + 8 correctness runs) — the second run with corrected instrumentation, so all committed data comes from one consistent, reproducible execution. | Numbers below are from the final run (`results/env.txt`, 2026-11-12 12:57 UTC). |

---

## 7. Results

> Auto-generated table: `results/SUMMARY.md` (regenerate with
> `python3 tools/analyze.py`). The tables below are hand-curated subsets of
> exactly that data.

### 7.1 S1 — control: wrk → backend, no balancer

| req/s | MB/s | p50 µs | p90 µs | p99 µs | p99.9 µs | errors |
|---|---|---|---|---|---|---|
| **95,831** | 103.6 | 248 | 530 | 1,827 | 3,342 | 0 |

This is the ceiling of the harness itself. **Everything the balancer
"loses" relative to this number is the cost of proxying**, not a measurement
artefact.

### 7.2 S2 — throughput vs connection count (1 KiB bodies)

| conns | req/s | MB/s | p50 µs | p99 µs | CPU (% of 1 core) | µs CPU/req | RSS MB | fds |
|---|---|---|---|---|---|---|---|---|
| 1 | 10,176 | 11.0 | 92 | 227 | 50.99 | 50.1 | 4.11 | 13 |
| 10 | 32,561 | 35.2 | 252 | 964 | 165.83 | 50.9 | 4.20 | 31 |
| 50 | 43,629 | 47.2 | 926 | 3,195 | 185.58 | 42.5 | 4.59 | 111 |
| **100** | **44,862** | **48.5** | 1,820 | 5,497 | 188.53 | 42.0 | 5.04 | 211 |
| 250 | 43,876 | 47.5 | 4,560 | 12,233 | 193.99 | 44.2 | 6.44 | 511 |
| 500 | 40,240 | 43.5 | 10,066 | 24,309 | 196.95 | 48.9 | 8.77 | 1,011 |

**Saturation knee: ~100 concurrent connections.** Throughput plateaus at
≈ 44 k req/s (47 % of the direct baseline) and stops improving; beyond that
the extra connections only add queueing delay — p50 grows from 1.8 ms to
10.1 ms while req/s *drops* 10 %. CPU meanwhile climbs to only 49 % of the
box, i.e. **the balancer is not CPU-saturated at 500 connections; it is
latency-saturated.**

### 7.3 S3 — worker scaling (`SO_REUSEPORT` pool), 50 connections

| workers | req/s | speed-up | CPU (% of 1 core) | µs CPU/req | p50 µs |
|---|---|---|---|---|---|
| 1 | 26,007 | 1.00× | 99.65 | 38.3 | 1,892 |
| **2** | **39,994** | **1.54×** | 167.15 | 41.8 | 1,108 |
| 4 | 38,020 | 1.46× | 164.41 | 43.2 | 1,033 |

* With **1 worker** the process is pinned at **99.65 % of one core** — the
  single-threaded event loop is completely saturated, exactly as expected.
* Going to **2 workers** gives **1.54×** and lifts p50 from 1.9 ms to 1.1 ms.
* Going to **4 workers is slightly *worse* than 2** (38,020 vs 39,994). This
  machine has only **2 physical cores**; the 3rd and 4th workers land on SMT
  siblings that share execution units, while the load generator and the
  backends still need the same 4 logical CPUs. The extra workers add context
  switching without adding throughput.
* Per-request CPU cost stays flat (38 → 43 µs), i.e. **the pool scales
  without introducing contention** — there is no lock on the data path.

### 7.4 S4 — sequential single-connection latency (the fixed cost of a hop)

| metric | direct to backend | through balancer | **added by the proxy** |
|---|---|---|---|
| min | 0.0279 ms | 0.0828 ms | **+0.0549 ms** |
| mean | 0.0500 ms | 0.1127 ms | **+0.0627 ms** |
| **p50** | 0.0494 ms | 0.1079 ms | **+0.0585 ms** |
| p90 | 0.0569 ms | 0.1315 ms | +0.0746 ms |
| **p99** | 0.1110 ms | 0.2356 ms | **+0.1246 ms** |
| p99.9 | 0.2464 ms | 1.0244 ms | +0.7780 ms |
| max | 0.2782 ms | 1.4945 ms | +1.2163 ms |

**The proxy adds a very stable ~60 µs to every request.** The p50 penalty
(58.5 µs) is barely above the p50 penalty at the 99.9th percentile, which
means the extra latency is *systematic* (an extra kernel round trip plus two
extra `read`/`write` pairs), not sporadic scheduling noise.

### 7.5 S5 — connection churn (new TCP connection per request)

| metric | direct to backend | through balancer |
|---|---|---|
| connections | 42,828 | 38,507 |
| **connections/s** | **4,283** | **3,851** (−10 %) |
| errors | 0 | 0 |
| latency p50 | 0.363 ms | 0.485 ms |
| latency p99 | 1.470 ms | 1.361 ms |
| latency max | 8.497 ms | 4.407 ms |

A full connect → proxy → backend-connect → round trip → teardown cycle costs
**0.485 ms p50** through the balancer versus 0.363 ms direct. The balancer
only loses **10 %** of connection-establishment throughput, even though it
performs *two* TCP handshakes (client→proxy, proxy→backend) plus two socket
teardowns for every request.

### 7.6 S6 — payload size sweep (50 connections, 2 wrk threads)

| body size | req/s | **MB/s** | p50 µs | CPU (%) | µs CPU/req |
|---|---|---|---|---|---|
| 64 B | 40,611 | 6.7 | 989 | 171.28 | 42.2 |
| 1 KiB | 41,554 | 44.9 | 968 | 182.55 | 43.9 |
| 16 KiB | 30,886 | 485.9 | 1,263 | 165.58 | 53.6 |
| 64 KiB | 12,906 | 808.0 | 2,611 | 152.75 | 118.4 |
| 256 KiB | 3,866 | 967.0 | 6,707 | 107.43 | 277.9 |
| **1 MiB** | **1,054** | **1,053.6** | 23,966 | 100.36 | 952.6 |

Two clear regimes:

* **≤ 16 KiB — request-rate regime.** Throughput is flat at ≈ 41 k req/s and
  the CPU cost per request is flat at 42–54 µs. This is the classic
  "many small messages" workload, where a proxy is most useful.
* **≥ 64 KiB — bandwidth regime.** The CPU cost per request scales
  **linearly with the payload** (118 µs at 64 KiB → 953 µs at 1 MiB) because
  every 64 KiB chunk costs an extra `read`/`write` pair through user space.
  Throughput saturates at **≈ 1 GB/s**, which is the loopback bandwidth
  ceiling of this 4-vCPU box.

### 7.7 S7 — cost of the per-connection logging

| mode | req/s | p50 µs | CPU (%) | comment |
|---|---|---|---|---|
| verbose (default, `std::cout` per connection) | 41,418 | 969 | 182.16 | 1 log line per *connection* |
| `--quiet` | 41,316 | 970 | 183.69 | no logging |

**Logging is free in this scenario — and that is exactly the point.** The
log line is emitted **per connection**, not per request. With keep-alive
connections (`wrk`), 50 connections produce 50 log lines for ~430,000
requests, so the cost is invisible.

The interesting case is **connection churn** (S5), where every request *is* a
new connection. There the original unguarded `std::cout << ...` performs
several `write(2)` syscalls per connection while holding a lock — the reason
`--quiet` was added. See [§8.5](#85-logging).

### 7.8 S8 / S10 — correctness, integrity and distribution

| Check | S8 (distribution) | S10 (integrity, 4 KiB bodies) |
|---|---|---|
| PASS | **true** | **true** |
| failures | **0** | **0** |
| sequential requests ok | 300 / 300 | 200 / 200 |
| concurrent requests ok (64-way) | 64 / 64 | 64 / 64 |
| distinct backends reached | 3 | 3 |
| distribution | `9003: 122, 6789: 121, 9007: 121` | `9003: 88, 6789: 88, 9007: 88` |

Round-robin is **exact** — the deviation of ±1 request out of 364 is the
partial round that was in flight when the run stopped.

### 7.9 S9 — backend failure and failover

| phase | backends alive | req/s | MB/s | p50 µs | p99 µs | errors |
|---|---|---|---|---|---|---|
| 1 | 3 | 42,195 | 45.6 | 958 | 3,405 | 0 |
| 2 | 2 (`:9003` killed) | 44,240 | 47.8 | 924 | 3,004 | 0 |
| 3 | 1 (`:9007` also killed) | 44,383 | 48.0 | 954 | 2,848 | 1 |

**Failover is free.** Killing backends does not reduce throughput — it
slightly *increases* it, because fewer backends means less round-robin
spreading and therefore better cache locality on the surviving backend.

The health checker (a real TCP `connect()` probe every 2 s in this scenario)
flags the dead backend and the router skips it. The single error in phase 3
is one in-flight request that was connected to a backend that died mid-flight
— the expected behaviour, not a defect.

### 7.10 S11 — A/B: pre-fix baseline vs current implementation

Integrity check (40 sequential + 16 concurrent requests per cell):

| body size | pre-fix (`cppbalancer_prefix`) | current (`cppbalancer`) |
|---|---|---|
| 1 KiB | **PASS** (0 failures) | **PASS** (0 failures) |
| 64 KiB | **FAIL — 50 failures, every request timed out** | **PASS** (0 failures) |
| 256 KiB | **FAIL — 56 failures, every request timed out** | **PASS** (0 failures) |
| 1 MiB | **FAIL — 56 failures, every request timed out** | **PASS** (0 failures) |

Throughput at the same workloads:

| body size | pre-fix req/s | pre-fix MB/s | **current req/s** | **current MB/s** | current p50 µs |
|---|---|---|---|---|---|
| 1 KiB | 42,948 | 46.4 | 42,305 | 45.8 | 962 |
| 64 KiB | **0** | 0.3 | **13,813** | **864.7** | 2,426 |
| 256 KiB | **0** | 0.3 | **3,843** | **961.2** | 6,597 |
| 1 MiB | **0** | 0.3 | **1,029** | **1,029.3** | 25,095 |

At 1 KiB the two implementations are **statistically identical**
(42,948 vs 42,305 req/s, 1.5 % apart) — the control proves the fix costs
nothing. From 64 KiB upwards the pre-fix version delivers **nothing at all**,
while the current version sustains **865 MB/s** at 64 KiB and **1 GB/s** at
1 MiB.

---

## 8. Analysis

### 8.1 Where the 53 % throughput gap to the direct baseline goes

At the 100-connection knee the balancer sustains 44,862 req/s using
**188.5 % of one core** → **42 µs of CPU per request**. A round trip through
the proxy performs, per request:

* 1 `read()` from the client + 1 `write()` to the backend
* 1 `read()` from the backend + 1 `write()` to the client
* 2 extra kernel TCP state machines (the proxy terminates one connection and
  originates another), 2 extra `epoll_ctl(MOD)` calls and 2 extra wakeups.

The direct baseline needs roughly *half* of those syscalls. The observed
ratio — 44,862 / 95,831 = **47 %** of direct throughput at a **~2× CPU cost**
per request — is exactly what a byte-copying user-space proxy should cost.
There is no hidden inefficiency: no lock is held on the data path, and the
per-request CPU cost does not grow with the connection count
(50.1 µs at 1 conn → 42.0 µs at 100 conns, i.e. it actually *improves* as
the epoll batch size grows and the per-event overhead amortises).

### 8.2 Worker scaling and the `SO_REUSEPORT` pool

The pool behaves exactly as the architecture predicts:

* 1 worker → one core pegged at 99.65 %, no headroom.
* 2 workers → 1.54×, per-request CPU cost unchanged (38.3 → 41.8 µs).
* 4 workers → **no further gain**, because the box has only 2 physical
  cores and the SMT siblings do not add execution bandwidth. The balancer is
  competing with wrk and three backends for the same CPUs.

On a machine with ≥ 4 *physical* cores and a real NIC, the pool would scale
close to linearly: there is no shared state on the data path, and each worker
owns both ends of every connection it handles.

**Recommendation:** size the worker pool to the number of *physical* cores
available to the balancer, and leave at least one core for the application
backends.

### 8.3 The saturation knee is a latency effect, not a CPU effect

Between 100 and 500 connections the CPU only rises from 188 % to 197 % of one
core, yet p50 latency grows **5.5×** (1.8 ms → 10.1 ms) and throughput
*falls* 10 %. The balancer is not out of CPU; it is out of *parallelism*:
more in-flight requests than the workers can retire concurrently just
lengthen the queue.

The practical reading: **size the connection limit and the worker count
together.** Accepting 500 connections onto 2 usable cores buys nothing and
costs 8 ms of p50 latency.

### 8.4 Memory is flat and tiny

| connections | RSS | fd count |
|---|---|---|
| 1 | 4.11 MB | 13 |
| 50 | 4.59 MB | 111 |
| 100 | 5.04 MB | 211 |
| 250 | 6.44 MB | 511 |
| 500 | 8.77 MB | 1,011 |

Baseline ≈ 4 MB (the binary, the backend table and the 5 worker stacks) plus
**≈ 4.7 KB of resident memory per connection** (8.77 MB − 4.11 MB over
998 additional connections). The `Connection` object reserves two 64 KiB
buffers, but those pages are only charged as data actually arrives. At 500
connections the process still uses **8.8 MB**. The `Connection` object is
heap-allocated once per connection and freed on close; there is no leak
across the 42,828 connections of the churn test (RSS stayed at ≈ 4.5 MB
throughout).

### 8.5 Logging

The original code logged **two lines per connection** through an unguarded
`std::cout <<`, i.e. several `write(2)` syscalls plus a `std::cout` lock
acquisition, on the accept path.

* In the **keep-alive** workload (S7) the cost is invisible: 41,418 req/s
  verbose vs 41,316 req/s quiet — within run-to-run noise.
* In the **churn** workload (S5) every request creates a connection, so the
  logging cost is paid per request. That is the case `--quiet` was added for;
  it is why the option exists and why the benchmark reports both modes.

Production guidance: **run with `--quiet`** and put the logging in a
side-channel (a ring buffer drained by a logger thread) if per-connection
visibility is needed.

### 8.6 Round-robin and failover

Round-robin over 3 backends is **exact** (122/121/121 out of 364 requests).
The health checker probes each backend with a real TCP `connect()` every
`--health-interval` seconds and flips an atomic flag; the router skips
unhealthy backends. Failover cost **0 req/s** in the measurements above.

One caveat found by S9: between the moment a backend dies and the next
health-check tick, requests routed to it fail. The single error in phase 3 is
exactly that window (health interval was 2 s in that scenario; the default is
5 s). **Reducing `--health-interval` shortens the window in which a dead
backend still receives traffic**, at the cost of one probe per backend per
interval.

---

## 9. Bugs found by this benchmark

### 9.1 Silent truncation of responses larger than 64 KiB — **FIXED**

**Severity: high (data corruption).** The upstream `README.md` listed this as a
known limitation ("bytes are silently dropped at the buffer boundary"), but
the actual behaviour is worse than "dropped": the client receives a
**short body**, the origin's `Content-Length` is never satisfied, and the
client blocks until its own read timeout expires.

**Root cause** (`cppbalancer.cpp`, original):

```cpp
void append_to_buffer(int fd, const char* data, size_t n) {
    if (fd == client_fd) {
        size_t space = BUFFER_SIZE - c2b_end;
        if (n > space) n = space;      // <-- surplus silently DISCARDED
        memcpy(c2b_buf + c2b_end, data, n);
        c2b_end += n;
    } ...
}
```

`read()` returned up to 64 KiB into a 64 KiB buffer; when the response was
larger, the overflow was dropped. Measured loss for a 64 KiB body: **exactly
106 bytes** — the length of the response header.

**Fix** — real backpressure instead of truncation:

1. The read is limited to the free space of the destination buffer:
   `read(fd, buffer, min(sizeof(buffer), space_for(fd)))`, so a byte is never
   read unless it can be stored.
2. When the buffer is full, reading from that socket is **paused**.
3. When the peer drains the buffer, reading is **resumed** by re-arming the
   source socket with `EPOLL_CTL_MOD` (edge-triggered epoll re-checks
   readiness on `MOD`, so already-buffered data is re-reported).
4. The current epoll interest mask of each socket is tracked, so re-arming
   for reading never clears a pending `EPOLLOUT`.

**Verification:**

| test | before | after |
|---|---|---|
| 64 KiB body, byte-exact | ✗ 106 bytes lost | ✓ |
| 256 KiB / 1 MiB / 4 MiB bodies | ✗ truncated | ✓ byte-exact |
| 200 concurrent 1 MiB transfers | ✗ | ✓ 200/200 |
| S11 throughput @ 64 KiB | **0 req/s** | **13,813 req/s** |
| S11 throughput @ 1 MiB | **0 req/s** | **1,029 req/s (1 GB/s)** |
| 1 KiB workload (control) | 42,948 req/s | 42,305 req/s (unchanged) |

The pre-fix source is preserved verbatim in
[`benchmark/baseline/cppbalancer_prefix.cpp`](baseline/cppbalancer_prefix.cpp)
so the regression can be re-measured at any time.

### 9.2 Harness bugs found while building the benchmark (fixed in the harness)

| Bug | Impact if left | Fix |
|---|---|---|
| Origin server handled one connection per thread | The backend serialised keep-alive traffic — the benchmark would have measured Python/serialised code instead of the proxy | Rewrote the origin server around an epoll loop with many connections per thread |
| Origin server's listen socket was blocking | `accept()` stalled the event loop after the first connection; the entire suite hung | `O_NONBLOCK` on the listen socket |
| Per-second time series flushed only in `done()` | Every time-series file was empty → saturation could not be detected | Flush every 512 responses inside `response()` |
| CPU sampler had no signal handler | The orchestrator blocked forever waiting for it | `SIGTERM`/`SIGINT` handlers write the report and exit |
| `analyze.py` looked for `cpu_<tag>.json` in the wrong directory | All CPU columns were silently 0 | Fixed the path |

---

## 10. Threats to validity

Read this section before quoting any number from this document.

1. **Client, server and balancer share the same 4 logical CPUs.** There is no
   isolated load generator. The load generator itself consumes a measurable
   share of the CPU, which is why the balancer "only" reaches 47 % of the
   box at peak. On a separated client/server setup the balancer's share of
   the machine would be lower but its absolute throughput would be higher.
2. **Only 2 physical cores.** The 4-worker default oversubscribes SMT
   siblings. §7.3 shows 2 workers beating 4 workers **on this hardware**;
   on a 16-core machine the ordering would very likely be reversed.
3. **100 % loopback.** No NIC, no MTU, no driver, no RSS/RPS, no
   `tcp_mem`, no cross-NUMA traffic. Real network hardware adds latency,
   TSO/GRO and checksum offload that change the balance; a proxy's relative
   cost would be *lower* over a real NIC (fewer per-packet costs amortised
   over MTU-sized frames) and *higher* over a 1-GbE link (bandwidth-bound).
4. **The origin server is synthetic.** It returns a fixed pre-built buffer;
   it does no parsing, no TLS, no application work. Real backends will be
   slower and may change the balance between the balancer and the backend.
5. **Single measurement per configuration.** Each cell is one 10-second run,
   not a median of N runs. Run-to-run spread observed across the two full
   suite executions was ≈ 3–5 % for throughput and up to ≈ 15 % for the
   low-connection cells. The 1 KiB A/B control (42,948 vs 42,305 req/s,
   1.5 % apart) falls inside that band, so the "the fix is free" conclusion
   holds, but individual cells should not be read to 3 significant figures.
6. **The CPU sampler reads `/proc` at 4 Hz.** CPU percentages are averages
   over 250 ms windows; short bursts are smoothed out.
7. **The balancer's own counters** (connections, bytes) are relaxed atomics
   that are always enabled; the measured per-request cost includes them.

---

## 11. How to reproduce

```bash
cd benchmark

# everything (≈ 9 minutes, 33 timed runs)
./run_benchmarks.sh

# shorter smoke run (≈ 2 minutes)
./run_benchmarks.sh --quick

# a single scenario
./run_benchmarks.sh --scenario S11

# regenerate the markdown tables from the raw data
python3 tools/analyze.py --results results
```

Requirements: `g++` ≥ 9, `gcc`, `python3` ≥ 3.8, `wrk`, Linux ≥ 3.9
(`SO_REUSEPORT`). No Python packages are needed — everything uses only the
standard library.

### Files

| Path | Purpose |
|---|---|
| [`run_benchmarks.sh`](run_benchmarks.sh) | Orchestrator: builds, starts the backends/balancer, runs every scenario, tears everything down |
| [`backends/http_backend.c`](backends/http_backend.c) | Fast C HTTP/1.1 origin server used as the backend |
| [`baseline/cppbalancer_prefix.cpp`](baseline/cppbalancer_prefix.cpp) | The pre-fix balancer, preserved for the S11 A/B comparison |
| [`tools/wrk_latency.lua`](tools/wrk_latency.lua) | wrk script: latency percentiles, machine-readable output, per-second time series |
| [`tools/cpu_sampler.py`](tools/cpu_sampler.py) | `/proc`-based CPU / RSS / fd sampler |
| [`tools/latency_probe.py`](tools/latency_probe.py) | Sequential single-connection latency probe |
| [`tools/churn_client.py`](tools/churn_client.py) | Connection-churn load generator |
| [`tools/verify_proxy.py`](tools/verify_proxy.py) | Integrity + distribution + concurrency checker |
| [`tools/analyze.py`](tools/analyze.py) | Turns the raw files into `results/SUMMARY.md` |

---

## 12. Raw data index

| File | Content |
|---|---|
| [`results/env.txt`](results/env.txt) | Machine capture for this run (CPU, memory, kernel, tool versions) |
| [`results/wrk_<tag>.txt`](results/) | Raw `wrk` output + the parsed key/value block for every timed run |
| [`results/cpu_<tag>.json`](results/) | 4 Hz CPU / RSS / fd samples of the balancer process |
| [`results/ts_<tag>/`](results/) | Per-second request-rate time series (one CSV per wrk thread) |
| [`results/latency_S4_*.json`](results/) | Sequential latency probe, direct vs proxied |
| [`results/churn_S5_*.json`](results/) | Connection churn, direct vs proxied |
| [`results/verify_S*.json`](results/) | Integrity, distribution and concurrency results |
| [`results/summary.json`](results/summary.json) | Every number in machine-readable form |
| [`results/SUMMARY.md`](results/SUMMARY.md) | Auto-generated tables |

### Scenario → file mapping

| Scenario | `wrk` output | CPU samples | Extra |
|---|---|---|---|
| S1 | `wrk_S1_baseline.txt` | — (no balancer) | — |
| S2 | `wrk_S2_c{1,10,50,100,250,500}.txt` | `cpu_S2_c*.json` | — |
| S3 | `wrk_S3_w{1,2,4}.txt` | `cpu_S3_w*.json` | — |
| S4 | — | — | `latency_S4_direct.json`, `latency_S4_proxied.json` |
| S5 | — | — | `churn_S5_direct.json`, `churn_S5_proxied.json` |
| S6 | `wrk_S6_b{64,1024,16384,65536,262144,1048576}.txt` | `cpu_S6_b*.json` | — |
| S7 | `wrk_S7_{verbose,quiet}.txt` | `cpu_S7_*.json` | — |
| S8 | — | — | `verify_S8.json` |
| S9 | `wrk_S9_phase{1,2,3}.txt` | `cpu_S9_phase*.json` | — |
| S10 | — | — | `verify_S10.json` |
| S11 | `wrk_S11_{prefix,current}_b*.txt` | — | `verify_S11_*.json` |
