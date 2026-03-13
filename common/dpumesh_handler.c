#include "dpumesh_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ====== Internal state ====== */

static dpumesh_ctx_t       *g_dpu_ctx = NULL;
static struct event_base   *g_dpu_base = NULL;
static request_handler_fn   g_dpu_request_handler = NULL;
static upstream_handler_fn  g_dpu_upstream_handler = NULL;

/* ====== Connection alloc/free ====== */

static dpu_conn_t *dpu_conn_alloc(void) {
    dpu_conn_t *c = calloc(1, sizeof(dpu_conn_t));
    if (!c) return NULL;
    c->base.fd = -1;
    c->base.base = g_dpu_base;
    return c;
}

static void dpu_conn_free(dpu_conn_t *c) {
    if (!c) return;
    if (c->base.ev) event_free(c->base.ev);
    if (c->base.fd >= 0) close(c->base.fd);
    free(c);
}

/* Forward declarations
 *
 * TCP handler has 5 callbacks for the full lifecycle:
 *   on_client_read_cb         — inbound request read
 *   on_client_write_cb        — response write
 *   on_upstream_connect_cb    — upstream TCP connect completion
 *   on_upstream_write_cb      — upstream request write
 *   on_upstream_read_cb       — upstream response read
 *
 * SHM eliminates network latency, so 3 of those collapse:
 *   on_upstream_connect_cb    — not needed: SHM has no connection handshake
 *   on_upstream_write_cb      — not needed: dpumesh_write() completes synchronously (memcpy)
 *   on_client_write_cb        — not needed: dpumesh_conn_respond() completes synchronously
 *
 * What remains maps 1:1:
 *   on_notify_cb              ↔ on_accept_cb + on_client_read_cb
 *   on_upstream_response_cb   ↔ on_upstream_read_cb
 *
 * dpu_conn_t stores a single dpu_desc_index (opaque index into ctx->inbound[])
 * instead of copying 4 routing metadata fields. The library (dpumesh.c) owns
 * the metadata; the handler just passes the index through.
 */
static void on_upstream_response_cb(evutil_socket_t fd, short what, void *arg);

/* ====== Inbound: notify_fd readable → dpumesh_read → request handler ====== */

static void on_request(const dpumesh_request_t *req, void *user_data) {
    (void)user_data;

    dpu_conn_t *c = dpu_conn_alloc();
    if (!c) return;

    /* Store descriptor index for later dpumesh_conn_respond() */
    c->dpu_desc_index = req->_desc_index;

    /* Format as HTTP request for http_parse_request() */
    const char *method = req->method ? req->method : "GET";
    const char *path = req->path ? req->path : "/";
    const char *qs = req->query_string ? req->query_string : "";
    const char *body = req->body ? req->body : "";
    size_t body_len = req->body_len;

    /* Handle dpumesh's 1-byte null-terminator for empty bodies */
    if (body_len == 1 && body[0] == '\0') {
        body_len = 0;
    }

    if (qs[0]) {
        c->base.rlen = snprintf(c->base.rbuf, sizeof(c->base.rbuf),
            "%s %s?%s HTTP/1.1\r\n"
            "Host: dpumesh\r\n"
            "Content-Length: %zu\r\n"
            "\r\n"
            "%.*s",
            method, path, qs, body_len, (int)body_len, body);
    } else {
        c->base.rlen = snprintf(c->base.rbuf, sizeof(c->base.rbuf),
            "%s %s HTTP/1.1\r\n"
            "Host: dpumesh\r\n"
            "Content-Length: %zu\r\n"
            "\r\n"
            "%.*s",
            method, path, body_len, (int)body_len, body);
    }

    http_request_t http_req;
    int ret = http_parse_request(&http_req, c->base.rbuf, c->base.rlen);
    if (ret == 0 && g_dpu_request_handler)
        g_dpu_request_handler(&c->base, &http_req);
    /* Don't free — handler owns it, will call dpumesh_conn_respond */
}

static void on_notify_cb(evutil_socket_t fd, short what, void *arg) {
    (void)fd; (void)what; (void)arg;
    dpumesh_read(g_dpu_ctx, on_request, NULL);
}

