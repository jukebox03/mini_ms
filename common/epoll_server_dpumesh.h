/*
 * epoll_server_dpumesh.h - epoll server with DPUmesh notify_fd integrated
 *
 * Difference from epoll_server.h:
 *   - Adds CONN_DPUMESH_NOTIFY state
 *   - epoll_run_dpumesh() registers notify_fd and dispatches to dpumesh_poll()
 */

#ifndef EPOLL_SERVER_DPUMESH_H
#define EPOLL_SERVER_DPUMESH_H

#include "epoll_server.h"
#include "dpumesh.h"

/*
 * Start epoll loop with DPUmesh integrated.
 * - listen_fd: TCP listen socket for external HTTP (can be -1 to disable)
 * - handler: HTTP request handler (for TCP clients)
 * - dpu_ctx: DPUmesh context
 * - on_resp: called when DPUmesh response arrives
 * - resp_data: user data for on_resp
 * - on_req: called when DPUmesh request arrives
 * - req_data: user data for on_req
 */
void epoll_run_dpumesh(int listen_fd, request_handler_fn handler,
                       dpumesh_ctx_t *dpu_ctx,
                       dpumesh_on_response_fn on_resp, void *resp_data,
                       dpumesh_on_request_fn on_req, void *req_data);

#endif
