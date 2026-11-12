/* ===========================================================================
 *  http_backend.c — minimal, fast HTTP/1.1 origin server used ONLY as the
 *  backend of the cppbalancer benchmark harness.
 *
 *  Why C and not the Python test server?
 *    The whole point of the benchmark is to measure the *balancer*. A Python
 *    backend would become the bottleneck (GIL, per-request parsing) and the
 *    measured numbers would describe Python, not cppbalancer. This server is
 *    deliberately as cheap as possible so the balancer stays the component
 *    under observation.
 *
 *  Design: one epoll event loop per thread, many concurrent connections per
 *  thread. (A thread-per-connection or connection-per-thread design would
 *  serialise keep-alive traffic and would become the bottleneck instead of
 *  the balancer — which is exactly the artefact we are trying to avoid.)
 *
 *  Features
 *    * multi-threaded with SO_REUSEPORT (one accept queue per thread, zero
 *      locking on the data path)
 *    * HTTP/1.1 keep-alive; honours `Connection: close`
 *    * configurable response body size (small-payload req/s and
 *      large-payload throughput are separate experiments)
 *    * echoes its identity in the X-Backend header so per-backend request
 *      distribution can be verified from the client side
 *    * optional artificial delay (to study queueing behaviour)
 *
 *  Build:  gcc -O2 -o http_backend http_backend.c -pthread
 *  Usage:  ./http_backend --port 9007 --threads 2 --body-size 1024 --label A
 * ===========================================================================*/

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_REQ      8192
#define MAX_CONNS    8192
#define MAX_EVENTS   256
#define MAX_THREADS  64

static int          g_port = 9007;
static int          g_delay_ms = 0;
static char         g_label[64] = "backend";
static char        *g_resp_keep = NULL;   /* response with Connection: keep-alive */
static size_t       g_resp_keep_len = 0;
static char        *g_resp_close = NULL;  /* response with Connection: close     */
static size_t       g_resp_close_len = 0;

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

struct conn {
    int         fd;
    char        req[MAX_REQ];
    size_t      req_used;
    const char *resp;
    size_t      resp_len;
    size_t      resp_off;
    int         close_after;
};

static void build_response(size_t body_size) {
    char hdr[512];

    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "X-Backend: %s\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        body_size, g_label);
    g_resp_keep_len = (size_t)n + body_size;
    g_resp_keep = (char *)malloc(g_resp_keep_len + 1);
    memcpy(g_resp_keep, hdr, (size_t)n);
    if (body_size) memset(g_resp_keep + n, 'x', body_size);
    g_resp_keep[g_resp_keep_len] = '\0';

    int n2 = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "X-Backend: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        body_size, g_label);
    g_resp_close_len = (size_t)n2 + body_size;
    g_resp_close = (char *)malloc(g_resp_close_len + 1);
    memcpy(g_resp_close, hdr, (size_t)n2);
    if (body_size) memset(g_resp_close + n2, 'x', body_size);
    g_resp_close[g_resp_close_len] = '\0';
}

