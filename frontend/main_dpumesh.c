/*
 * frontend (DPUmesh version)
 *
 * Receives ingress requests via SHM (from TCP Bridge/DPU daemon).
 * Calls id_service and attend_service via SHM (dpumesh_send).
 * Receives responses via SHM (on_response callback).
 *
 * Two-stage state machine: WAITING_ID → WAITING_ATTEND → respond.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json_util.h"
#include "dpumesh.h"
#include "epoll_server_dpumesh.h"

static dpumesh_ctx_t *dpu_ctx;

static char id_service_name[64] = "id-service";
static char attend_service_name[64] = "attend-service";

/* ====== Per-request state ====== */

typedef enum {
    STAGE_WAITING_ID,
    STAGE_WAITING_ATTEND,
} frontend_stage_t;

#define MAX_PENDING 256

typedef struct {
    int              active;
    frontend_stage_t stage;
    char             name[64];
    int              id;
    char             orig_req_id_str[64];
    char             orig_source_worker[128];
    int              orig_case_flag;
    dpumesh_req_id   id_req_id;
    dpumesh_req_id   attend_req_id;
} pending_t;

static pending_t pending[MAX_PENDING];

static pending_t *alloc_pending(void) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!pending[i].active) {
            memset(&pending[i], 0, sizeof(pending_t));
            pending[i].active = 1;
            return &pending[i];
        }
    }
    return NULL;
}

static pending_t *find_pending_by_req_id(dpumesh_req_id rid) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (pending[i].active &&
            (pending[i].id_req_id == rid || pending[i].attend_req_id == rid))
            return &pending[i];
    }
    return NULL;
}

/* ====== Callbacks ====== */

static void on_request(const dpumesh_request_t *req, void *user_data) {
    (void)user_data;

    printf("[frontend] request: method=%s path=%s qs=%s\n",
           req->method ? req->method : "?",
           req->path ? req->path : "?",
           req->query_string ? req->query_string : "");

    /* Parse name from query string or body */
    char name[64] = "";
    if (req->query_string && strlen(req->query_string) > 0)
        query_get(req->query_string, "name", name, sizeof(name));

    if (name[0] == '\0' && req->body && req->body_len > 0) {
        /* Try to find name in body (JSON) */
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
        return;
    }

    /* Allocate pending state */
    pending_t *p = alloc_pending();
    if (!p) {
        const char *body = "{\"error\":\"too many pending requests\"}";
        dpumesh_respond(dpu_ctx, req, 503, body, strlen(body));
        return;
    }

    snprintf(p->name, sizeof(p->name), "%s", name);
    snprintf(p->orig_req_id_str, sizeof(p->orig_req_id_str), "%s", req->req_id_str);
    snprintf(p->orig_source_worker, sizeof(p->orig_source_worker), "%s", req->source_worker);
    p->orig_case_flag = req->_case_flag;
    p->stage = STAGE_WAITING_ID;

    /* Stage 1: call id_service via SHM */
    char url[256];
    /* Use k8s service namespace for DPU routing */
    const char *ns = getenv("NAMESPACE");
    if (!ns) ns = "mini-ms-dpumesh";
    snprintf(url, sizeof(url), "http://%s.%s.svc.cluster.local/id?name=%s",
             id_service_name, ns, name);

    p->id_req_id = dpumesh_send(dpu_ctx, "GET", url, NULL, NULL, 0);
    if (p->id_req_id == 0) {
        const char *body = "{\"error\":\"dpumesh_send failed\"}";
        dpumesh_respond(dpu_ctx, req, 502, body, strlen(body));
        p->active = 0;
        return;
    }

    printf("[frontend] sent id_service request (req_id=%u) for name=%s\n",
           p->id_req_id, name);
}

