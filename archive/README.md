# Archive

Code that is **not part of the current implementation** but is kept for
history, reference and comparison.

| File | What it is | Why it is archived |
|---|---|---|
| `loadbalancer_round_robin.cpp` | The first-generation balancer: a **single-threaded** event loop with one shared `accept()` queue, a shared `fd_to_connection` map and a 4 KiB buffer. | It is the design that `cppbalancer.cpp` replaced. It is kept so the improvement (multi-threaded `SO_REUSEPORT` pool, 64 KiB buffers, per-thread connection maps, half-close handling) stays traceable. It is **not built or benchmarked** by the current test/benchmark tooling. |

## Do not use it in production

`loadbalancer_round_robin.cpp` has several limitations that the current
implementation fixes:

* **Single threaded** — one slow backend blocks every other connection.
* **Shared accept queue** — a thundering herd of workers on one `accept()`.
* **No `TCP_NODELAY`** — ~40 ms Nagle delay on small HTTP writes.
* **No half-close handling** — a `FIN` from either side tears the whole
  connection down, losing an in-flight response body.
* **4 KiB buffers** — oversized responses are silently truncated.
* **Shared `std::map` of connections** — a global mutex on the hot path.

## How it differs from the current implementation

```
loadbalancer_round_robin.cpp        cppbalancer.cpp
---------------------------         --------------------------------
1 thread                            N threads (1 per core), SO_REUSEPORT
shared accept queue                 one listen socket per worker
global map + mutex                  per-thread map, no locks
4 KiB buffers                       64 KiB buffers
no TCP_NODELAY                      TCP_NODELAY on both legs
no half-close support               full half-close state machine
no backpressure                     pause/resume reading per direction
hard-coded backends                 --backends / $CPPBALANCER_BACKENDS
verbose per-connection stdout       --quiet switchable
```

## Related artefacts

* The **pre-backpressure** version of the current balancer (the state of
  `cppbalancer.cpp` before the benchmark suite uncovered the >64 KiB
  data-loss bug) lives in `benchmark/baseline/cppbalancer_prefix.cpp`.
  It is used as the "before" side of the A/B comparison in benchmark
  scenario S11.
* The current implementation and its measurements are described in
  [`../README.md`](../README.md) and [`../benchmark/README.md`](../benchmark/README.md).
