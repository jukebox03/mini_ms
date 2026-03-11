#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json_util.h"
#include "http_parse.h"
#include "tcp_handler.h"

static char id_host[256] = "127.0.0.1";
static int  id_port = 8081;
static char attend_host[256] = "127.0.0.1";
static int  attend_port = 8082;

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
        conn_start_response(client, 404, "{\"error\":\"not found\"}", 20);
        return;
    }

    char name[64];
    if (query_get(req->query, "name", name, sizeof(name)) != 0) {
        conn_start_response(client, 400, "{\"error\":\"missing name\"}", 23);
        return;
    }

    /* Stage 1: call id_service */
    frontend_ctx_t *ctx = calloc(1, sizeof(frontend_ctx_t));
    if (!ctx) {
        conn_start_response(client, 500, "{\"error\":\"alloc failed\"}", 23);
        return;
    }
    ctx->stage = STAGE_WAITING_ID;
    snprintf(ctx->name, sizeof(ctx->name), "%s", name);

    char path[256];
    snprintf(path, sizeof(path), "/id?name=%s", name);

    if (conn_start_upstream(client, id_host, id_port, "GET", path, ctx) < 0) {
        free(ctx);
        conn_start_response(client, 502, "{\"error\":\"upstream failed\"}", 26);
    }
}

static void handle_upstream_response(conn_t *upstream, http_response_t *resp) {
    conn_t *client = upstream->client_conn;
    frontend_ctx_t *ctx = upstream->handler_ctx;
    if (!client || !ctx) return;

    if (resp->status_code != 200) {
        conn_start_response(client, resp->status_code, resp->body, resp->body_len);
        free(ctx);
        return;
    }

    if (ctx->stage == STAGE_WAITING_ID) {
        /* Parse {"id": N} from id_service response */
        const char *p = strstr(resp->body, "\"id\":");
        if (!p) {
            conn_start_response(client, 502, "{\"error\":\"bad id response\"}", 26);
            free(ctx);
            return;
        }
        ctx->id = atoi(p + 5);
        ctx->stage = STAGE_WAITING_ATTEND;

        /* Stage 2: call attend_service */
        char path[256];
        snprintf(path, sizeof(path), "/attend?id=%d", ctx->id);

        if (conn_start_upstream(client, attend_host, attend_port, "GET", path, ctx) < 0) {
            conn_start_response(client, 502, "{\"error\":\"upstream failed\"}", 26);
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

        conn_start_response(client, 200, body, blen);
        free(ctx);
    }
}

int main(void) {
    const char *env;
    if ((env = getenv("ID_SERVICE_HOST")))  strncpy(id_host, env, sizeof(id_host) - 1);
    if ((env = getenv("ID_SERVICE_PORT")))  id_port = atoi(env);
    if ((env = getenv("ATTEND_SERVICE_HOST"))) strncpy(attend_host, env, sizeof(attend_host) - 1);
    if ((env = getenv("ATTEND_SERVICE_PORT"))) attend_port = atoi(env);

    printf("[frontend] id_service=%s:%d attend_service=%s:%d\n",
           id_host, id_port, attend_host, attend_port);

    tcp_handler_set_upstream(handle_upstream_response);

    int port = 8080;
    if ((env = getenv("PORT"))) port = atoi(env);

    int fd = make_listen_socket(port);
    if (fd < 0) return 1;

    epoll_run(fd, handle_request);
    return 0;
}