static void on_response(dpumesh_req_id req_id, const dpumesh_response_t *resp, void *ud) {
    (void)ud;

    pending_t *p = find_pending_by_req_id(req_id);
    if (!p) {
        printf("[frontend] unknown response req_id=%u\n", req_id);
        return;
    }

    if (p->stage == STAGE_WAITING_ID) {
        /* Parse {"id": N} from id_service response */
        if (!resp->body || resp->body_len == 0) {
            printf("[frontend] empty id_service response\n");
            /* Send error back - construct a fake request for dpumesh_respond */
            dpumesh_request_t fake_req;
            memset(&fake_req, 0, sizeof(fake_req));
            snprintf(fake_req.req_id_str, sizeof(fake_req.req_id_str), "%s", p->orig_req_id_str);
            snprintf(fake_req.source_worker, sizeof(fake_req.source_worker), "%s", p->orig_source_worker);
            const char *body = "{\"error\":\"id_service error\"}";
            dpumesh_respond(dpu_ctx, &fake_req, 502, body, strlen(body));
            p->active = 0;
            return;
        }

        const char *id_p = strstr(resp->body, "\"id\":");
        if (id_p) {
            p->id = atoi(id_p + 5);
        }

        /* Check for error response */
        if (strstr(resp->body, "\"error\"")) {
            dpumesh_request_t fake_req;
            memset(&fake_req, 0, sizeof(fake_req));
            snprintf(fake_req.req_id_str, sizeof(fake_req.req_id_str), "%s", p->orig_req_id_str);
            snprintf(fake_req.source_worker, sizeof(fake_req.source_worker), "%s", p->orig_source_worker);
            dpumesh_respond(dpu_ctx, &fake_req, resp->status_code, resp->body, resp->body_len);
            p->active = 0;
            return;
        }

        printf("[frontend] id_service returned id=%d for %s\n", p->id, p->name);

        /* Stage 2: call attend_service */
        p->stage = STAGE_WAITING_ATTEND;
        char url[256];
        const char *ns = getenv("NAMESPACE");
        if (!ns) ns = "mini-ms-dpumesh";
        snprintf(url, sizeof(url), "http://%s.%s.svc.cluster.local/attend?id=%d",
                 attend_service_name, ns, p->id);

        p->attend_req_id = dpumesh_send(dpu_ctx, "GET", url, NULL, NULL, 0);
        if (p->attend_req_id == 0) {
            dpumesh_request_t fake_req;
            memset(&fake_req, 0, sizeof(fake_req));
            snprintf(fake_req.req_id_str, sizeof(fake_req.req_id_str), "%s", p->orig_req_id_str);
            snprintf(fake_req.source_worker, sizeof(fake_req.source_worker), "%s", p->orig_source_worker);
            const char *body = "{\"error\":\"dpumesh_send failed\"}";
            dpumesh_respond(dpu_ctx, &fake_req, 502, body, strlen(body));
            p->active = 0;
        }

    } else if (p->stage == STAGE_WAITING_ATTEND) {
        /* Parse {"attended": true/false} */
        int attended = 0;
        if (resp->body && strstr(resp->body, "true"))
            attended = 1;

        /* Assemble final response */
        char body[256];
        int blen = snprintf(body, sizeof(body),
            "{\"name\":\"%s\",\"id\":%d,\"attended\":%s}",
            p->name, p->id, attended ? "true" : "false");

        printf("[frontend] result: %s\n", body);

        /* Send response back to original requester */
        dpumesh_request_t fake_req;
        memset(&fake_req, 0, sizeof(fake_req));
        snprintf(fake_req.req_id_str, sizeof(fake_req.req_id_str), "%s", p->orig_req_id_str);
        snprintf(fake_req.source_worker, sizeof(fake_req.source_worker), "%s", p->orig_source_worker);
        dpumesh_respond(dpu_ctx, &fake_req, 200, body, blen);

        p->active = 0;
    }
}

int main(void) {
    const char *app = getenv("APP");
    if (!app) app = "frontend";

    const char *env;
    if ((env = getenv("ID_SERVICE_NAME")))     snprintf(id_service_name, sizeof(id_service_name), "%s", env);
    if ((env = getenv("ATTEND_SERVICE_NAME"))) snprintf(attend_service_name, sizeof(attend_service_name), "%s", env);

    int worker_id = 0;
    if ((env = getenv("WORKER_ID"))) worker_id = atoi(env);

    if (dpumesh_init(&dpu_ctx, app, worker_id) != 0) {
        fprintf(stderr, "Failed to init dpumesh\n");
        return 1;
    }

    printf("[frontend] DPUmesh mode: id_service=%s attend_service=%s\n",
           id_service_name, attend_service_name);

    /* No TCP listen — ingress comes from DPU daemon TCP bridge */
    epoll_run_dpumesh(-1, NULL, dpu_ctx, on_response, NULL, on_request, NULL);

    dpumesh_destroy(dpu_ctx);
    return 0;
}
