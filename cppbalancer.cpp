// ===========================================================================
//  cppbalancer — non-blocking, multi-threaded TCP/HTTP load balancer
//
//  Architecture (unchanged from the original design):
//    * one worker thread per CPU core, each with its OWN listen socket bound
//      with SO_REUSEPORT  -> the kernel spreads accepts across cores, no
//      shared accept queue, no thundering herd, no mutex on the accept path
//    * every worker owns a private epoll instance and drives BOTH ends of
//      every proxied connection from that single thread (no fd ownership
//      handover between threads -> no locking at all on the data path)
//    * all sockets are O_NONBLOCK; data sockets are edge-triggered (EPOLLET)
//    * TCP_NODELAY on both legs to defeat Nagle's 40ms delay for HTTP
//    * a background thread health-checks every backend with a real TCP
//      connect() and atomically flags unhealthy ones
//
//  What this version adds on top of the original (all behaviour-preserving
//  by default — every new option falls back to the original hard-coded
//  values) is *runtime configurability* + *lightweight counters*, both of
//  which are required to benchmark the balancer reproducibly:
//
//    --listen-port / --backends / --workers / --health-interval / --quiet
//
//  See benchmark/README.md for the methodology and the measured results.
// ===========================================================================

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <iostream>
#include <sstream>
#include <map>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <memory>
#include <mutex>

#define MAX_EVENTS   1024
#define BUFFER_SIZE  65536   // Increased: HTTP responses easily exceed 4 KB
#define LISTEN_PORT  8181

// ---------------------------------------------------------------------------
// Runtime configuration
// ---------------------------------------------------------------------------
struct Config {
    int                      listen_port     = LISTEN_PORT;
    int                      workers         = 0;   // 0 => hardware_concurrency()
    int                      health_interval = 5;   // seconds
    bool                     verbose         = true;
    std::vector<std::string> backend_specs;         // "ip:port"
};

static Config g_cfg;

// ---------------------------------------------------------------------------
// Logging — the original code used an unguarded std::cout << per connection.
// That is several write(2) syscalls per connection and shows up clearly
// under connection-churn load, so it is now switchable with --quiet.
// ---------------------------------------------------------------------------
static std::atomic<bool> g_verbose{true};
static std::mutex        g_log_mutex;

static void log_msg(const char* fmt, ...) {
    if (!g_verbose.load(std::memory_order_relaxed)) return;
    va_list ap;
    va_start(ap, fmt);
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        vfprintf(stdout, fmt, ap);
    }
    va_end(ap);
}

// ---------------------------------------------------------------------------
// Lightweight counters (relaxed atomics — negligible cost, huge diagnostic
// value while benchmarking: bytes proxied, connections accepted, errors).
// ---------------------------------------------------------------------------
struct Counters {
    std::atomic<uint64_t> connections_accepted{0};
    std::atomic<uint64_t> connections_closed{0};
    std::atomic<uint64_t> backend_connect_fail{0};
    std::atomic<uint64_t> no_healthy_backend{0};
    std::atomic<uint64_t> bytes_c2b{0};
    std::atomic<uint64_t> bytes_b2c{0};
};
static Counters g_ctr;

// ---------------------------------------------------------------------------
// Backend
// ---------------------------------------------------------------------------
struct Backend {
    std::string ip;
    int port;
    std::atomic<bool> healthy{true};
    std::atomic<int>  active_connections{0};
    Backend(std::string ip_, int port_) : ip(ip_), port(port_) {}
};

std::vector<std::unique_ptr<Backend>> backends;
std::atomic<size_t> rr_index{0};

