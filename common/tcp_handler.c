#include "tcp_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

/* ====== Internal state ====== */

request_handler_fn  g_request_handler = NULL;
upstream_handler_fn g_upstream_handler = NULL;

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ====== Connection alloc/free ====== */

conn_t *conn_alloc(int fd, struct event_base *base) {
    conn_t *c = calloc(1, sizeof(conn_t));
    if (!c) return NULL;
    c->fd = fd;
    c->base = base;
    return c;
}

void conn_free(conn_t *c) {
    if (!c) return;
    if (c->ev) event_free(c->ev);
    if (c->fd >= 0) close(c->fd);
    free(c);
}

int make_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblocking(fd);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 128) < 0) {
        perror("listen"); close(fd); return -1;
    }
    printf("[server] listening on port %d\n", port);
    return fd;
}

/* Forward declarations */
static void on_client_read_cb(evutil_socket_t fd, short what, void *arg);
static void on_client_write_cb(evutil_socket_t fd, short what, void *arg);
static void on_upstream_connect_cb(evutil_socket_t fd, short what, void *arg);
static void on_upstream_write_cb(evutil_socket_t fd, short what, void *arg);
static void on_upstream_read_cb(evutil_socket_t fd, short what, void *arg);

/* ====== Inbound: listen_fd readable → accept → read → request handler ====== */

static void on_accept_cb(evutil_socket_t fd, short what, void *arg) {
    (void)what;
    struct event_base *base = arg;

    while (1) {
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int cfd = accept(fd, (struct sockaddr *)&addr, &alen);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("accept");
            break;
        }
        set_nonblocking(cfd);
        int opt = 1;
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        conn_t *c = conn_alloc(cfd, base);
        if (!c) { close(cfd); continue; }

        c->ev = event_new(base, cfd, EV_READ|EV_PERSIST, on_client_read_cb, c);
        event_add(c->ev, NULL);
    }
}

static void on_client_read_cb(evutil_socket_t fd, short what, void *arg) {
    (void)what;
    conn_t *c = arg;

    int n = read(fd, c->rbuf + c->rlen, sizeof(c->rbuf) - c->rlen - 1);
    if (n <= 0) {
        conn_free(c);
        return;
    }
    c->rlen += n;
    c->rbuf[c->rlen] = '\0';

    http_request_t req;
    int ret = http_parse_request(&req, c->rbuf, c->rlen);
    if (ret == 1) return;  /* need more data */
    if (ret < 0) {
        conn_start_response(c, 400, "{\"error\":\"bad request\"}", 23);
        return;
    }

    if (g_request_handler)
        g_request_handler(c, &req);
}

/* ====== Per-request: respond ====== */

void conn_start_response(conn_t *c, int status, const char *body, size_t body_len) {
    c->wlen = http_format_response(c->wbuf, sizeof(c->wbuf), status, body, body_len);
    c->wpos = 0;
    if (c->ev) event_free(c->ev);
    c->ev = event_new(c->base, c->fd, EV_WRITE|EV_PERSIST, on_client_write_cb, c);
    event_add(c->ev, NULL);
}

static void on_client_write_cb(evutil_socket_t fd, short what, void *arg) {
    (void)what;
    conn_t *c = arg;

    int remaining = c->wlen - c->wpos;
    int n = write(fd, c->wbuf + c->wpos, remaining);
    if (n < 0) {
        if (errno == EAGAIN) return;
        conn_free(c);
        return;
    }
    c->wpos += n;
    if (c->wpos >= c->wlen) {
        conn_free(c);
    }
}

/* ====== Per-request: upstream ====== */

int conn_start_upstream(conn_t *client, const char *host, int port,
                        const char *method, const char *path,
                        void *handler_ctx) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    set_nonblocking(fd);

    int ret = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (ret < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    conn_t *uc = conn_alloc(fd, client->base);
    if (!uc) { close(fd); return -1; }
    uc->client_conn = client;
    uc->handler_ctx = handler_ctx;

    uc->wlen = snprintf(uc->wbuf, sizeof(uc->wbuf),
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        method, path, host);
    uc->wpos = 0;

    uc->ev = event_new(client->base, fd, EV_WRITE|EV_PERSIST,
                        on_upstream_connect_cb, uc);
    event_add(uc->ev, NULL);
    return 0;
}

