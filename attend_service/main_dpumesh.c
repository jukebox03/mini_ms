/*
 * attend_service (DPUmesh version)
 *
 * Receives requests via SHM RX SQ.
 * Sends responses via SHM TX SQ.
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

    const char *qs = req->query_string;
    char id_str[32] = "";

    if (qs && strlen(qs) > 0) {
        query_get(qs, "id", id_str, sizeof(id_str));
    }

    if (id_str[0] == '\0' && req->body && req->body_len > 0) {
        const char *ip = strstr(req->body, "\"id\":");
        if (ip) {
            ip += 5;
            while (*ip == ' ') ip++;
            int i = 0;
            while (*ip && *ip != ',' && *ip != '}' && *ip != '"' && i < 31)
                id_str[i++] = *ip++;
            id_str[i] = '\0';
        }
    }

    if (id_str[0] == '\0') {
        const char *body = "{\"error\":\"missing id\"}";
        dpumesh_respond(dpu_ctx, req, 400, body, strlen(body));
        printf("[attend_service] 400: missing id\n");
        return;
    }

    const int *attended = kv_store_get(&store, id_str);
    if (!attended) {
        const char *body = "{\"error\":\"id not found\"}";
        dpumesh_respond(dpu_ctx, req, 404, body, strlen(body));
        printf("[attend_service] 404: id=%s\n", id_str);
        return;
    }

    char body[128];
    int blen = snprintf(body, sizeof(body), "{\"attended\":%s}",
                        *attended ? "true" : "false");
    dpumesh_respond(dpu_ctx, req, 200, body, blen);
    printf("[attend_service] 200: id=%s -> attended=%s\n",
           id_str, *attended ? "true" : "false");
}

static void on_response(dpumesh_req_id req_id, const dpumesh_response_t *resp, void *ud) {
    (void)req_id; (void)resp; (void)ud;
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

    epoll_run_dpumesh(-1, NULL, dpu_ctx, on_response, NULL, on_request, NULL);

    dpumesh_destroy(dpu_ctx);
    return 0;
}
