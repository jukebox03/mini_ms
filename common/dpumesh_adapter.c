#include "dpumesh_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

/* ====== Internal state ====== */

static request_handler_fn  g_dpu_request_handler = NULL;
static upstream_handler_fn g_dpu_upstream_handler = NULL;
static struct event_base  *g_dpu_base = NULL;

/* ====== Thread-safe queue + pipe notification ====== */

#define DPU_QUEUE_SIZE 1024

static dpu_conn_t *g_queue[DPU_QUEUE_SIZE];
static int g_queue_head = 0, g_queue_tail = 0;
static pthread_mutex_t g_queue_lock = PTHREAD_MUTEX_INITIALIZER;

static int g_pipe_fd[2] = {-1, -1};

static void queue_push(dpu_conn_t *c) {
    pthread_mutex_lock(&g_queue_lock);
    g_queue[g_queue_tail] = c;
    g_queue_tail = (g_queue_tail + 1) % DPU_QUEUE_SIZE;
    pthread_mutex_unlock(&g_queue_lock);

    char b = 1;
    if (write(g_pipe_fd[1], &b, 1) < 0) { /* ignore */ }
}

static dpu_conn_t *queue_pop(void) {
    dpu_conn_t *c = NULL;
    pthread_mutex_lock(&g_queue_lock);
    if (g_queue_head != g_queue_tail) {
        c = g_queue[g_queue_head];
        g_queue_head = (g_queue_head + 1) % DPU_QUEUE_SIZE;
    }
    pthread_mutex_unlock(&g_queue_lock);
    return c;
}

/* ====== Pending upstream map (thread-safe) ====== */

#define MAX_PENDING_UPSTREAM 256

typedef struct {
    uint32_t    req_id;
    dpu_conn_t *conn;
} pending_entry_t;

static pending_entry_t g_pending[MAX_PENDING_UPSTREAM];
static pthread_mutex_t g_pending_lock = PTHREAD_MUTEX_INITIALIZER;

static void pending_add(uint32_t req_id, dpu_conn_t *conn) {
    pthread_mutex_lock(&g_pending_lock);
    for (int i = 0; i < MAX_PENDING_UPSTREAM; i++) {
        if (g_pending[i].conn == NULL) {
            g_pending[i].req_id = req_id;
            g_pending[i].conn = conn;
            break;
        }
    }
    pthread_mutex_unlock(&g_pending_lock);
}

