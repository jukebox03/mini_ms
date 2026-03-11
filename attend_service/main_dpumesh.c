/*
 * attend_service (DPUmesh version) - per-request eventfd
 *
 * Nearly identical to TCP version (attend_service/main.c).
 * Only differences: dpumesh_conn_respond, dpumesh init, epoll_run_dpumesh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json_util.h"
#include "http_parse.h"
#include "tcp_handler.h"
#include "dpumesh_handler.h"

static kv_store_t store;
static dpumesh_ctx_t *dpu_ctx;

static void handle_request(conn_t *client, http_request_t *req) {
    if (strcmp(req->method, "GET") != 0 || strcmp(req->path, "/attend") != 0) {
        dpumesh_conn_respond(client,404, "{\"error\":\"not found\"}", 20);
        return;
    }

    char id_str[32];
    if (query_get(req->query, "id", id_str, sizeof(id_str)) != 0) {
        dpumesh_conn_respond(client,400, "{\"error\":\"missing id\"}", 21);
        return;
    }

    const int *attended = kv_store_get(&store, id_str);
    if (!attended) {
        dpumesh_conn_respond(client,404, "{\"error\":\"id not found\"}", 23);
        return;
    }

    char body[128];
    int blen = snprintf(body, sizeof(body), "{\"attended\":%s}", *attended ? "true" : "false");
    dpumesh_conn_respond(client,200, body, blen);
}

int main(void) {
    const char *data_file = getenv("DATA_FILE");
    if (!data_file) data_file = "data/attendance.json";

    if (kv_store_load(&store, data_file) != 0) {
        fprintf(stderr, "Failed to load %s\n", data_file);
        return 1;
    }
    printf("[attend_service] loaded %d entries from %s\n", store.count, data_file);

    const char *app = getenv("APP");
    if (!app) app = "attend-service";

    int worker_id = 0;
    const char *wid = getenv("WORKER_ID");
    if (wid) worker_id = atoi(wid);

    if (dpumesh_init(&dpu_ctx, app, worker_id) != 0) {
        fprintf(stderr, "Failed to init dpumesh\n");
        return 1;
    }

    dpumesh_run(dpu_ctx, handle_request);

    dpumesh_destroy(dpu_ctx);
    return 0;
}
