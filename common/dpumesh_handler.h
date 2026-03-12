#ifndef DPUMESH_HANDLER_H
#define DPUMESH_HANDLER_H

#include <event2/event.h>
#include "tcp_handler.h"
#include "dpumesh.h"

typedef struct dpu_conn {
    conn_t   base;
    int      dpu_desc_index;
} dpu_conn_t;

/*
 * Initialize dpumesh handler: create pipe notification, start poller thread.
 * Returns 0 on success, -1 on error.
 */
int  dpumesh_handler_init(struct event_base *base, dpumesh_ctx_t *dpu_ctx,
                          request_handler_fn handler);

/* Set upstream response handler for dpumesh connections */
void dpumesh_handler_set_upstream(upstream_handler_fn handler);

/* Convenience wrapper: init + event_base + dispatch + free (matches epoll_run pattern) */
void dpumesh_run(dpumesh_ctx_t *dpu_ctx, request_handler_fn handler);

/* Respond to a DPUmesh request via SHM. Frees the client conn. */
void dpumesh_conn_respond(conn_t *client,
                          int status, const char *body, size_t body_len);

/* Send upstream request via DPUmesh SHM. Returns 0 on success, -1 on error. */
int  dpumesh_conn_upstream(conn_t *client,
                           const char *method, const char *url,
                           void *handler_ctx);

#endif
