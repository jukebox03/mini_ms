#ifndef DPUMESH_ADAPTER_H
#define DPUMESH_ADAPTER_H

#include <event2/event.h>
#include "tcp_handler.h"
#include "dpumesh.h"

typedef struct dpu_conn {
    conn_t   base;
    char     dpu_req_id_str[64];
    char     dpu_source_worker[128];
    int      dpu_case_flag;
    uint32_t dpu_req_id;
    int      dpu_is_response;
} dpu_conn_t;

/*
 * Initialize dpumesh adapter: create pipe notification, start poller thread.
 * Returns 0 on success, -1 on error.
 */
int  dpumesh_adapter_init(struct event_base *base, dpumesh_ctx_t *dpu_ctx,
                          request_handler_fn handler);

/* Set upstream response handler for dpumesh connections */
void dpumesh_adapter_set_upstream_handler(upstream_handler_fn handler);

/* Respond to a DPUmesh request via SHM. Frees the client conn. */
void dpumesh_conn_respond(conn_t *client, dpumesh_ctx_t *ctx,
                          int status, const char *body, size_t body_len);

/* Send upstream request via DPUmesh SHM. Returns 0 on success, -1 on error. */
int  dpumesh_conn_upstream(conn_t *client, dpumesh_ctx_t *ctx,
                           const char *method, const char *url,
                           void *handler_ctx);

#endif
