# Architecture of `cppbalancer`

> A detailed walk-through of how the load balancer is built and how a single
> byte travels through it.
>
> **Source of truth:** [`cppbalancer.cpp`](../cppbalancer.cpp) (~600 lines).
> **Measured behaviour:** [`benchmark/README.md`](../benchmark/README.md).

---

## Table of contents

1. [Design goals](#1-design-goals)
2. [The big picture](#2-the-big-picture)
3. [Threads and their responsibilities](#3-threads-and-their-responsibilities)
4. [The `SO_REUSEPORT` worker pool](#4-the-soreuseport-worker-pool)
5. [The epoll event loop](#5-the-epoll-event-loop)
6. [The connection state machine](#6-the-connection-state-machine)
7. [Buffers and the two data directions](#7-buffers-and-the-two-data-directions)
8. [Backpressure — the pause/resume mechanism](#8-backpressure--the-pauseresume-mechanism)
9. [A complete request, end to end](#9-a-complete-request-end-to-end)
10. [Half-close handling](#10-half-close-handling)
11. [Backend selection: round robin](#11-backend-selection-round-robin)
12. [Health checking and failover](#12-health-checking-and-failover)
13. [Memory model and thread safety](#13-memory-model-and-thread-safety)
14. [Configuration](#14-configuration)
15. [Failure modes and how they are handled](#15-failure-modes-and-how-they-are-handled)
16. [Cost model — where the time goes](#16-cost-model--where-the-time-goes)

---

## 1. Design goals

| Goal | How the design achieves it |
|---|---|
| **Never block a thread** | Every socket is `O_NONBLOCK`; one `epoll_wait()` per worker drives everything. |
| **No lock on the data path** | Each worker owns *both* ends of every connection it handles, so a connection is never touched by two threads. There is no mutex anywhere in the request path. |
| **Scale with cores** | One worker per core, each with its own listen socket (`SO_REUSEPORT`), so the kernel distributes accepts without a shared queue. |
| **Never corrupt the stream** | A read is only issued for bytes that fit in the destination buffer; reading pauses instead of truncating. |
| **No dependencies** | Only libc and Linux `epoll`. No Boost, no libevent, no OpenSSL. |

---

## 2. The big picture

```mermaid
graph TB
    subgraph CLIENTS["Clients"]
        C1["client A"]
        C2["client B"]
    end

    subgraph PROC["cppbalancer process"]
        direction TB

        MAIN["main thread<br/>parse args, spawn pool, join"]

        subgraph POOL["Worker pool — N threads, one per core"]
            direction LR
            W0["worker 0<br/>listen_fd + epoll + fd map"]
            W1["worker 1<br/>listen_fd + epoll + fd map"]
            WN["worker N-1<br/>listen_fd + epoll + fd map"]
        end

        HC["health-checker thread<br/>connect() probe per backend<br/>every --health-interval s"]
    end

    subgraph BACKENDS["Backend pool"]
        direction LR
        B1["backend :9007"]
        B2["backend :9003"]
        B3["backend :6789"]
    end

    KERNEL["Linux kernel<br/>SO_REUSEPORT accept hashing<br/>TCP state machines, TCP_NODELAY"]

    CLIENTS -->|"TCP connect"| KERNEL
    KERNEL -->|"accept() by any worker"| POOL
    MAIN -.->|"spawn"| POOL
    MAIN -.->|"spawn"| HC
    HC -.->|"probe"| BACKENDS
    POOL -->|"proxy bytes"| BACKENDS
    POOL <-->|"proxy bytes"| CLIENTS

    style POOL fill:#e3f2fd,stroke:#1565c0,stroke-width:2px
    style HC fill:#fff3e0,stroke:#ef6c00
    style KERNEL fill:#f3e5f5,stroke:#6a1b9a
```

**Three kinds of threads, three different jobs:**

| Thread | Count | Owns | Blocking? |
|---|---|---|---|
| `main` | 1 | argument parsing, spawning, joining | Only on `join()` |
| **worker** | N = `hardware_concurrency()` | its own `listen_fd`, its own `epoll` instance, its own `fd → Connection*` map | Never — `epoll_wait(-1)` sleeps until an event |
| **health-checker** | 1 | the `Backend` health flags | Yes, deliberately — it sleeps 5 s between probe rounds |

---

## 3. Threads and their responsibilities

```mermaid
graph LR
    subgraph W["worker thread"]
        direction TB
        L["listen_fd<br/>SO_REUSEADDR + SO_REUSEPORT<br/>level-triggered EPOLLIN"]
        E["epoll instance<br/>private to this worker"]
        M["fd_to_conn map<br/>thread_local std::map&lt;int, Connection*&gt;"]
        LOOP["event loop<br/>epoll_wait → dispatch"]
        ACC["accept() → select backend<br/>→ connect() → register fds"]
        IO["read / try_write / arm_peer_write"]
        L --> LOOP
        E --> LOOP
        M --> LOOP
        LOOP --> ACC
        LOOP --> IO
    end

    style W fill:#e8f5e9,stroke:#2e7d32
```

The critical property: **`fd_to_conn` is `thread_local`**. A connection accepted
by worker 3 is only ever looked up, read from, written to and closed by
worker 3. That is why there is no mutex and no work-queue hand-off between
threads — the cost of proxying a connection is paid entirely inside one core.

```cpp
void worker_thread() {
    thread_local std::map<int, Connection*> fd_to_conn;   // <-- private to this thread
    ...
}
```

---

## 4. The `SO_REUSEPORT` worker pool

Each worker creates its **own** listening socket on the same port:

```cpp
int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT,  &opt, sizeof(opt));
bind(listen_fd, (sockaddr*)&addr, sizeof(addr));   // same port, same address
listen(listen_fd, 1024);
```

The kernel keeps one accept queue per socket and hashes the 4-tuple of each
incoming connection to pick the socket that will receive it. The result:

```mermaid
graph LR
    subgraph K["Kernel"]
        H["SO_REUSEPORT hash<br/>(client addr:port) → socket"]
        Q0["accept queue 0"]
        Q1["accept queue 1"]
        Q2["accept queue 2"]
        Q3["accept queue 3"]
        H --> Q0 & Q1 & Q2 & Q3
    end

    W0["worker 0"] --- Q0
    W1["worker 1"] --- Q1
    W2["worker 2"] --- Q2
    W3["worker 3"] --- Q3

    style K fill:#f3e5f5,stroke:#6a1b9a
```

**Compared with the classic "one shared listen socket" design:**

| | shared accept queue | `SO_REUSEPORT` (this design) |
|---|---|---|
| `accept()` contention | All workers wake on one queue → thundering herd | Each worker has a private queue |
| Locks | A mutex around `accept()` | **None** |
| Cache behaviour | One queue → cache-line ping-pong | Per-worker queues stay warm |
| Fairness | Unfair (one worker can drain the queue) | Kernel-hashed, even by default |

`SO_REUSEPORT` requires **Linux ≥ 3.9**. The kernel will refuse to `bind()`
a second socket to the same port without it, which is a useful safety net.

> **Measured:** 1 worker → 26,007 req/s (pinned at 99.65 % of one core);
> 2 workers → 39,994 req/s (**1.54×**); 4 workers → no further gain on a
> 2-physical-core machine. See
> [benchmark §7.3](../benchmark/README.md#73-s3--worker-scaling-soreuseport-pool-50-connections).

---

## 5. The epoll event loop

### The one deliberate exception: the listen socket

```mermaid
graph TB
    LFD["listen_fd<br/>EPOLLIN — LEVEL triggered"]
    DSOCK["client_fd / backend_fd<br/>EPOLLIN|EPOLLOUT|EPOLLRDHUP — EDGE triggered"]

    LFD --> L1["Level-triggered on purpose:<br/>a missed edge on a listen socket<br/>can silently drop a connection<br/>under a burst"]
    DSOCK --> D1["Edge-triggered:<br/>the loop only wakes when<br/>new data actually arrives"]

    style LFD fill:#ffebee,stroke:#c62828
    style DSOCK fill:#e8f5e9,stroke:#2e7d32
```

Every data socket is **edge-triggered** (`EPOLLET`). The contract that comes
with ET is: *you must read until `EAGAIN`*. The code honours this — the read
loop only exits on `EAGAIN`, on EOF, on error, or when the buffer fills up.

The listen socket is the exception. With ET, if the worker's `epoll_wait()`
returns and a wake-up is missed, a pending connection can sit in the accept
queue unnoticed. Level-triggered semantics make that impossible: the socket
stays readable and the worker is told again.

### The event dispatch

```mermaid
flowchart TD
    W["epoll_wait(epfd, events, 1024, -1)"] --> NEW{"fd == listen_fd?"}
    NEW -->|"yes"| ACC["accept loop<br/>(level-triggered ⇒ loop until EAGAIN)"]
    ACC --> SEL["select_backend()<br/>round robin over healthy backends"]
    SEL --> OK{"a healthy backend?"}
    OK -->|"no"| CLOSE["close(client_fd)"]
    OK -->|"yes"| CONNECT["socket() + connect() to backend<br/>non-blocking ⇒ may be EINPROGRESS"]
    CONNECT --> REG["register both fds in epoll<br/>client: EPOLLIN|EPOLLET<br/>backend: EPOLLOUT|EPOLLET"]

    NEW -->|"no"| LOOKUP["fd_to_conn.find(fd)"]
    LOOKUP --> HANDSHAKE{"state == CONNECTING<br/>and fd == backend_fd<br/>and EPOLLOUT?"}
    HANDSHAKE -->|"yes"| SOERR["getsockopt(SO_ERROR)"]
    SOERR --> GOOD{"error == 0?"}
    GOOD -->|"no"| DROP["remove_connection()"]
    GOOD -->|"yes"| ACTIVE["state = ACTIVE<br/>arm backend for EPOLLIN"]
    HANDSHAKE -->|"no"| RD["EPOLLIN → read → append_to_buffer"]
    RD --> WR["EPOLLOUT → try_write"]
    WR --> CLEAN["state == CLOSING? → remove_connection()"]
    style NEW fill:#e3f2fd
    style ACC fill:#fff8e1
    style RD fill:#e8f5e9
    style WR fill:#e8f5e9
```

---

## 6. The connection state machine

```mermaid
stateDiagram-v2
    [*] --> CONNECTING : accept() + backend connect() issued

    CONNECTING --> ACTIVE : backend handshake OK<br/>(SO_ERROR == 0)
    CONNECTING --> [*] : backend connect failed<br/>(remove_connection)

    ACTIVE --> ACTIVE : normal bidirectional proxying
    ACTIVE --> HALF_CLOSED : one side sent FIN
    ACTIVE --> CLOSING : hard error on either socket

    HALF_CLOSED --> HALF_CLOSED : draining the remaining direction
    HALF_CLOSED --> CLOSING : both directions drained
    HALF_CLOSED --> [*] : response fully flushed to the client

    CLOSING --> [*] : deferred cleanup at the end<br/>of the event-loop iteration

    note right of CONNECTING
        Client data that arrives during
        the handshake is buffered in
        c2b_buf and flushed as soon
        as the backend is ready.
    end note

    note right of HALF_CLOSED
        A FIN from the peer does NOT
        tear the connection down.
        The other direction is still
        drained first, so an in-flight
        response body is never lost.
    end note
```

### Why the states exist at all

A TCP connection has **two** independent halves. `close()` kills both. The
naive proxy therefore breaks this common case:

```mermaid
sequenceDiagram
    participant C as Client
    participant P as Proxy (naive)
    participant B as Backend

    C->>P: GET /
    P->>B: GET /
    B-->>P: 200 OK + body
    P-->>C: 200 OK + body
    B->>P: FIN (backend closes)
    Note over P: naive proxy calls close() here
    P--xC: connection torn down<br/>before the body is flushed
```

`cppbalancer` tracks the two halves separately with `client_eof` and
`backend_eof`, and only tears the connection down once **both** directions
are finished.

---

## 7. Buffers and the two data directions

Every `Connection` owns two fixed-size buffers and one epoll interest mask
per socket:

```mermaid
graph LR
    CLIENT["client<br/>(the real socket)"]
    BACKEND["backend<br/>(the real socket)"]

    subgraph CONN["Connection object — heap, one per proxied connection"]
        direction LR
        CF["client_fd<br/>interest mask: client_mask"]
        C2B["c2b_buf [64 KiB]<br/>client → backend"]
        BF["backend_fd<br/>interest mask: backend_mask"]
        B2C["b2c_buf [64 KiB]<br/>backend → client"]
        ST["state · eof flags · paused flags"]
    end

    CLIENT -->|"read()"| CF
    CF -->|"write()"| CLIENT
    CF -->|"bytes"| C2B
    C2B -->|"write()"| BF
    BF -->|"read()"| B2C
    B2C -->|"write()"| CF
    BF -->|"write()"| BACKEND
    ST -.-> CF
    ST -.-> BF

    style CONN fill:#e3f2fd,stroke:#1565c0
```

The two directions are handled by **the same code**:

| | client → backend | backend → client |
|---|---|---|
| Buffer | `c2b_buf` | `b2c_buf` |
| Source socket | `client_fd` | `backend_fd` |
| Destination socket | `backend_fd` | `client_fd` |
| Source of truth for "full" | `c2b_end == BUFFER_SIZE` | `b2c_end == BUFFER_SIZE` |

`append_to_buffer(fd, …)` and `try_write(fd)` are deliberately symmetric —
one function handles both directions, which halves the surface area for bugs.
The `fd == client_fd ? … : …` test inside them is the only place the two
directions differ.

### `EPOLLOUT` is armed only when there is something to write

```mermaid
sequenceDiagram
    participant K as Kernel
    participant W as Worker
    participant P as Peer

    K->>W: EPOLLIN on client_fd
    W->>W: read() → 1 200 bytes into c2b_buf
    W->>W: arm_peer_write(client_fd)<br/>→ backend_fd gets EPOLLOUT
    Note over W: backend_fd is NOT writable yet
    K->>W: EPOLLOUT on backend_fd
    W->>W: try_write(backend_fd) → 1 200 bytes out
    W->>W: c2b_buf empty → arm backend_fd<br/>back to EPOLLIN only
```

This keeps the epoll set minimal: a socket is only interested in writability
while there is genuinely buffered data waiting for it.

---

## 8. Backpressure — the pause/resume mechanism

This is the part of the design that guarantees the balancer never corrupts a
stream.

### The problem

The obvious implementation reads greedily into a fixed buffer and clamps what
does not fit:

```cpp
// WRONG — the surplus is silently discarded
size_t space = BUFFER_SIZE - c2b_end;
if (n > space) n = space;
memcpy(c2b_buf + c2b_end, data, n);
```

With `BUFFER_SIZE = 65536`, any response larger than 64 KiB loses its tail.
The client then waits forever for the `Content-Length` it was promised.

### The solution

```mermaid
flowchart TD
    RD["EPOLLIN arrives on fd"] --> SPACE["space = free bytes in the<br/>destination buffer"]
    SPACE --> FULL{"space == 0?"}
    FULL -->|"yes"| PAUSE["paused[fd] = true<br/>STOP reading this socket<br/>break out of the read loop"]
    FULL -->|"no"| READ["read(fd, buf, min(sizeof buf, space))<br/>never more than fits"]
    READ --> N{"n > 0?"}
    N -->|"yes"| APPEND["append_to_buffer()<br/>no clamping needed — it always fits"]
    APPEND --> FULL2{"buffer now full?"}
    FULL2 -->|"yes"| PAUSE
    FULL2 -->|"no"| LOOP2["loop: try to read more"]
    N -->|"no"| EOF["n == 0 → peer sent FIN<br/>handle half-close"]

    PAUSE -.->|"buffer drains"| RESUME
    LOOP2 --> SPACE

    subgraph WRITE["try_write(peer) — the drain path"]
        W1["write() as much as the<br/>peer socket accepts"]
        W1 --> W2{"buffer empty?"}
        W2 -->|"yes"| RESUME["paused[src] = false<br/>resume_read(src)<br/>EPOLL_CTL_MOD(src, mask|EPOLLIN|EPOLLET)"]
        W2 -->|"no"| W3["stay paused;<br/>wait for the next EPOLLOUT"]
    end

    style PAUSE fill:#fff3e0,stroke:#ef6c00
    style RESUME fill:#e8f5e9,stroke:#2e7d32
    style READ fill:#e3f2fd
```

### The three ingredients

1. **Never read more than fits.**
   ```cpp
   size_t space = conn->space_for(fd);
   if (space == 0) { conn->set_paused(fd, true); break; }
   ssize_t n = read(fd, buffer, (sizeof(buffer) < space) ? sizeof(buffer) : space);
   ```
   A byte is never read unless there is a place to put it. Truncation becomes
   structurally impossible.

2. **Pause instead of spin.** When the buffer is full the read loop breaks and
   the socket stops being drained. The bytes stay in the kernel's socket
   receive buffer, where they are safe.

3. **Resume when the peer has drained the buffer.** `try_write()` is the only
   place a buffer shrinks, so it is the natural place to resume the reader:
   ```cpp
   if (*start >= *end) {                       // buffer fully drained
       *start = *end = 0;
       int src = (fd == backend_fd) ? client_fd : backend_fd;
       if (is_paused(src)) {
           set_paused(src, false);
           resume_read(src);                   // EPOLL_CTL_MOD with EPOLLIN re-added
       }
   }
   ```

### Why the interest mask has to be tracked

`resume_read()` re-arms the *source* socket with `EPOLL_CTL_MOD`. If it
naively wrote `EPOLLIN | EPOLLET`, it would **clear a pending `EPOLLOUT`** on
that socket — and the socket might have been holding bytes destined for the
other direction. So each socket's current mask is remembered:

```cpp
uint32_t client_mask  = EPOLLIN | EPOLLET;
uint32_t backend_mask = EPOLLOUT | EPOLLET;

void arm(int fd, uint32_t mask) {
    epoll_event e{}; e.events = mask; e.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e);
    if (fd == client_fd) client_mask = mask; else backend_mask = mask;
}

void resume_read(int fd) {
    uint32_t mask = (fd == client_fd) ? client_mask : backend_mask;
    arm(fd, mask | EPOLLIN | EPOLLET);        // adds EPOLLIN, keeps EPOLLOUT
}
```

### A note on edge-triggered re-arming

`epoll_ctl(EPOLL_CTL_MOD)` re-checks the socket's readiness, so data that is
already sitting in the kernel's receive buffer is reported again. That is
what makes the pause/resume cycle work with `EPOLLET`.

### Deadlock freedom

Could both buffers fill at once and wedge the connection? No:

* `c2b_buf` full → the backend socket is armed for `EPOLLOUT` → the data is
  written to the backend → `c2b_buf` drains → the client is resumed.
* `b2c_buf` full → the client socket is armed for `EPOLLOUT` → the data is
  written to the client → `b2c_buf` drains → the backend is resumed.

Each full buffer always has a peer write pending, and that write is what
unblocks it.

---

## 9. A complete request, end to end

```mermaid
sequenceDiagram
    autonumber
    participant C as Client
    participant K as Kernel
    participant W as Worker (thread-local state)
    participant B as Backend

    C->>K: TCP SYN → :8181
    K->>W: SO_REUSEPORT hashes the connection to this worker
    W->>K: accept() → client_fd
    W->>W: set_nonblocking(client_fd) + set_nodelay(client_fd)
    W->>W: select_backend() → round robin → backend :9007
    W->>K: socket() + connect() to backend :9007 (non-blocking)
    K-->>W: EINPROGRESS ⇒ state = CONNECTING
    W->>K: epoll_ctl(ADD client_fd, EPOLLIN|EPOLLET)
    W->>K: epoll_ctl(ADD backend_fd, EPOLLOUT|EPOLLET)

    C->>K: "GET / HTTP/1.1" + headers
    K->>W: EPOLLIN on client_fd
    W->>W: read() → 1 200 bytes into c2b_buf
    Note over W: state is CONNECTING, so the data is buffered<br/>and NOT forwarded yet

    K->>W: EPOLLOUT on backend_fd (handshake complete)
    W->>W: getsockopt(SO_ERROR) == 0 ⇒ state = ACTIVE
    W->>K: arm backend_fd → EPOLLIN (+EPOLLOUT, because c2b_buf has data)

    W->>K: write(c2b_buf → backend_fd)  — the request is forwarded
    W->>W: c2b_buf empty ⇒ arm backend_fd back to EPOLLIN

    B->>K: "HTTP/1.1 200 OK\r\nContent-Length: 1024\r\n\r\n…"
    K->>W: EPOLLIN on backend_fd
    W->>W: read() → 1 400 bytes into b2c_buf
    W->>K: arm client_fd → EPOLLIN|EPOLLOUT
    K->>W: EPOLLOUT on client_fd
    W->>K: write(b2c_buf → client_fd)  — the response is forwarded
    W->>W: b2c_buf empty ⇒ arm client_fd back to EPOLLIN

    C->>K: FIN
    K->>W: EPOLLIN on client_fd, read() == 0
    W->>W: client_eof = true, c2b_buf empty ⇒ shutdown(backend_fd, SHUT_WR)
    B->>K: FIN (backend sees EOF)
    K->>W: EPOLLIN on backend_fd, read() == 0
    W->>W: backend_eof = true, b2c_buf empty ⇒ shutdown(client_fd, SHUT_WR) then close_connection()
    W->>W: end of the event-loop iteration ⇒ remove_connection()
    W->>W: ~Connection(): epoll_ctl(DEL) ×2, close() ×2, counters updated
```

Notice the **asymmetry during `CONNECTING`**: the client can send data before
the backend handshake has finished. That data is buffered in `c2b_buf` and
flushed the moment the backend becomes ready — the client never waits for a
round trip it did not need to wait for.

---

## 10. Half-close handling

```mermaid
stateDiagram-v2
    direction LR
    [*] --> BothOpen : connection established
    BothOpen --> ClientFIN : client sends FIN<br/>(client_eof = true)
    BothOpen --> BackendFIN : backend sends FIN<br/>(backend_eof = true)

    ClientFIN --> Draining : shutdown(backend_fd, SHUT_WR)<br/>only if c2b_buf is empty
    BackendFIN --> Draining : shutdown(client_fd, SHUT_WR)<br/>only if b2c_buf is empty

    Draining --> [*] : response fully flushed

    note right of ClientFIN
        The write half of the client
        socket is closed, but the
        read half stays open so an
        in-flight response can still
        be delivered.
    end note
```

The rules, in code:

```cpp
if (n == 0) {                       // peer sent FIN
    if (fd == conn->client_fd) {
        conn->client_eof = true;
        if (conn->c2b_end == conn->c2b_start)      // nothing pending → propagate
            shutdown(conn->backend_fd, SHUT_WR);
    } else {
        conn->backend_eof = true;
        if (conn->b2c_end == conn->b2c_start) {
            shutdown(conn->client_fd, SHUT_WR);
            conn->close_connection();
        }
    }
}
```

A `FIN` is only propagated **once the data buffered in that direction has
been flushed**. Propagating it earlier would truncate a response that is
still in flight.

---

## 11. Backend selection: round robin

```mermaid
flowchart LR
    NEW["new client accepted"] --> RR["rr_index.fetch_add(1)<br/>atomic, shared by all workers"]
    RR --> PICK["idx = rr_index % backends.size()"]
    PICK --> HEALTH{"backends[idx].healthy?"}
    HEALTH -->|"yes"| USE["route to this backend"]
    HEALTH -->|"no"| NEXT["try the next index<br/>(up to n attempts)"]
    NEXT --> HEALTH
    HEALTH -->|"all unhealthy"| NONE["close the client fd<br/>count no_healthy_backend"]

    style RR fill:#e3f2fd
    style USE fill:#e8f5e9,stroke:#2e7d32
```

`rr_index` is a single `std::atomic<size_t>` shared by every worker, so the
rotation is global rather than per-thread — the traffic is spread evenly
across the pool instead of each worker favouring its own "next" backend.

Because the increment happens on every attempt, a backend that is
unhealthy is simply skipped over; the rotation still advances.

> **Measured:** 122 / 121 / 121 requests over three backends out of 364 —
> exact. See [benchmark §7.8](../benchmark/README.md#78-s8--s10--correctness-integrity-and-distribution).

---

## 12. Health checking and failover

```mermaid
sequenceDiagram
    participant HC as Health-checker thread
    participant K as Kernel
    participant B as Backend
    participant R as Router (any worker)

    loop every --health-interval seconds (default 5)
        HC->>K: socket() + connect(backend, timeout 2 s)
        alt connect succeeded
            K-->>HC: 0
            HC->>B: healthy = true
        else connect failed / timed out
            HC->>B: healthy = false
        end
        HC->>K: close(probe socket)
    end

    Note over R: select_backend() reads the atomic flag
    R->>B: only healthy backends receive traffic
```

The probe is a **real TCP connect**, not a `ping`. It exercises the same path
a real request would take, so it detects a backend that is up but not
serving, a firewall that has started dropping packets, and a host that has
gone away — all the same way.

`healthy` and `active_connections` are `std::atomic`, so the health checker
can flip them from one thread while the workers read them from another,
with no lock.

**Measured:** killing one of three backends cost **0 req/s**
(42,195 → 44,240). Killing a second one also cost nothing. See
[benchmark §7.9](../benchmark/README.md#79-s9--backend-failure-and-failover).

---

## 13. Memory model and thread safety

| Object | Owner | Access | Synchronisation |
|---|---|---|---|
| `backends` (vector) | `main` | written once before the workers start; read-only afterwards | safe by construction (happens-before via `thread` creation) |
| `Backend::healthy` | health-checker **writes**, workers **read** | `std::atomic<bool>` | lock-free, `seq_cst` |
| `Backend::active_connections` | workers | `std::atomic<int>` | lock-free |
| `rr_index` | **all workers** increment | `std::atomic<size_t>` | lock-free, `relaxed` |
| `g_ctr` (counters) | **all workers** increment | `std::atomic<uint64_t>` | lock-free, `relaxed` |
| `fd_to_conn` map | **one worker only** | `thread_local` | needs none |
| `Connection` object | **one worker only** | heap-allocated, owned by the worker | needs none |
| `g_log_mutex` | all workers (logging only) | `std::mutex` | only on the logging path, skipped entirely with `--quiet` |

**The rule: a connection is never touched by two threads.** A worker accepts
a client, picks a backend, and then owns both file descriptors for the rest
of that connection's life — including closing them. There is no fd-ownership
hand-off, no work queue and no atomic reference counting on the hot path.

The only shared mutable state is a handful of atomics, and none of them sit
in the per-byte path.

---

## 14. Configuration

| Option | Environment variable | Default | Effect |
|---|---|---|---|
| `--listen-port <port>` | `CPPBALANCER_LISTEN_PORT` | `8181` | Port every worker binds with `SO_REUSEPORT`. |
| `--backends <ip:port,…>` | `CPPBALANCER_BACKENDS` | `127.0.0.1:9007,127.0.0.1:9003,127.0.0.1:6789` | The backend pool. A bare `port` means `127.0.0.1`. |
| `--workers <n>` | `CPPBALANCER_WORKERS` | `hardware_concurrency()` | Number of worker threads. `0` or unset ⇒ one per logical CPU. |
| `--health-interval <sec>` | `CPPBALANCER_HEALTH_INTERVAL` | `5` | Seconds between health-probe rounds. |
| `--quiet` | `CPPBALANCER_QUIET=1` | off | Suppress the per-connection log lines. |

Command-line options override environment variables. The defaults reproduce
the original hard-coded behaviour exactly, so the change is non-breaking.

> **Sizing the worker pool:** use the number of *physical* cores available to
> the balancer, and leave headroom for the backends. On the 2-physical-core
> benchmark machine, 2 workers beat 4 — the extra two landed on SMT siblings
> that share execution units with the workers already running.

---

## 15. Failure modes and how they are handled

| Failure | Detection | Reaction |
|---|---|---|
| **Backend is down** | health-checker's `connect()` probe fails | `healthy = false`; the router skips it. Measured cost: **0 req/s**. |
| **Backend dies mid-connection** | `read()` on `backend_fd` returns `0` or `EPIPE` | `backend_eof = true`; the client socket is shut down for writing and the connection is closed. The client sees a truncated response — unavoidable without retrying the request. |
| **Backend handshake fails** | `getsockopt(SO_ERROR)` after `EPOLLOUT` | `remove_connection()`; both sockets are closed. |
| **Client disappears** | `read()` on `client_fd` returns `0` | `client_eof = true`; the backend socket is shut down for writing so the backend can stop working. |
| **All backends unhealthy** | `select_backend()` returns `nullptr` | The client fd is closed and `no_healthy_backend` is incremented. |
| **Write to a peer fails** | `write()` returns an error other than `EAGAIN` | `close_connection()`; the connection is torn down at the end of the event-loop iteration. |
| **Backend is slow** | the destination buffer fills | **Backpressure**: reading is paused, the client is not read from, and the kernel's socket buffer applies the rest of the pressure. No data is lost. |
| **Signal / `EINTR`** | `epoll_wait()` returns `-1` with `errno == EINTR` | The loop simply continues. |

---

## 16. Cost model — where the time goes

For every request the balancer performs, relative to talking to the backend
directly:

```mermaid
flowchart LR
    A["client → kernel"] --> B["proxy reads client_fd"]
    B --> C["proxy writes backend_fd"]
    C --> D["backend → kernel"]
    D --> E["proxy reads backend_fd"]
    E --> F["proxy writes client_fd"]
    F --> G["kernel → client"]

    style B fill:#ffebee,stroke:#c62828
    style C fill:#ffebee,stroke:#c62828
    style E fill:#ffebee,stroke:#c62828
    style F fill:#ffebee,stroke:#c62828
```

**Four extra syscalls and two extra kernel TCP state machines per request**
(red boxes) — that is the entire intrinsic cost of a user-space TCP proxy.

The measured price on the benchmark machine:

| | direct | through the proxy |
|---|---|---|
| Throughput | 95,831 req/s | 44,862 req/s (**47 %**) |
| CPU per request | — | **42 µs** (1.89 cores at peak) |
| Added p50 latency | — | **+0.058 ms** |
| Added p99 latency | — | +0.125 ms |

The ~42 µs is dominated by the four extra syscalls and the two extra wakeups
(epoll returns, context switches). It does **not** grow with the number of
connections — the per-event overhead amortises as the epoll batch size grows,
which is why a single connection is the *least* efficient case
(50.1 µs/request at 1 connection vs 42.0 µs at 100).

For large payloads the cost per request scales linearly with the size,
because every 64 KiB chunk costs an extra `read`/`write` pair through user
space — see [benchmark §7.6](../benchmark/README.md#76-s6--payload-size-sweep-50-connections-2-wrk-threads).

---

## See also

* [`../README.md`](../README.md) — overview, build instructions, benchmark headline
* [`../benchmark/README.md`](../benchmark/README.md) — full benchmark methodology, environment and results
* [`../test/README.md`](../test/README.md) — interactive smoke tests
* [`../archive/README.md`](../archive/README.md) — the superseded single-threaded design and how it differs