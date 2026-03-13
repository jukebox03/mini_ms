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
    int         _desc_index;
} dpumesh_request_t;

/* Inbound routing metadata (stored in library, indexed by _desc_index) */
typedef struct {
    uint32_t req_id;
    char     req_id_str[64];
    char     source_worker[128];
    int      case_flag;
    int      active;
} dpumesh_inbound_t;

/* ====== Write Message Type ====== */
#define DPUMESH_MSG_REQUEST   0
#define DPUMESH_MSG_RESPONSE  1

typedef struct {
    int          type;           /* DPUMESH_MSG_REQUEST or DPUMESH_MSG_RESPONSE */
    const char  *method;         /* request only */
    const char  *url;            /* request only */
    int          status_code;    /* response only */
    const dpumesh_request_t *orig_req;  /* response only (req_id, source_worker, case_flag) */
    const void  *body;
    size_t       body_len;
} dpumesh_msg_t;

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
 * Returns a notify fd (pipe read end) signaled by the internal poller thread.
 * Register with epoll/libevent; when readable, call dpumesh_poll().
 */
int dpumesh_get_notify_fd(dpumesh_ctx_t *ctx);

/*
 * Read requests from notify pipe (responses go to per-request fds).
 * Returns number of descriptors processed.
 */
int dpumesh_read(dpumesh_ctx_t *ctx,
                 dpumesh_on_request_fn on_req, void *req_data);

/*
 * Read a single response from a per-request fd.
 * response_fd is closed internally.
 * Returns 0 on success, -1 on failure.
 */
int dpumesh_read_response(dpumesh_ctx_t *ctx, int response_fd,
                          dpumesh_response_t *out_resp);

/* ====== Non-blocking Write ====== */

/*
 * Write a request or response to SHM.
 * msg->type determines behavior:
 *   DPUMESH_MSG_REQUEST:  sends to upstream service,
 *                         creates per-request pipe, returns read-end in *out_response_fd
 *   DPUMESH_MSG_RESPONSE: sends response back using msg->orig_req metadata
 *                         (out_response_fd ignored, pass NULL)
 * Returns 0 on success, -1 on failure.
 */
int dpumesh_write(dpumesh_ctx_t *ctx, const dpumesh_msg_t *msg,
                  int *out_response_fd);

/* ====== Buffer Management ====== */
void dpumesh_request_free(dpumesh_ctx_t *ctx, const dpumesh_request_t *req);
void dpumesh_response_free(dpumesh_ctx_t *ctx, const dpumesh_response_t *resp);

/* ====== Utility ====== */
int dpumesh_get_pod_id(dpumesh_ctx_t *ctx);
const char *dpumesh_get_worker_id(dpumesh_ctx_t *ctx);

#endif
