#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json_util.h"
#include "http_parse.h"
#include "tcp_handler.h"
#include "dpumesh_handler.h"

static dpumesh_ctx_t *dpu_ctx;

static char id_service_name[64] = "id-service";
static char attend_service_name[64] = "attend-service";

/* Per-request state for the two-stage upstream call chain */
typedef enum {
    STAGE_WAITING_ID,
    STAGE_WAITING_ATTEND,
} frontend_stage_t;

typedef struct {
    frontend_stage_t stage;
    char name[64];
    int  id;
} frontend_ctx_t;

static void handle_upstream_response(conn_t *upstream, http_response_t *resp);

static void handle_request(conn_t *client, http_request_t *req) {
    if (strcmp(req->method, "GET") != 0 || strcmp(req->path, "/lookup") != 0) {
        dpumesh_conn_respond(client,404, "{\"error\":\"not found\"}", 20);
        return;
    }

    char name[64];
    if (query_get(req->query, "name", name, sizeof(name)) != 0) {
        dpumesh_conn_respond(client,400, "{\"error\":\"missing name\"}", 23);
        return;
    }

    /* Stage 1: call id_service */
    frontend_ctx_t *ctx = calloc(1, sizeof(frontend_ctx_t));
    if (!ctx) {
        dpumesh_conn_respond(client,500, "{\"error\":\"alloc failed\"}", 23);
        return;
    }
    ctx->stage = STAGE_WAITING_ID;
    snprintf(ctx->name, sizeof(ctx->name), "%s", name);

    char url[256];
    const char *ns = getenv("NAMESPACE");
    if (!ns) ns = "mini-ms-dpumesh";
    snprintf(url, sizeof(url), "http://%s.%s.svc.cluster.local/id?name=%s", id_service_name, ns, name);

    if (dpumesh_conn_upstream(client,"GET", url, ctx) < 0) {
        free(ctx);
        dpumesh_conn_respond(client,502, "{\"error\":\"upstream failed\"}", 26);
    }
}

static void handle_upstream_response(conn_t *upstream, http_response_t *resp) {
    conn_t *client = upstream->client_conn;
    frontend_ctx_t *ctx = upstream->handler_ctx;
    if (!client || !ctx) return;

    if (resp->status_code != 200) {
        dpumesh_conn_respond(client,resp->status_code, resp->body, resp->body_len);
        free(ctx);
        return;
    }

    if (ctx->stage == STAGE_WAITING_ID) {
        /* Parse {"id": N} from id_service response */
        const char *p = strstr(resp->body, "\"id\":");
        if (!p) {
            dpumesh_conn_respond(client,502, "{\"error\":\"bad id response\"}", 26);
            free(ctx);
            return;
        }
        ctx->id = atoi(p + 5);
        ctx->stage = STAGE_WAITING_ATTEND;

        /* Stage 2: call attend_service */
        char url[256];
        const char *ns = getenv("NAMESPACE");
        if (!ns) ns = "mini-ms-dpumesh";
        snprintf(url, sizeof(url), "http://%s.%s.svc.cluster.local/attend?id=%d", attend_service_name, ns, ctx->id);

        if (dpumesh_conn_upstream(client,"GET", url, ctx) < 0) {
            dpumesh_conn_respond(client,502, "{\"error\":\"upstream failed\"}", 26);
            free(ctx);
        }
    } else if (ctx->stage == STAGE_WAITING_ATTEND) {
        /* Parse {"attended": true/false} from attend_service response */
        int attended = (strstr(resp->body, "true") != NULL) ? 1 : 0;

        /* Assemble final response */
        char body[256];
        int blen = snprintf(body, sizeof(body),
            "{\"name\":\"%s\",\"id\":%d,\"attended\":%s}",
            ctx->name, ctx->id, attended ? "true" : "false");

        dpumesh_conn_respond(client,200, body, blen);
        free(ctx);
    }
}

int main(void) {
    const char *app = getenv("APP");
    if (!app) app = "frontend";

    const char *env;
    if ((env = getenv("ID_SERVICE_NAME")))  snprintf(id_service_name, sizeof(id_service_name), "%s", env);
    if ((env = getenv("ATTEND_SERVICE_NAME")))  snprintf(attend_service_name, sizeof(attend_service_name), "%s", env);

    int worker_id = 0;
    if ((env = getenv("WORKER_ID"))) worker_id = atoi(env);

    if (dpumesh_init(&dpu_ctx, app, worker_id) != 0) {
        fprintf(stderr, "Failed to init dpumesh\n");
        return 1;
    }

    printf("[frontend] DPUmesh mode: id_service=%s attend_service=%s\n",
           id_service_name, attend_service_name);

    dpumesh_handler_set_upstream(handle_upstream_response);
    dpumesh_run(dpu_ctx, handle_request);

    dpumesh_destroy(dpu_ctx);
    return 0;
}