/* ====== Per-request: respond ====== */

void dpumesh_conn_respond(conn_t *client,
                          int status, const char *body, size_t body_len) {
    dpu_conn_t *dc = (dpu_conn_t *)client;

    /* Reconstruct dpumesh_request_t with descriptor index */
    dpumesh_request_t req;
    memset(&req, 0, sizeof(req));
    req._desc_index = dc->dpu_desc_index;

    dpumesh_msg_t msg = {0};
    msg.type = DPUMESH_MSG_RESPONSE;
    msg.status_code = status;
    msg.orig_req = &req;
    msg.body = body;
    msg.body_len = body_len;
    dpumesh_write(g_dpu_ctx, &msg, NULL);

    dpu_conn_free(dc);
}

/* ====== Per-request: upstream ====== */

static void on_upstream_response_cb(evutil_socket_t fd, short what, void *arg) {
    (void)what;
    dpu_conn_t *uc = arg;

    dpumesh_response_t resp;
    if (dpumesh_read_response(g_dpu_ctx, fd, &resp) < 0) {
        dpu_conn_free(uc);
        return;
    }

    /* fd is closed by dpumesh_read_response, clear so dpu_conn_free won't double-close */
    uc->base.fd = -1;

    int status = resp.status_code > 0 ? resp.status_code : 200;
    const char *body = resp.body ? resp.body : "";
    size_t body_len = resp.body_len;

    uc->base.rlen = snprintf(uc->base.rbuf, sizeof(uc->base.rbuf),
        "HTTP/1.1 %d OK\r\n"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%.*s",
        status, body_len, (int)body_len, body);

    http_response_t http_resp;
    int ret = http_parse_response(&http_resp, uc->base.rbuf, uc->base.rlen);
    if (ret == 0 && g_dpu_upstream_handler)
        g_dpu_upstream_handler(&uc->base, &http_resp);
    dpu_conn_free(uc);
}

int dpumesh_conn_upstream(conn_t *client,
                          const char *method, const char *url,
                          void *handler_ctx) {
    dpu_conn_t *dc = (dpu_conn_t *)client;

    dpu_conn_t *uc = dpu_conn_alloc();
    if (!uc) return -1;
    uc->base.client_conn = client;
    uc->base.handler_ctx = handler_ctx;

    /* Copy descriptor index from client for chained responses */
    uc->dpu_desc_index = dc->dpu_desc_index;

    /* Send request via SHM — get per-request response fd */
    dpumesh_msg_t msg = {0};
    msg.type = DPUMESH_MSG_REQUEST;
    msg.method = method;
    msg.url = url;
    int response_fd;
    if (dpumesh_write(g_dpu_ctx, &msg, &response_fd) < 0) {
        free(uc);
        return -1;
    }

    /* Register response fd with libevent (same pattern as TCP!) */
    uc->base.fd = response_fd;
    uc->base.ev = event_new(g_dpu_base, response_fd, EV_READ,
                             on_upstream_response_cb, uc);
    event_add(uc->base.ev, NULL);

    return 0;
}

/* ====== Public API ====== */

int dpumesh_handler_init(struct event_base *base, dpumesh_ctx_t *dpu_ctx,
                          request_handler_fn handler) {
    g_dpu_base = base;
    g_dpu_ctx = dpu_ctx;
    g_dpu_request_handler = handler;

    int fd = dpumesh_get_notify_fd(dpu_ctx);
    struct event *ev = event_new(base, fd, EV_READ|EV_PERSIST,
                                  on_notify_cb, NULL);
    event_add(ev, NULL);

    printf("[server] dpumesh handler initialized\n");
    fflush(stdout);
    return 0;
}

void dpumesh_handler_set_upstream(upstream_handler_fn handler) {
    g_dpu_upstream_handler = handler;
}

void dpumesh_run(dpumesh_ctx_t *dpu_ctx, request_handler_fn handler) {
    struct event_base *base = event_base_new();
    dpumesh_handler_init(base, dpu_ctx, handler);
    printf("[server] event loop started\n");
    event_base_dispatch(base);
    event_base_free(base);
}
