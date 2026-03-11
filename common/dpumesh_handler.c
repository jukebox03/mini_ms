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

/* ====== Pending upstream map (single-threaded, no mutex needed) ====== */

#define MAX_PENDING_UPSTREAM 256

typedef struct {
    uint32_t    req_id;
    dpu_conn_t *conn;
} pending_entry_t;

static pending_entry_t g_pending[MAX_PENDING_UPSTREAM];

static void pending_add(uint32_t req_id, dpu_conn_t *conn) {
    for (int i = 0; i < MAX_PENDING_UPSTREAM; i++) {
        if (g_pending[i].conn == NULL) {
            g_pending[i].req_id = req_id;
            g_pending[i].conn = conn;
            return;
        }
    }
}

static dpu_conn_t *pending_remove(uint32_t req_id) {
    for (int i = 0; i < MAX_PENDING_UPSTREAM; i++) {
        if (g_pending[i].conn && g_pending[i].req_id == req_id) {
            dpu_conn_t *result = g_pending[i].conn;
            g_pending[i].conn = NULL;
            g_pending[i].req_id = 0;
            return result;
        }
    }
    return NULL;
}

/* ====== dpu_conn_t alloc/free ====== */

static dpu_conn_t *dpu_conn_alloc(void) {
    dpu_conn_t *c = calloc(1, sizeof(dpu_conn_t));
    if (!c) return NULL;
    c->base.fd = -1;    /* no real socket for dpumesh conns */
    c->base.base = g_dpu_base;
    return c;
}

static void dpu_conn_free(dpu_conn_t *c) {
    if (!c) return;
    free(c);
}

/* ====== dpumesh_poll callbacks (main thread, no queue needed) ====== */

static void on_response(dpumesh_req_id req_id,
                         const dpumesh_response_t *resp,
                         void *user_data) {
    (void)user_data;

    dpu_conn_t *uc = pending_remove(req_id);
    if (!uc) {
        fprintf(stderr, "[dpumesh] unknown response req_id=%u\n", req_id);
        return;
    }

    int status = resp->status_code > 0 ? resp->status_code : 200;
    const char *body = resp->body ? resp->body : "";
    size_t body_len = resp->body_len;

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

static void on_request(const dpumesh_request_t *req, void *user_data) {
    (void)user_data;

    dpu_conn_t *c = dpu_conn_alloc();
    if (!c) return;

    /* Store dpumesh metadata for later dpumesh_conn_respond() */
    snprintf(c->dpu_req_id_str, sizeof(c->dpu_req_id_str), "%s",
             req->req_id_str);
    snprintf(c->dpu_source_worker, sizeof(c->dpu_source_worker), "%s",
             req->source_worker);
    c->dpu_case_flag = req->_case_flag;
    c->dpu_req_id = req->req_id;

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

/* ====== Notify fd callback ====== */

static void on_notify_cb(evutil_socket_t fd, short what, void *arg) {
    (void)fd; (void)what; (void)arg;
    dpumesh_poll(g_dpu_ctx, on_response, NULL, on_request, NULL);
}

/* ====== Public API ====== */

void dpumesh_conn_respond(conn_t *client,
                          int status, const char *body, size_t body_len) {
    dpu_conn_t *dc = (dpu_conn_t *)client;

    /* Reconstruct dpumesh_request_t from stored metadata */
    dpumesh_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.req_id_str, sizeof(req.req_id_str), "%s", dc->dpu_req_id_str);
    snprintf(req.source_worker, sizeof(req.source_worker), "%s",
             dc->dpu_source_worker);
    req.req_id = dc->dpu_req_id;
    req._case_flag = dc->dpu_case_flag;

    dpumesh_respond(g_dpu_ctx, &req, status, body, body_len);

    dpu_conn_free(dc);
}

int dpumesh_conn_upstream(conn_t *client,
                          const char *method, const char *url,
                          void *handler_ctx) {
    dpu_conn_t *dc = (dpu_conn_t *)client;

    dpu_conn_t *uc = dpu_conn_alloc();
    if (!uc) return -1;
    uc->base.client_conn = client;
    uc->base.handler_ctx = handler_ctx;

    /* Copy dpumesh metadata from client for chained responses */
    snprintf(uc->dpu_req_id_str, sizeof(uc->dpu_req_id_str), "%s",
             dc->dpu_req_id_str);
    snprintf(uc->dpu_source_worker, sizeof(uc->dpu_source_worker), "%s",
             dc->dpu_source_worker);
    uc->dpu_case_flag = dc->dpu_case_flag;
    uc->dpu_req_id = dc->dpu_req_id;

    /* Send request via SHM */
    uint32_t req_id = dpumesh_send(g_dpu_ctx, method, url, NULL, NULL, 0);
    if (req_id == 0) {
        free(uc);
        return -1;
    }

    /* Register for response matching */
    pending_add(req_id, uc);

    return 0;
}

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
