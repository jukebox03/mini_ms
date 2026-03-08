/*
 * dpumesh.h - DPUmesh C API
 *
 * Binary-compatible with Python dpumesh common.py SHM structures.
 * Uses same /dev/shm layout: BufferPool, DescriptorRing, PodRegistry.
 */

#ifndef DPUMESH_H
#define DPUMESH_H

#include <stdint.h>
#include <stddef.h>

/* ====== Constants (must match common.py) ====== */
#define DPUMESH_SLOT_SIZE       (1024 * 1024)   /* 1MB */
#define DPUMESH_NUM_SLOTS       64
#define DPUMESH_DESCRIPTOR_SIZE 64
#define DPUMESH_MAX_DESCRIPTORS 512
#define DPUMESH_SHM_PREFIX_DEFAULT "dpumesh"
#define DPUMESH_MAX_PENDING     256

/* Flags (match Python CaseFlag, OpFlag) */
#define CASE_EXTERNAL  1
#define CASE_INGRESS   2
#define CASE_LOCAL     3
#define OP_REQUEST     0x00
#define OP_RESPONSE    0x10

/* PoolType (match Python PoolType) */
#define POOL_NONE           0
#define POOL_HOST_TX_HEADER 1
#define POOL_HOST_TX_BODY   2
#define POOL_HOST_RX_HEADER 3
#define POOL_HOST_RX_BODY   4
#define POOL_DPU_TX_HEADER  5
#define POOL_DPU_TX_BODY    6
#define POOL_DPU_RX_BODY    7

/* ====== SwDescriptor (64 bytes, packed, matches '<iIiIIIiibbBBiiii12x') ====== */
typedef struct __attribute__((packed)) {
    int32_t  header_buf_slot;       /* i */
    uint32_t header_len;            /* I */
    int32_t  body_buf_slot;         /* i */
    uint32_t body_len;              /* I */
    uint32_t req_id;                /* I */
    uint32_t step_id;               /* I */
    int32_t  dst_pod_id;            /* i */
    int32_t  src_pod_id;            /* i */
    int8_t   flags;                 /* b */
    int8_t   valid;                 /* b */
    uint8_t  src_body_pool_type;    /* B */
    uint8_t  src_header_pool_type;  /* B */
    int32_t  src_body_pod_id;       /* i */
    int32_t  src_header_pod_id;     /* i */
    int32_t  src_body_buf_slot;     /* i */
    int32_t  src_header_buf_slot;   /* i */
    uint8_t  _pad[12];             /* 12x */
} sw_descriptor_t;

/* ====== Public API Types ====== */
typedef struct dpumesh_ctx dpumesh_ctx_t;
typedef uint32_t dpumesh_req_id;

typedef struct {
    int         status_code;
    const char *body;
    size_t      body_len;
    char        req_id_str[64];
    char        source_worker[128];
    /* internal: for buffer free */
    int         _header_slot;
    int         _body_slot;
} dpumesh_response_t;

typedef struct {
    const char *method;
    const char *path;
    const char *body;
    size_t      body_len;
    const char *query_string;
    char        req_id_str[64];
    char        source_worker[128];
    char        dest_worker[128];
    dpumesh_req_id req_id;
    int         src_pod_id;
    int         _header_slot;
    int         _body_slot;
    int         _case_flag;
} dpumesh_request_t;

/* Callbacks */
typedef void (*dpumesh_on_response_fn)(dpumesh_req_id req_id,
                                       const dpumesh_response_t *resp,
                                       void *user_data);
typedef void (*dpumesh_on_request_fn)(const dpumesh_request_t *req,
                                      void *user_data);

/* ====== Lifecycle ====== */
int  dpumesh_init(dpumesh_ctx_t **ctx, const char *app_name, int worker_id);
void dpumesh_destroy(dpumesh_ctx_t *ctx);

/* ====== Event Loop Integration ====== */

/*
 * Returns a timerfd that fires periodically.
 * Register with epoll; when readable, call dpumesh_poll().
 */
int dpumesh_get_notify_fd(dpumesh_ctx_t *ctx);

/*
 * Check RX SQ and invoke callbacks.
 * Returns number of descriptors processed.
 */
int dpumesh_poll(dpumesh_ctx_t *ctx,
                 dpumesh_on_response_fn on_resp, void *resp_data,
                 dpumesh_on_request_fn  on_req,  void *req_data);

/* ====== Non-blocking Send ====== */

/*
 * Send request to another service via SHM.
 * url format: "http://service_name/path?query"
 * Returns req_id > 0 on success, 0 on failure.
 */
dpumesh_req_id dpumesh_send(dpumesh_ctx_t *ctx,
                            const char *method,
                            const char *url,
                            const char *headers_json,
                            const void *body,
                            size_t body_len);

/*
 * Send response to an ingress request.
 */
int dpumesh_respond(dpumesh_ctx_t *ctx,
                    const dpumesh_request_t *req,
                    int status_code,
                    const char *body,
                    size_t body_len);

/* ====== Buffer Management ====== */
void dpumesh_request_free(dpumesh_ctx_t *ctx, const dpumesh_request_t *req);
void dpumesh_response_free(dpumesh_ctx_t *ctx, const dpumesh_response_t *resp);

/* ====== Utility ====== */
int dpumesh_get_pod_id(dpumesh_ctx_t *ctx);
const char *dpumesh_get_worker_id(dpumesh_ctx_t *ctx);

#endif