static void on_upstream_connect_cb(evutil_socket_t fd, short what, void *arg) {
    (void)what;
    conn_t *uc = arg;

    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
        fprintf(stderr, "[upstream] connect failed: %s\n", strerror(err));
        if (uc->client_conn) {
            conn_start_response(uc->client_conn, 502,
                "{\"error\":\"upstream connect failed\"}", 35);
        }
        conn_free(uc);
        return;
    }

    /* Connected — switch to write callback */
    if (uc->ev) event_free(uc->ev);
    uc->ev = event_new(uc->base, fd, EV_WRITE|EV_PERSIST,
                        on_upstream_write_cb, uc);
    event_add(uc->ev, NULL);
}

static void on_upstream_write_cb(evutil_socket_t fd, short what, void *arg) {
    (void)what;
    conn_t *uc = arg;

    int remaining = uc->wlen - uc->wpos;
    int n = write(fd, uc->wbuf + uc->wpos, remaining);
    if (n < 0) {
        if (errno == EAGAIN) return;
        if (uc->client_conn)
            conn_start_response(uc->client_conn, 502,
                "{\"error\":\"upstream write failed\"}", 33);
        conn_free(uc);
        return;
    }
    uc->wpos += n;
    if (uc->wpos >= uc->wlen) {
        /* Done writing — switch to reading response */
        uc->rlen = 0;
        if (uc->ev) event_free(uc->ev);
        uc->ev = event_new(uc->base, fd, EV_READ|EV_PERSIST,
                            on_upstream_read_cb, uc);
        event_add(uc->ev, NULL);
    }
}

static void on_upstream_read_cb(evutil_socket_t fd, short what, void *arg) {
    (void)what;
    conn_t *uc = arg;

    int n = read(fd, uc->rbuf + uc->rlen, sizeof(uc->rbuf) - uc->rlen - 1);
    if (n < 0) {
        if (errno == EAGAIN) return;
        if (uc->client_conn)
            conn_start_response(uc->client_conn, 502,
                "{\"error\":\"upstream read failed\"}", 32);
        conn_free(uc);
        return;
    }
    if (n == 0) {
        /* Connection closed by upstream — try to parse what we have */
        uc->rbuf[uc->rlen] = '\0';
        http_response_t resp;
        if (http_parse_response(&resp, uc->rbuf, uc->rlen) == 0 && g_upstream_handler) {
            g_upstream_handler(uc, &resp);
        } else if (uc->client_conn) {
            conn_start_response(uc->client_conn, 502,
                "{\"error\":\"upstream closed\"}", 27);
        }
        conn_free(uc);
        return;
    }
    uc->rlen += n;
    uc->rbuf[uc->rlen] = '\0';

    http_response_t resp;
    int ret = http_parse_response(&resp, uc->rbuf, uc->rlen);
    if (ret == 1) return; /* need more data */
    if (ret < 0) {
        if (uc->client_conn)
            conn_start_response(uc->client_conn, 502,
                "{\"error\":\"upstream bad response\"}", 33);
        conn_free(uc);
        return;
    }

    if (g_upstream_handler)
        g_upstream_handler(uc, &resp);
    conn_free(uc);
}

/* ====== Public API ====== */

void tcp_listen_start(struct event_base *base, int listen_fd,
                      request_handler_fn handler) {
    g_request_handler = handler;
    struct event *ev = event_new(base, listen_fd, EV_READ|EV_PERSIST,
                                  on_accept_cb, base);
    event_add(ev, NULL);
}

void tcp_handler_set_upstream(upstream_handler_fn handler) {
    g_upstream_handler = handler;
}

void epoll_run(int listen_fd, request_handler_fn handler) {
    struct event_base *base = event_base_new();
    tcp_listen_start(base, listen_fd, handler);
    printf("[server] event loop started\n");
    event_base_dispatch(base);
    event_base_free(base);
}
