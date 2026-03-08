/*
 * id_service (DPUmesh version)
 *
 * Receives requests via SHM RX SQ instead of TCP.
 * Sends responses via SHM TX SQ.
 * No TCP listen socket needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json_util.h"
#include "dpumesh.h"
#include "epoll_server_dpumesh.h"

static kv_store_t store;
static dpumesh_ctx_t *dpu_ctx;

static void on_request(const dpumesh_request_t *req, void *user_data) {
    (void)user_data;

    /* Parse query string from path or query_string field */
    const char *qs = req->query_string;

    /* Also check body for query params (Case2: body may contain the request) */
    char name[64] = "";

    /* Try query_string first */
    if (qs && strlen(qs) > 0) {
        query_get(qs, "name", name, sizeof(name));
    }

    /* If not found, try parsing from body */
    if (name[0] == '\0' && req->body && req->body_len > 0) {
        /* Body might contain JSON with name field */
        const char *np = strstr(req->body, "\"name\":");
        if (np) {
            np += 7;
            while (*np == ' ' || *np == '"') np++;
            int i = 0;
            while (*np && *np != '"' && *np != ',' && *np != '}' && i < 63)
                name[i++] = *np++;
            name[i] = '\0';
        }
    }

    if (name[0] == '\0') {
        const char *body = "{\"error\":\"missing name\"}";
        dpumesh_respond(dpu_ctx, req, 400, body, strlen(body));
        printf("[id_service] 400: missing name\n");
        return;
    }

    const int *id = kv_store_get(&store, name);
    if (!id) {
        const char *body = "{\"error\":\"name not found\"}";
        dpumesh_respond(dpu_ctx, req, 404, body, strlen(body));
        printf("[id_service] 404: %s\n", name);
        return;
    }

    char body[128];
    int blen = snprintf(body, sizeof(body), "{\"id\":%d}", *id);
    dpumesh_respond(dpu_ctx, req, 200, body, blen);
    printf("[id_service] 200: %s -> id=%d\n", name, *id);
}

static void on_response(dpumesh_req_id req_id, const dpumesh_response_t *resp, void *ud) {
    (void)req_id; (void)resp; (void)ud;
    /* id_service doesn't make outgoing requests, so this shouldn't be called */
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

    /* No TCP listen socket — all communication via SHM */
    epoll_run_dpumesh(-1, NULL, dpu_ctx, on_response, NULL, on_request, NULL);

    dpumesh_destroy(dpu_ctx);
    return 0;
}