static void *worker(void *arg) {
    (void)arg;
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return NULL; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)g_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("backend bind"); close(lfd); return NULL; }
    if (listen(lfd, 4096) < 0) { perror("backend listen"); close(lfd); return NULL; }

    /* Non-blocking listen socket: the accept() loop below must never stall
     * the event loop waiting for a connection that has not arrived yet. */
    int lfd_flags = fcntl(lfd, F_GETFL, 0);
    fcntl(lfd, F_SETFL, lfd_flags | O_NONBLOCK);

    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); close(lfd); return NULL; }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events   = EPOLLIN;
    ev.data.ptr = NULL;                 /* NULL marks the listen socket */
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);

    struct epoll_event events[MAX_EVENTS];

    while (!g_stop) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, 200);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            struct conn *c = (struct conn *)events[i].data.ptr;

            if (c == NULL) {                       /* listen socket */
                for (;;) {
                    int cfd = accept(lfd, NULL, NULL);
                    if (cfd < 0) break;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    int flags = fcntl(cfd, F_GETFL, 0);
                    fcntl(cfd, F_SETFL, flags | O_NONBLOCK);

                    struct conn *nc = calloc(1, sizeof(struct conn));
                    nc->fd = cfd;
                    memset(&ev, 0, sizeof(ev));
                    ev.events   = EPOLLIN | EPOLLRDHUP;
                    ev.data.ptr = nc;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
                        close(cfd);
                        free(nc);
                    }
                }
                continue;
            }

            int want_close = (events[i].events & (EPOLLHUP | EPOLLERR)) != 0;

            /* 1) Flush a response that could not be written in one go. */
            if (!want_close && c->resp && c->resp_off < c->resp_len) {
                ssize_t w = write(c->fd, c->resp + c->resp_off, c->resp_len - c->resp_off);
                if (w > 0) {
                    c->resp_off += (size_t)w;
                } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    want_close = 1;
                }
            }

            /* 2) Read the request. */
            if (!want_close && (events[i].events & EPOLLIN)) {
                ssize_t r = read(c->fd, c->req + c->req_used,
                                 sizeof(c->req) - c->req_used - 1);
                if (r > 0) {
                    c->req_used += (size_t)r;
                    c->req[c->req_used] = '\0';
                    if (strstr(c->req, "\r\n\r\n")) {
                        if (g_delay_ms > 0) {
                            struct timespec ts;
                            ts.tv_sec  = g_delay_ms / 1000;
                            ts.tv_nsec = (long)(g_delay_ms % 1000) * 1000000L;
                            nanosleep(&ts, NULL);
                        }
                        if (strcasestr(c->req, "connection: close")) {
                            c->resp = g_resp_close;
                            c->resp_len = g_resp_close_len;
                            c->close_after = 1;
                        } else {
                            c->resp = g_resp_keep;
                            c->resp_len = g_resp_keep_len;
                            c->close_after = 0;
                        }
                        c->resp_off = 0;
                        c->req_used = 0;

                        ssize_t w = write(c->fd, c->resp, c->resp_len);
                        if (w > 0) {
                            c->resp_off = (size_t)w;
                        } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                            want_close = 1;
                        }
                    }
                } else if (r == 0) {
                    want_close = 1;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    want_close = 1;
                }
            }

            /* 3) Response fully written? */
            if (!want_close && c->resp && c->resp_off >= c->resp_len) {
                if (c->close_after) {
                    want_close = 1;
                } else {
                    c->resp = NULL;
                }
            }

            /* 4) Still pending -> ask for writability. */
            if (!want_close && c->resp && c->resp_off < c->resp_len) {
                struct epoll_event mev;
                memset(&mev, 0, sizeof(mev));
                mev.events   = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                mev.data.ptr = c;
                epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &mev);
            }

            if (want_close) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
                close(c->fd);
                free(c);
            }
        }
    }

    close(epfd);
    close(lfd);
    return NULL;
}

int main(int argc, char **argv) {
    int    threads = 1;
    size_t body_size = 1024;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)            g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc)    threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--body-size") && i + 1 < argc)  body_size = (size_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--label") && i + 1 < argc)      snprintf(g_label, sizeof(g_label), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--delay-ms") && i + 1 < argc)   g_delay_ms = atoi(argv[++i]);
        else {
            fprintf(stderr,
                "Usage: %s [--port N] [--threads N] [--body-size BYTES] [--label NAME] [--delay-ms N]\n",
                argv[0]);
            return 2;
        }
    }
    if (threads < 1) threads = 1;
    if (threads > MAX_THREADS) threads = MAX_THREADS;

    signal(SIGINT, on_sig);
    signal(SIGPIPE, SIG_IGN);

    build_response(body_size);

    fprintf(stderr, "http_backend: port=%d threads=%d body=%zu label=%s delay_ms=%d\n",
            g_port, threads, body_size, g_label, g_delay_ms);
    fflush(stderr);

    pthread_t t[MAX_THREADS];
    for (int i = 0; i < threads; ++i) pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < threads; ++i) pthread_join(t[i], NULL);
    return 0;
}
