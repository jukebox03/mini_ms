#include "epoll_server.h"
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

upstream_handler_fn g_upstream_handler = NULL;

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

conn_t *conn_alloc(int fd, conn_state_t state, int epfd) {
    conn_t *c = calloc(1, sizeof(conn_t));
    if (!c) return NULL;
    c->fd = fd;
    c->state = state;
    c->epfd = epfd;
    return c;
}

void conn_free(conn_t *c) {
    if (!c) return;
    if (c->fd >= 0) {
        epoll_ctl(c->epfd, EPOLL_CTL_DEL, c->fd, NULL);
        close(c->fd);
    }
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

void conn_start_response(conn_t *c, int status, const char *body, size_t body_len) {
    c->wlen = http_format_response(c->wbuf, sizeof(c->wbuf), status, body, body_len);
    c->wpos = 0;
    c->state = CONN_CLIENT_WRITING;

    struct epoll_event ev = { .events = EPOLLOUT, .data.ptr = c };
    epoll_ctl(c->epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

int conn_start_upstream(conn_t *client, const char *host, int port,
                        const char *method, const char *path,
                        void *handler_ctx) {
    /* Resolve host */
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

    conn_t *uc = conn_alloc(fd, CONN_UPSTREAM_CONNECTING, client->epfd);
    if (!uc) { close(fd); return -1; }
    uc->client_conn = client;
    uc->handler_ctx = handler_ctx;

    /* Prepare HTTP request in write buffer */
    uc->wlen = snprintf(uc->wbuf, sizeof(uc->wbuf),
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        method, path, host);
    uc->wpos = 0;

    struct epoll_event ev = { .events = EPOLLOUT, .data.ptr = uc };
    epoll_ctl(client->epfd, EPOLL_CTL_ADD, fd, &ev);
    return 0;
}

static void handle_accept(conn_t *listen_conn) {
    while (1) {
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int fd = accept(listen_conn->fd, (struct sockaddr *)&addr, &alen);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("accept");
            break;
        }
        set_nonblocking(fd);
        int opt = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        conn_t *c = conn_alloc(fd, CONN_CLIENT_READING, listen_conn->epfd);
        if (!c) { close(fd); continue; }

        struct epoll_event ev = { .events = EPOLLIN, .data.ptr = c };
        epoll_ctl(listen_conn->epfd, EPOLL_CTL_ADD, fd, &ev);
    }
}

static void handle_client_read(conn_t *c, request_handler_fn handler) {
    int n = read(c->fd, c->rbuf + c->rlen, sizeof(c->rbuf) - c->rlen - 1);
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

    handler(c, &req);
}

static void handle_client_write(conn_t *c) {
    int remaining = c->wlen - c->wpos;
    int n = write(c->fd, c->wbuf + c->wpos, remaining);
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

static void handle_upstream_connected(conn_t *uc) {
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(uc->fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
        fprintf(stderr, "[upstream] connect failed: %s\n", strerror(err));
        /* Send error to client */
        if (uc->client_conn) {
            conn_start_response(uc->client_conn, 502,
                "{\"error\":\"upstream connect failed\"}", 35);
        }
        conn_free(uc);
        return;
    }
    uc->state = CONN_UPSTREAM_WRITING;
    /* EPOLLOUT is already set, so write handler will fire */
}

static void handle_upstream_write(conn_t *uc) {
    int remaining = uc->wlen - uc->wpos;
    int n = write(uc->fd, uc->wbuf + uc->wpos, remaining);
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
        /* Done writing, switch to reading response */
        uc->state = CONN_UPSTREAM_READING;
        uc->rlen = 0;
        struct epoll_event ev = { .events = EPOLLIN, .data.ptr = uc };
        epoll_ctl(uc->epfd, EPOLL_CTL_MOD, uc->fd, &ev);
    }
}

static void handle_upstream_read(conn_t *uc) {
    int n = read(uc->fd, uc->rbuf + uc->rlen, sizeof(uc->rbuf) - uc->rlen - 1);
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

void epoll_run(int listen_fd, request_handler_fn handler) {
    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); exit(1); }

    conn_t *lc = conn_alloc(listen_fd, CONN_LISTENING, epfd);
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = lc };
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    /*
     * === DPUmesh integration point (future) ===
     * int dpu_fd = dpumesh_get_notify_fd(ctx);
     * conn_t *dpu = conn_alloc(dpu_fd, CONN_DPUMESH_NOTIFY, epfd);
     * struct epoll_event dpu_ev = { .events = EPOLLIN, .data.ptr = dpu };
     * epoll_ctl(epfd, EPOLL_CTL_ADD, dpu_fd, &dpu_ev);
     */

    struct epoll_event events[MAX_EVENTS];
    printf("[server] epoll loop started\n");

    while (1) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; i++) {
            conn_t *c = events[i].data.ptr;
            switch (c->state) {
                case CONN_LISTENING:
                    handle_accept(c);
                    break;
                case CONN_CLIENT_READING:
                    handle_client_read(c, handler);
                    break;
                case CONN_CLIENT_WRITING:
                    handle_client_write(c);
                    break;
                case CONN_UPSTREAM_CONNECTING:
                    handle_upstream_connected(c);
                    break;
                case CONN_UPSTREAM_WRITING:
                    handle_upstream_write(c);
                    break;
                case CONN_UPSTREAM_READING:
                    handle_upstream_read(c);
                    break;
                /*
                 * Future:
                 * case CONN_DPUMESH_NOTIFY:
                 *     dpumesh_poll(ctx, on_response, NULL, on_request, NULL);
                 *     break;
                 */
            }
        }
    }
}
