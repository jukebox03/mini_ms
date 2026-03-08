/*
 * epoll_server_dpumesh.c - epoll event loop with DPUmesh notify_fd
 *
 * This is the key integration point: one fd added, one case added.
 */

#include "epoll_server_dpumesh.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

/* Reuse CONN_LISTENING..CONN_UPSTREAM_READING from epoll_server.c */
/* We add a new pseudo-state for the DPUmesh fd */
#define CONN_DPUMESH_NOTIFY 100

/* We store dpumesh callback context in a simple struct */
typedef struct {
    dpumesh_ctx_t          *dpu_ctx;
    dpumesh_on_response_fn  on_resp;
    void                   *resp_data;
    dpumesh_on_request_fn   on_req;
    void                   *req_data;
} dpu_epoll_ctx_t;

static dpu_epoll_ctx_t g_dpu_epoll;

void epoll_run_dpumesh(int listen_fd, request_handler_fn handler,
                       dpumesh_ctx_t *dpu_ctx,
                       dpumesh_on_response_fn on_resp, void *resp_data,
                       dpumesh_on_request_fn on_req, void *req_data) {
    int epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); exit(1); }

    /* Register listen socket (if provided) */
    if (listen_fd >= 0) {
        conn_t *lc = conn_alloc(listen_fd, CONN_LISTENING, epfd);
        struct epoll_event ev = { .events = EPOLLIN, .data.ptr = lc };
        epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);
    }

    /* ======= DPUmesh integration: one fd, one registration ======= */
    int dpu_fd = dpumesh_get_notify_fd(dpu_ctx);
    conn_t *dpu_conn = conn_alloc(dpu_fd, CONN_DPUMESH_NOTIFY, epfd);
    struct epoll_event dpu_ev = { .events = EPOLLIN, .data.ptr = dpu_conn };
    epoll_ctl(epfd, EPOLL_CTL_ADD, dpu_fd, &dpu_ev);

    g_dpu_epoll.dpu_ctx = dpu_ctx;
    g_dpu_epoll.on_resp = on_resp;
    g_dpu_epoll.resp_data = resp_data;
    g_dpu_epoll.on_req = on_req;
    g_dpu_epoll.req_data = req_data;
    /* ============================================================= */

    struct epoll_event events[MAX_EVENTS];
    printf("[server] epoll loop started (with DPUmesh notify_fd=%d)\n", dpu_fd);

    while (1) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; i++) {
            conn_t *c = events[i].data.ptr;

            /* ======= DPUmesh: one case branch ======= */
            if (c->state == CONN_DPUMESH_NOTIFY) {
                dpumesh_poll(g_dpu_epoll.dpu_ctx,
                             g_dpu_epoll.on_resp, g_dpu_epoll.resp_data,
                             g_dpu_epoll.on_req,  g_dpu_epoll.req_data);
                continue;
            }
            /* ========================================= */

            switch (c->state) {
                case CONN_LISTENING:
                    /* Accept handled by epoll_server.c's handle_accept equivalent */
                    {
                        while (1) {
                            struct sockaddr_in addr;
                            socklen_t alen = sizeof(addr);
                            int fd = accept(c->fd, (struct sockaddr *)&addr, &alen);
                            if (fd < 0) break;
                            int flags = fcntl(fd, F_GETFL, 0);
                            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                            int opt = 1;
                            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
                            conn_t *nc = conn_alloc(fd, CONN_CLIENT_READING, epfd);
                            if (!nc) { close(fd); continue; }
                            struct epoll_event ev = { .events = EPOLLIN, .data.ptr = nc };
                            epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
                        }
                    }
                    break;
                case CONN_CLIENT_READING:
                    {
                        int nr = read(c->fd, c->rbuf + c->rlen, sizeof(c->rbuf) - c->rlen - 1);
                        if (nr <= 0) { conn_free(c); break; }
                        c->rlen += nr;
                        c->rbuf[c->rlen] = '\0';
                        http_request_t req;
                        if (http_parse_request(&req, c->rbuf, c->rlen) == 0 && handler)
                            handler(c, &req);
                    }
                    break;
                case CONN_CLIENT_WRITING:
                    {
                        int rem = c->wlen - c->wpos;
                        int nw = write(c->fd, c->wbuf + c->wpos, rem);
                        if (nw < 0) { if (errno != EAGAIN) conn_free(c); break; }
                        c->wpos += nw;
                        if (c->wpos >= c->wlen) conn_free(c);
                    }
                    break;
                case CONN_UPSTREAM_CONNECTING:
                case CONN_UPSTREAM_WRITING:
                case CONN_UPSTREAM_READING:
                    /* These states use the same handlers from epoll_server.c
                     * For dpumesh version, upstream calls go through SHM, not TCP.
                     * So these states are only used if TCP fallback is needed. */
                    break;
                default:
                    break;
            }
        }
    }
}