// Default pool = the original hard-coded one.
void init_backends() {
    if (g_cfg.backend_specs.empty()) {
        g_cfg.backend_specs = {"127.0.0.1:9007", "127.0.0.1:9003", "127.0.0.1:6789"};
    }
    for (const auto& spec : g_cfg.backend_specs) {
        auto pos = spec.rfind(':');
        std::string ip = (pos == std::string::npos) ? "127.0.0.1" : spec.substr(0, pos);
        int port = (pos == std::string::npos) ? std::atoi(spec.c_str())
                                              : std::atoi(spec.c_str() + pos + 1);
        backends.emplace_back(std::make_unique<Backend>(ip, port));
    }
}

Backend* select_backend() {
    size_t n = backends.size();
    if (n == 0) return nullptr;
    for (size_t i = 0; i < n; ++i) {
        size_t idx = rr_index.fetch_add(1, std::memory_order_relaxed) % n;
        if (backends[idx]->healthy) return backends[idx].get();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------
void set_nonblocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

// Disable Nagle — critical for HTTP: without this the kernel holds small
// writes (e.g. a lone status line) until the ACK arrives, adding ~40 ms
// latency on loopback and making browsers think the server is unresponsive.
void set_nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// ---------------------------------------------------------------------------
// Connection state machine
//
//  CONNECTING  – backend TCP handshake in progress; client data is buffered
//  ACTIVE      – both sides open; normal bidirectional proxy
//  HALF_CLOSED – one side sent FIN; keep draining the other direction
//  CLOSING     – tear everything down
// ---------------------------------------------------------------------------
enum class ConnState { CONNECTING, ACTIVE, HALF_CLOSED, CLOSING };

struct Connection {
    int        client_fd;
    int        backend_fd;
    ConnState  state;
    int        epfd;
    Backend*   backend;

    // c2b: bytes read from client,  waiting to be written to backend
    // b2c: bytes read from backend, waiting to be written to client
    char   c2b_buf[BUFFER_SIZE];
    size_t c2b_start = 0, c2b_end = 0;
    char   b2c_buf[BUFFER_SIZE];
    size_t b2c_start = 0, b2c_end = 0;

    // Half-close tracking: which side has sent FIN already
    bool client_eof  = false;
    bool backend_eof = false;

    // ---- Backpressure -----------------------------------------------------
    // Each direction owns one fixed-size buffer:
    //     c2b_buf : client -> backend   (source = client_fd)
    //     b2c_buf : backend -> client   (source = backend_fd)
    // When the buffer for a direction is full we must STOP reading from that
    // direction's source socket (otherwise bytes are silently discarded at
    // the buffer boundary) and resume reading once the peer has drained the
    // buffer. This is the difference between "proxying large responses" and
    // "corrupting them".
    bool client_paused  = false;   // c2b_buf full  -> stop reading client_fd
    bool backend_paused = false;   // b2c_buf full  -> stop reading backend_fd

    // Current epoll interest masks, so that re-arming a socket for reading
    // never accidentally clears a pending EPOLLOUT.
    uint32_t client_mask  = EPOLLIN | EPOLLET;
    uint32_t backend_mask = EPOLLOUT | EPOLLET;

    Connection(int cfd, int bfd, int ep, Backend* b)
        : client_fd(cfd), backend_fd(bfd), state(ConnState::CONNECTING),
          epfd(ep), backend(b) {}

    // Apply an interest mask to one leg of the connection.
    void arm(int fd, uint32_t mask) {
        epoll_event e{};
        e.events  = mask;
        e.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e);
        if (fd == client_fd)  client_mask  = mask;
        else                  backend_mask = mask;
    }

    // Re-enable reading on `fd` without disturbing its EPOLLOUT state.
    void resume_read(int fd) {
        uint32_t mask = (fd == client_fd) ? client_mask : backend_mask;
        mask |= EPOLLIN | EPOLLET;
        arm(fd, mask);
    }

    // Free space in the buffer that feeds `fd`?  Returns the number of bytes
    // that may be read from `fd` right now.
    size_t space_for(int fd) const {
        if (fd == client_fd) return BUFFER_SIZE - c2b_end;
        else                 return BUFFER_SIZE - b2c_end;
    }

    // Mark the source of the buffer feeding `fd` as paused (its buffer is
    // full) / not paused.
    void set_paused(int fd, bool paused) {
        if (fd == client_fd) client_paused = paused;
        else                 backend_paused = paused;
    }

    bool is_paused(int fd) const {
        return (fd == client_fd) ? client_paused : backend_paused;
    }

    ~Connection() {
        epoll_ctl(epfd, EPOLL_CTL_DEL, client_fd,  nullptr);
        epoll_ctl(epfd, EPOLL_CTL_DEL, backend_fd, nullptr);
        close(client_fd);
        close(backend_fd);
        if (backend) backend->active_connections--;
        g_ctr.connections_closed.fetch_add(1, std::memory_order_relaxed);
        log_msg("Connection closed: client=%d backend=%d\n", client_fd, backend_fd);
    }

    void close_connection() { state = ConnState::CLOSING; }

    // -----------------------------------------------------------------------
    // try_write(fd)
    //   Writing TO fd -> drain the buffer destined for fd:
    //     backend_fd  <- c2b_buf   (data from client going to backend)
    //     client_fd   <- b2c_buf   (data from backend going to client)
    // -----------------------------------------------------------------------
    void try_write(int fd) {
        char*   buf;
        size_t* start;
        size_t* end;
        if (fd == backend_fd) { buf = c2b_buf; start = &c2b_start; end = &c2b_end; }
        else                  { buf = b2c_buf; start = &b2c_start; end = &b2c_end; }

        while (*start < *end) {
            ssize_t n = write(fd, buf + *start, *end - *start);
            if (n > 0) {
                *start += n;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                close_connection();
                return;
            }
        }
        if (*start >= *end) {
            *start = *end = 0;
            // Buffer fully drained: propagate any pending half-close.
            if (fd == backend_fd && client_eof) {
                shutdown(backend_fd, SHUT_WR);
            } else if (fd == client_fd && backend_eof) {
                shutdown(client_fd, SHUT_WR);
                close_connection();
            }
            // The buffer that feeds `fd` is now empty. If its source socket
            // was paused because the buffer was full, reading can resume.
            int src = (fd == backend_fd) ? client_fd : backend_fd;
            if (is_paused(src)) {
                set_paused(src, false);
                resume_read(src);
            }
        }
    }

    // -----------------------------------------------------------------------
    // append_to_buffer(fd, data, n)
    //   Reading FROM fd -> store into the buffer that carries data away from fd:
    //     client_fd  -> c2b_buf   (will be forwarded to backend)
    //     backend_fd -> b2c_buf   (will be forwarded to client)
    // -----------------------------------------------------------------------
    void append_to_buffer(int fd, const char* data, size_t n) {
        if (fd == client_fd) {
            size_t space = BUFFER_SIZE - c2b_end;
            if (n > space) n = space;
            memcpy(c2b_buf + c2b_end, data, n);
            c2b_end += n;
            g_ctr.bytes_c2b.fetch_add(n, std::memory_order_relaxed);
        } else {
            size_t space = BUFFER_SIZE - b2c_end;
            if (n > space) n = space;
            memcpy(b2c_buf + b2c_end, data, n);
            b2c_end += n;
            g_ctr.bytes_b2c.fetch_add(n, std::memory_order_relaxed);
        }
    }

    // -----------------------------------------------------------------------
    // has_data(fd) -- is there data buffered TO WRITE to fd?
    //   backend_fd is fed from c2b_buf
    //   client_fd  is fed from b2c_buf
    // -----------------------------------------------------------------------
    bool has_data(int fd) const {
        if (fd == backend_fd) return c2b_end > c2b_start;
        else                  return b2c_end > b2c_start;
    }

    // Re-arm the peer of fd for EPOLLOUT so buffered bytes get flushed.
    void arm_peer_write(int fd) {
        int peer = (fd == client_fd) ? backend_fd : client_fd;
        if (has_data(peer)) {
            arm(peer, EPOLLIN | EPOLLOUT | EPOLLET);
        }
    }
};

void remove_connection(Connection* conn, std::map<int, Connection*>& m) {
    if (!conn) return;
    m.erase(conn->client_fd);
    m.erase(conn->backend_fd);
    delete conn;
}

// ---------------------------------------------------------------------------
// Health checker
// ---------------------------------------------------------------------------
void backend_health_checker() {
    while (true) {
        for (auto& b : backends) {
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            struct timeval tv{ .tv_sec = 2, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(b->port);
            inet_pton(AF_INET, b->ip.c_str(), &addr.sin_addr);

            int ret = connect(sock, (sockaddr*)&addr, sizeof(addr));
            b->healthy = (ret == 0);
            close(sock);
        }
        std::this_thread::sleep_for(std::chrono::seconds(g_cfg.health_interval));
    }
}

// ---------------------------------------------------------------------------
// Worker thread  (one per CPU core, all sharing SO_REUSEPORT)
// ---------------------------------------------------------------------------
static std::atomic<int> g_workers_bound{0};

void worker_thread() {
    thread_local std::map<int, Connection*> fd_to_conn;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(g_cfg.listen_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd,  (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind");   return; }
    if (listen(listen_fd, 1024) < 0)                          { perror("listen"); return; }

    set_nonblocking(listen_fd);
    int epfd = epoll_create1(0);

    // Level-triggered on listen_fd: EPOLLET here can silently drop new
    // connections under burst load when a wakeup is missed.
    epoll_event ev{};
    ev.events  = EPOLLIN;   // NOT EPOLLET
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    epoll_event events[MAX_EVENTS];
    char        buffer[BUFFER_SIZE];

    log_msg("Worker started, port %d\n", g_cfg.listen_port);

    if (g_workers_bound.fetch_add(1, std::memory_order_release) + 1 == g_cfg.workers) {
        log_msg("READY: cppbalancer listening on port %d with %d workers, %zu backends\n",
                g_cfg.listen_port, g_cfg.workers, backends.size());
        fflush(stdout);
    }

    while (true) {
        int n_ready = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n_ready < 0) { if (errno == EINTR) continue; perror("epoll_wait"); break; }

        for (int i = 0; i < n_ready; ++i) {
            int      fd       = events[i].data.fd;
            uint32_t ev_flags = events[i].events;

            // ----------------------------------------------------------------
            // New incoming client
            // ----------------------------------------------------------------
            if (fd == listen_fd) {
                while (true) {
                    int cfd = accept(listen_fd, nullptr, nullptr);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept"); break;
                    }

                    set_nonblocking(cfd);
                    set_nodelay(cfd);

                    Backend* backend = select_backend();
                    if (!backend) {
                        log_msg("No healthy backend\n");
                        g_ctr.no_healthy_backend.fetch_add(1, std::memory_order_relaxed);
                        close(cfd);
                        continue;
                    }

                    int bfd = socket(AF_INET, SOCK_STREAM, 0);
                    set_nonblocking(bfd);
                    set_nodelay(bfd);

                    sockaddr_in baddr{};
                    baddr.sin_family = AF_INET;
                    baddr.sin_port   = htons(backend->port);
                    inet_pton(AF_INET, backend->ip.c_str(), &baddr.sin_addr);

                    int ret = connect(bfd, (sockaddr*)&baddr, sizeof(baddr));
                    if (ret < 0 && errno != EINPROGRESS) {
                        perror("backend connect");
                        g_ctr.backend_connect_fail.fetch_add(1, std::memory_order_relaxed);
                        close(cfd); close(bfd);
                        continue;
                    }

                    backend->active_connections++;
                    g_ctr.connections_accepted.fetch_add(1, std::memory_order_relaxed);

                    Connection* conn = new Connection(cfd, bfd, epfd, backend);
                    fd_to_conn[cfd] = conn;
                    fd_to_conn[bfd] = conn;

                    // client_fd: watch for reads immediately so the HTTP request
                    // is buffered in c2b_buf while the backend handshake is pending.
                    epoll_event e1{};
                    e1.events  = EPOLLIN | EPOLLET;
                    e1.data.fd = cfd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &e1);

                    // backend_fd: EPOLLOUT fires when the handshake completes
                    // (or immediately for a synchronous connect on loopback).
                    epoll_event e2{};
                    e2.events  = EPOLLOUT | EPOLLET;
                    e2.data.fd = bfd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, bfd, &e2);

                    // connect() can succeed synchronously on loopback.
                    // Transition to ACTIVE immediately so the pending client
                    // data isn't gated on a CONNECTING check that never clears.
                    if (ret == 0) conn->state = ConnState::ACTIVE;

                    log_msg("New client=%d backend=%d%s\n", cfd, bfd,
                            (ret == 0 ? " (sync)" : " (async)"));
                }
                continue;
            }

            // ----------------------------------------------------------------
            // Existing connection event
            // ----------------------------------------------------------------
            auto it = fd_to_conn.find(fd);
            if (it == fd_to_conn.end()) continue;
            Connection* conn = it->second;

            // ----------------------------------------------------------------
            // Backend handshake complete
            // ----------------------------------------------------------------
            if (conn->state == ConnState::CONNECTING &&
                fd == conn->backend_fd &&
                (ev_flags & EPOLLOUT))
            {
                int err = 0; socklen_t len = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                    log_msg("Backend connect failed: %s\n", strerror(err));
                    g_ctr.backend_connect_fail.fetch_add(1, std::memory_order_relaxed);
                    remove_connection(conn, fd_to_conn);
                    continue;
                }

                conn->state = ConnState::ACTIVE;

                // Switch to EPOLLIN. If the client already sent data during the
                // handshake it's sitting in c2b_buf — add EPOLLOUT to flush it.
                uint32_t flags = EPOLLIN | EPOLLET;
                if (conn->has_data(fd)) flags |= EPOLLOUT;

                conn->arm(fd, flags);
                continue;
            }

            bool removed = false;

            // ----------------------------------------------------------------
            // Readable
            // ----------------------------------------------------------------
            if (!removed && (ev_flags & EPOLLIN)) {
                bool active = (conn->state == ConnState::ACTIVE ||
                               conn->state == ConnState::HALF_CLOSED);
                while (true) {
                    // Never read more than the destination buffer can hold:
                    // anything beyond that would be silently discarded and
                    // would corrupt the stream.
                    size_t space = conn->space_for(fd);
                    if (space == 0) {
                        // Buffer full: apply backpressure. Stop reading from
                        // this socket until the peer drains the buffer.
                        conn->set_paused(fd, true);
                        break;
                    }

                    ssize_t n = read(fd, buffer, (sizeof(buffer) < space)
                                                ? sizeof(buffer) : space);
                    if (n > 0) {
                        conn->append_to_buffer(fd, buffer, (size_t)n);
                        if (active) conn->arm_peer_write(fd);

                        if (conn->space_for(fd) == 0) {
                            conn->set_paused(fd, true);
                            break;
                        }

                    } else if (n == 0) {
                        // Peer closed write side (FIN). Don't hard-close yet:
                        // the response body may still be in flight.
                        if (fd == conn->client_fd) {
                            conn->client_eof = true;
                            if (conn->c2b_end == conn->c2b_start)
                                shutdown(conn->backend_fd, SHUT_WR);
                        } else {
                            conn->backend_eof = true;
                            if (conn->b2c_end == conn->b2c_start) {
                                shutdown(conn->client_fd, SHUT_WR);
                                conn->close_connection();
                            }
                        }
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        remove_connection(conn, fd_to_conn);
                        removed = true;
                        break;
                    }
                }
            }

            // ----------------------------------------------------------------
            // Writable
            // ----------------------------------------------------------------
            if (!removed && (ev_flags & EPOLLOUT)) {
                conn->try_write(fd);
                if (!conn->has_data(fd)) {
                    conn->arm(fd, EPOLLIN | EPOLLET);
                }
            }

            // ----------------------------------------------------------------
            // Deferred cleanup
            // ----------------------------------------------------------------
            if (!removed && conn->state == ConnState::CLOSING) {
                remove_connection(conn, fd_to_conn);
            }
        }
    }

    close(listen_fd);
    close(epfd);
}