static dpu_conn_t *pending_remove(uint32_t req_id) {
    dpu_conn_t *result = NULL;
    pthread_mutex_lock(&g_pending_lock);
    for (int i = 0; i < MAX_PENDING_UPSTREAM; i++) {
        if (g_pending[i].conn && g_pending[i].req_id == req_id) {
            result = g_pending[i].conn;
            g_pending[i].conn = NULL;
            g_pending[i].req_id = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_pending_lock);
    return result;
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
    /* fd is -1 for dpumesh conns, no socket to close */
    free(c);
}

/* ====== Poller callbacks (called from poller thread) ====== */

static void poller_on_response(dpumesh_req_id req_id,
                                const dpumesh_response_t *resp,
                                void *user_data) {
    (void)user_data;

    fprintf(stderr, "[poller] on_response req_id=%u status=%d\n",
            req_id, resp->status_code);

    dpu_conn_t *uc = pending_remove(req_id);
    if (!uc) {
        fprintf(stderr, "[poller] unknown response req_id=%u\n", req_id);
        return;
    }

    /* Format as HTTP response for http_parse_response() */
    int status = resp->status_code > 0 ? resp->status_code : 200;
    const char *body = resp->body ? resp->body : "";
    size_t body_len = resp->body_len;

    uc->base.rlen = snprintf(uc->base.rbuf, sizeof(uc->base.rbuf),
        "HTTP/1.1 %d OK\r\n"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%.*s",
        status, body_len, (int)body_len, body);

    uc->dpu_is_response = 1;
    queue_push(uc);
}

static void poller_on_request(const dpumesh_request_t *req, void *user_data) {
    (void)user_data;

    fprintf(stderr, "[poller] on_request method=%s path=%s qs=%s\n",
            req->method ? req->method : "?",
            req->path ? req->path : "?",
            req->query_string ? req->query_string : "");

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

    c->dpu_is_response = 0;
    queue_push(c);
}

/* ====== Pipe callback (main thread) ====== */

static void on_pipe_cb(evutil_socket_t fd, short what, void *arg) {
    (void)what; (void)arg;

    char buf[64];
    if (read(fd, buf, sizeof(buf)) < 0) { /* drain pipe */ }

    dpu_conn_t *c;
    while ((c = queue_pop()) != NULL) {
        if (c->dpu_is_response) {
            /* Upstream response */
            fprintf(stderr, "[main] DPU upstream response rlen=%d\n",
                    c->base.rlen);
            http_response_t resp;
            int ret = http_parse_response(&resp, c->base.rbuf, c->base.rlen);
            fprintf(stderr, "[main] resp parse_ret=%d status=%d\n",
                    ret, resp.status_code);
            if (ret == 0 && g_dpu_upstream_handler)
                g_dpu_upstream_handler(&c->base, &resp);
            dpu_conn_free(c);
        } else {
            /* Incoming request */
            fprintf(stderr, "[main] DPU request rlen=%d\n", c->base.rlen);
            http_request_t req;
            int ret = http_parse_request(&req, c->base.rbuf, c->base.rlen);
            fprintf(stderr, "[main] parse_ret=%d method=%s path=%s query=%s\n",
                    ret, req.method, req.path, req.query);
            if (ret == 0 && g_dpu_request_handler)
                g_dpu_request_handler(&c->base, &req);
            /* Don't free — handler owns it, will call dpumesh_conn_respond */
        }
    }
}

/* ====== Adaptive polling thread ====== */

typedef struct {
    dpumesh_ctx_t *dpu_ctx;
    volatile int   running;
} poller_ctx_t;

static poller_ctx_t g_poller;

static void *poller_thread_fn(void *arg) {
    poller_ctx_t *pc = arg;
    int idle_count = 0;

    printf("[poller] adaptive polling thread started\n");
    fflush(stdout);

    while (pc->running) {
        int count = dpumesh_poll(pc->dpu_ctx,
                                 poller_on_response, NULL,
                                 poller_on_request, NULL);
        if (count > 0) {
            idle_count = 0;
        } else {
            idle_count++;
            if (idle_count > 10000) {
                usleep(1000);           /* 1ms - deep idle */
            } else if (idle_count > 100) {
                usleep(100);            /* 100us - light idle */
            }
            /* else: busy poll (< 100 iterations) */
        }
    }

    return NULL;
}

/* ====== Public API ====== */

void dpumesh_conn_respond(conn_t *client, dpumesh_ctx_t *ctx,
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

    dpumesh_respond(ctx, &req, status, body, body_len);

    dpu_conn_free(dc);
}

int dpumesh_conn_upstream(conn_t *client, dpumesh_ctx_t *ctx,
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
    uint32_t req_id = dpumesh_send(ctx, method, url, NULL, NULL, 0);
    if (req_id == 0) {
        free(uc);
        return -1;
    }

    /* Register for polling thread to match response */
    pending_add(req_id, uc);

    return 0;
}

int dpumesh_adapter_init(struct event_base *base, dpumesh_ctx_t *dpu_ctx,
                         request_handler_fn handler) {
    g_dpu_base = base;
    g_dpu_request_handler = handler;

    /* Create notification pipe */
    if (pipe(g_pipe_fd) < 0) {
        perror("pipe");
        return -1;
    }

    /* Set read end non-blocking */
    int flags = fcntl(g_pipe_fd[0], F_GETFL, 0);
    fcntl(g_pipe_fd[0], F_SETFL, flags | O_NONBLOCK);

    /* Register pipe read end with event_base */
    struct event *ev = event_new(base, g_pipe_fd[0], EV_READ|EV_PERSIST,
                                  on_pipe_cb, NULL);
    event_add(ev, NULL);

    /* Start poller thread */
    g_poller.dpu_ctx = dpu_ctx;
    g_poller.running = 1;

    pthread_t poller_tid;
    if (pthread_create(&poller_tid, NULL, poller_thread_fn, &g_poller) != 0) {
        perror("pthread_create");
        return -1;
    }

    printf("[server] dpumesh adapter initialized (with polling thread)\n");
    fflush(stdout);
    return 0;
}

void dpumesh_adapter_set_upstream_handler(upstream_handler_fn handler) {
    g_dpu_upstream_handler = handler;
}
