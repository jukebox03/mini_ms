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
    if (strcmp(req->method, "GET") != 0 || strcmp(req->path, "/id") != 0) {
        dpumesh_conn_respond(client,404, "{\"error\":\"not found\"}", 20);
        return;
    }

    char name[64];
    if (query_get(req->query, "name", name, sizeof(name)) != 0) {
        dpumesh_conn_respond(client,400, "{\"error\":\"missing name\"}", 23);
        return;
    }

    const int *id = kv_store_get(&store, name);
    if (!id) {
        dpumesh_conn_respond(client,404, "{\"error\":\"name not found\"}", 26);
        return;
    }

    char body[128];
    int blen = snprintf(body, sizeof(body), "{\"id\":%d}", *id);
    dpumesh_conn_respond(client,200, body, blen);
}

int main(void) {
    const char *data_file = getenv("DATA_FILE");
    if (!data_file) data_file = "data/ids.json";

    if (kv_store_load(&store, data_file) != 0) {
        fprintf(stderr, "Failed to load %s\n", data_file);
        return 1;
    }
    printf("[id_service] loaded %d entries from %s\n", store.count, data_file);

    const char *app = getenv("APP");
    if (!app) app = "id-service";

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