// ---------------------------------------------------------------------------
// Argument / environment parsing
// ---------------------------------------------------------------------------
static void usage(const char* argv0) {
    fprintf(stderr,
"Usage: %s [options]\n"
"\n"
"  --listen-port <port>      Port the balancer listens on            (default 8181)\n"
"  --backends <ip:port,...>  Comma separated backend pool            (default 127.0.0.1:9007,127.0.0.1:9003,127.0.0.1:6789)\n"
"  --workers <n>             Number of worker threads               (default: hardware concurrency)\n"
"  --health-interval <sec>   Backend health-check interval in sec    (default 5)\n"
"  --quiet                   Suppress per-connection logging        (default: logging on)\n"
"  --help                    Show this help\n"
"\n"
"Environment overrides: CPPBALANCER_LISTEN_PORT, CPPBALANCER_BACKENDS,\n"
"                       CPPBALANCER_WORKERS, CPPBALANCER_HEALTH_INTERVAL, CPPBALANCER_QUIET\n",
            argv0);
}

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

static void parse_args(int argc, char** argv) {
    // Environment first, command line wins.
    if (const char* p = getenv("CPPBALANCER_LISTEN_PORT"))     g_cfg.listen_port = atoi(p);
    if (const char* p = getenv("CPPBALANCER_BACKENDS"))        g_cfg.backend_specs = split_csv(p);
    if (const char* p = getenv("CPPBALANCER_WORKERS"))         g_cfg.workers = atoi(p);
    if (const char* p = getenv("CPPBALANCER_HEALTH_INTERVAL")) g_cfg.health_interval = atoi(p);
    if (const char* p = getenv("CPPBALANCER_QUIET"))           g_cfg.verbose = !(atoi(p) != 0);

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "%s requires a value\n", name); exit(2); }
            return argv[++i];
        };
        if      (a == "--listen-port")     g_cfg.listen_port = atoi(need("--listen-port").c_str());
        else if (a == "--backends")        g_cfg.backend_specs = split_csv(need("--backends"));
        else if (a == "--workers")         g_cfg.workers = atoi(need("--workers").c_str());
        else if (a == "--health-interval") g_cfg.health_interval = atoi(need("--health-interval").c_str());
        else if (a == "--quiet")           g_cfg.verbose = false;
        else if (a == "--help" || a == "-h") { usage(argv[0]); exit(0); }
        else { fprintf(stderr, "Unknown option: %s\n", a.c_str()); usage(argv[0]); exit(2); }
    }

    if (g_cfg.workers <= 0) {
        unsigned int hw = std::thread::hardware_concurrency();
        g_cfg.workers = (hw == 0) ? 2 : (int)hw;
    }
    if (g_cfg.listen_port <= 0 || g_cfg.listen_port > 65535) {
        fprintf(stderr, "Invalid --listen-port\n"); exit(2);
    }
    if (g_cfg.health_interval <= 0) g_cfg.health_interval = 5;
    g_verbose.store(g_cfg.verbose, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    parse_args(argc, argv);

    init_backends();
    std::thread(backend_health_checker).detach();

    unsigned int workers = (unsigned int)g_cfg.workers;

    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned int i = 0; i < workers; ++i)
        pool.emplace_back(worker_thread);

    for (auto& t : pool) t.join();
    return 0;
}
