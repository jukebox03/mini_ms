/*
 * dpumesh.c - DPUmesh C API implementation
 *
 * Binary-compatible with Python dpumesh/common.py SHM layout.
 */

#define _GNU_SOURCE
#include "dpumesh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <pthread.h>

/* Runtime SHM prefix (configurable via SHM_PREFIX env var) */
static const char *shm_prefix(void) {
    static const char *cached = NULL;
    if (!cached) {
        cached = getenv("SHM_PREFIX");
        if (!cached) cached = DPUMESH_SHM_PREFIX_DEFAULT;
    }
    return cached;
}

/* ====================================================================
 * BufferPool - matches Python BufferPool layout exactly
 * Layout: [bitmap: num_slots bytes] [data: num_slots * slot_size bytes]
 * ==================================================================== */

typedef struct {
    char    name[128];
    char    shm_path[256];
    char    lock_path[256];
    int     num_slots;
    int     slot_size;
    size_t  total_size;
    int     shm_fd;
    void   *mm;
} buffer_pool_t;

static int bp_init(buffer_pool_t *bp, const char *name, int create) {
    memset(bp, 0, sizeof(*bp));
    snprintf(bp->name, sizeof(bp->name), "%s", name);
    snprintf(bp->shm_path, sizeof(bp->shm_path), "/dev/shm/%s_%s",
             shm_prefix(), name);
    snprintf(bp->lock_path, sizeof(bp->lock_path), "%s.lock", bp->shm_path);
    bp->num_slots = DPUMESH_NUM_SLOTS;
    bp->slot_size = DPUMESH_SLOT_SIZE;
    bp->total_size = bp->num_slots + ((size_t)bp->num_slots * bp->slot_size);
    bp->shm_fd = -1;
    bp->mm = MAP_FAILED;

    if (create) {
        int fd = open(bp->shm_path, O_RDWR | O_CREAT, 0666);
        if (fd < 0) { perror("bp_init create"); return -1; }
        fchmod(fd, 0666);
        if (ftruncate(fd, bp->total_size) < 0) { close(fd); return -1; }
        void *m = mmap(NULL, bp->total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) { close(fd); return -1; }
        memset(m, 0, bp->num_slots); /* clear bitmap */
        msync(m, bp->num_slots, MS_SYNC);
        munmap(m, bp->total_size);
        close(fd);
        /* create lock file */
        int lf = open(bp->lock_path, O_CREAT | O_WRONLY, 0666);
        if (lf >= 0) { fchmod(lf, 0666); close(lf); }
    }

    /* open for use */
    bp->shm_fd = open(bp->shm_path, O_RDWR);
    if (bp->shm_fd < 0) return -1;
    bp->mm = mmap(NULL, bp->total_size, PROT_READ | PROT_WRITE, MAP_SHARED, bp->shm_fd, 0);
    if (bp->mm == MAP_FAILED) { close(bp->shm_fd); bp->shm_fd = -1; return -1; }
    return 0;
}

static void bp_destroy(buffer_pool_t *bp) {
    if (bp->mm != MAP_FAILED) munmap(bp->mm, bp->total_size);
    if (bp->shm_fd >= 0) close(bp->shm_fd);
}

static int bp_flock(buffer_pool_t *bp) {
    int fd = open(bp->lock_path, O_RDWR);
    if (fd < 0) return -1;
    flock(fd, LOCK_EX);
    return fd;
}

static void bp_funlock(int lock_fd) {
    if (lock_fd >= 0) { flock(lock_fd, LOCK_UN); close(lock_fd); }
}

static int bp_alloc(buffer_pool_t *bp) {
    int lk = bp_flock(bp);
    uint8_t *bitmap = (uint8_t *)bp->mm;
    for (int i = 0; i < bp->num_slots; i++) {
        if (bitmap[i] == 0) {
            bitmap[i] = 1;
            msync(bitmap + i, 1, MS_SYNC);
            bp_funlock(lk);
            return i;
        }
    }
    bp_funlock(lk);
    return -1;
}

static void bp_free(buffer_pool_t *bp, int slot) {
    if (slot < 0 || slot >= bp->num_slots) return;
    int lk = bp_flock(bp);
    uint8_t *bitmap = (uint8_t *)bp->mm;
    bitmap[slot] = 0;
    msync(bitmap + slot, 1, MS_SYNC);
    bp_funlock(lk);
}

static int bp_write(buffer_pool_t *bp, int slot, const void *data, size_t len) {
    if (slot < 0 || slot >= bp->num_slots) return -1;
    if ((int)len > bp->slot_size) return -1;
    size_t offset = bp->num_slots + ((size_t)slot * bp->slot_size);
    memcpy((char *)bp->mm + offset, data, len);
    msync((char *)bp->mm + offset, len, MS_SYNC);
    return (int)len;
}

static int bp_read(buffer_pool_t *bp, int slot, void *buf, size_t maxlen) {
    if (slot < 0 || slot >= bp->num_slots) return 0;
    size_t offset = bp->num_slots + ((size_t)slot * bp->slot_size);
    size_t rlen = maxlen < (size_t)bp->slot_size ? maxlen : (size_t)bp->slot_size;
    memcpy(buf, (char *)bp->mm + offset, rlen);
    return (int)rlen;
}


/* ====================================================================
 * DescriptorRing - matches Python DescriptorRing layout
 * Layout: [header: 12 bytes (head,tail,count as uint32)] [descs: N*64]
 * ==================================================================== */

typedef struct {
    char    name[128];
    char    shm_path[256];
    char    lock_path[256];
    int     max_descs;
    size_t  total_size;
    int     shm_fd;
    void   *mm;
} desc_ring_t;

static int dr_init(desc_ring_t *dr, const char *name, int create) {
    memset(dr, 0, sizeof(*dr));
    snprintf(dr->name, sizeof(dr->name), "%s", name);
    snprintf(dr->shm_path, sizeof(dr->shm_path), "/dev/shm/%s_%s",
             shm_prefix(), name);
    snprintf(dr->lock_path, sizeof(dr->lock_path), "%s.lock", dr->shm_path);
    dr->max_descs = DPUMESH_MAX_DESCRIPTORS;
    dr->total_size = 12 + (dr->max_descs * DPUMESH_DESCRIPTOR_SIZE);
    dr->shm_fd = -1;
    dr->mm = MAP_FAILED;

    if (create) {
        int fd = open(dr->shm_path, O_RDWR | O_CREAT, 0666);
        if (fd < 0) return -1;
        fchmod(fd, 0666);
        if (ftruncate(fd, dr->total_size) < 0) { close(fd); return -1; }
        void *m = mmap(NULL, dr->total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) { close(fd); return -1; }
        memset(m, 0, 12); /* head=0, tail=0, count=0 */
        msync(m, 12, MS_SYNC);
        munmap(m, dr->total_size);
        close(fd);
        int lf = open(dr->lock_path, O_CREAT | O_WRONLY, 0666);
        if (lf >= 0) { fchmod(lf, 0666); close(lf); }
    }

    dr->shm_fd = open(dr->shm_path, O_RDWR);
    if (dr->shm_fd < 0) return -1;
    dr->mm = mmap(NULL, dr->total_size, PROT_READ | PROT_WRITE, MAP_SHARED, dr->shm_fd, 0);
    if (dr->mm == MAP_FAILED) { close(dr->shm_fd); dr->shm_fd = -1; return -1; }
    return 0;
}

static void dr_destroy(desc_ring_t *dr) {
    if (dr->mm != MAP_FAILED) munmap(dr->mm, dr->total_size);
    if (dr->shm_fd >= 0) close(dr->shm_fd);
}

static int dr_flock(desc_ring_t *dr) {
    int fd = open(dr->lock_path, O_RDWR);
    if (fd < 0) return -1;
    flock(fd, LOCK_EX);
    return fd;
}

static int dr_put(desc_ring_t *dr, const sw_descriptor_t *desc) {
    int lk = dr_flock(dr);
    uint32_t *hdr = (uint32_t *)dr->mm;
    uint32_t head = hdr[0], tail = hdr[1], count = hdr[2];
    if (count >= (uint32_t)dr->max_descs) {
        bp_funlock(lk);
        return -1;
    }
    size_t offset = 12 + (tail * DPUMESH_DESCRIPTOR_SIZE);
    memcpy((char *)dr->mm + offset, desc, DPUMESH_DESCRIPTOR_SIZE);
    hdr[0] = head;
    hdr[1] = (tail + 1) % dr->max_descs;
    hdr[2] = count + 1;
    msync(dr->mm, dr->total_size, MS_SYNC);
    bp_funlock(lk);
    return 0;
}

static int dr_get(desc_ring_t *dr, sw_descriptor_t *desc) {
    int lk = dr_flock(dr);
    uint32_t *hdr = (uint32_t *)dr->mm;
    uint32_t head = hdr[0], tail = hdr[1], count = hdr[2];
    if (count == 0) {
        bp_funlock(lk);
        return -1;
    }
    size_t offset = 12 + (head * DPUMESH_DESCRIPTOR_SIZE);
    memcpy(desc, (char *)dr->mm + offset, DPUMESH_DESCRIPTOR_SIZE);
    hdr[0] = (head + 1) % dr->max_descs;
    hdr[1] = tail;
    hdr[2] = count - 1;
    msync(dr->mm, 12, MS_SYNC);
    bp_funlock(lk);
    return 0;
}


/* ====================================================================
 * PodRegistry - JSON file with flock (matches Python PodRegistry)
 * ==================================================================== */

/* These are now functions to support runtime SHM_PREFIX */
static const char *registry_path(void) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "/dev/shm/%s_pod_registry", shm_prefix());
    return buf;
}
static const char *registry_lock(void) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "/dev/shm/%s_pod_registry.lock", shm_prefix());
    return buf;
}
static const char *pod_counter_path(void) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "/dev/shm/%s_pod_id_counter", shm_prefix());
    return buf;
}
static const char *pod_counter_lock(void) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "/dev/shm/%s_pod_id_counter.lock", shm_prefix());
    return buf;
}

static int pod_registry_alloc_id(void) {
    /* Create lock file if needed */
    int lf = open(pod_counter_lock(), O_CREAT | O_RDWR, 0666);
    if (lf < 0) return -1;
    fchmod(lf, 0666);
    flock(lf, LOCK_EX);

    int new_id = 1;
    FILE *f = fopen(pod_counter_path(), "r");
    if (f) {
        int cur = 0;
        if (fscanf(f, "%d", &cur) == 1)
            new_id = cur + 1;
        fclose(f);
    }
    f = fopen(pod_counter_path(), "w");
    if (f) { fprintf(f, "%d", new_id); fclose(f); }
    chmod(pod_counter_path(), 0666);

    flock(lf, LOCK_UN);
    close(lf);
    return new_id;
}

static int pod_registry_register(const char *worker_name, int pod_id, const char *service) {
    int lf = open(registry_lock(), O_CREAT | O_RDWR, 0666);
    if (lf < 0) return -1;
    fchmod(lf, 0666);
    flock(lf, LOCK_EX);

    /* Read existing JSON */
    char buf[8192] = "{}";
    FILE *f = fopen(registry_path(), "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
    }

    /* Remove trailing '}', append new entry */
    char *end = strrchr(buf, '}');
    if (!end) end = buf + strlen(buf);

    /* Check if there are existing entries */
    int has_entries = 0;
    for (char *p = buf; p < end; p++) {
        if (*p == ':') { has_entries = 1; break; }
    }

    char newbuf[8192];
    *end = '\0';
    snprintf(newbuf, sizeof(newbuf),
        "%s%s\"%s\": {\"pod_id\": %d, \"service\": \"%s\"}}",
        buf, has_entries ? ", " : "", worker_name, pod_id, service);

    f = fopen(registry_path(), "w");
    if (f) { fputs(newbuf, f); fclose(f); }
    chmod(registry_path(), 0666);

    flock(lf, LOCK_UN);
    close(lf);
    return 0;
}


/* ====================================================================
 * Header serialization (produces JSON matching Python serialize_header)
 * ==================================================================== */

static int serialize_header(char *buf, size_t bufsz,
                            const char *method, const char *url,
                            const char *path, const char *query_string,
                            int status_code, const char *req_id_str,
                            const char *source_worker, const char *dest_worker) {
    return snprintf(buf, bufsz,
        "{\"method\": \"%s\", \"url\": \"%s\", \"path\": \"%s\", "
        "\"headers\": {}, \"query_string\": \"%s\", \"remote_addr\": \"\", "
        "\"status_code\": %d, \"req_id_str\": \"%s\", "
        "\"source_worker\": \"%s\", \"dest_worker\": \"%s\"}",
        method ? method : "",
        url ? url : "",
        path ? path : "",
        query_string ? query_string : "",
        status_code,
        req_id_str ? req_id_str : "",
        source_worker ? source_worker : "",
        dest_worker ? dest_worker : "");
}

/* Simple JSON value extraction: find "key": "value" or "key": number */
static int json_get_str(const char *json, const char *key, char *out, size_t outsz) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < outsz - 1) {
            out[i++] = *p++;
        }
        out[i] = '\0';
        return 0;
    }
    return -1;
}

static int json_get_int(const char *json, const char *key) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ') p++;
    return atoi(p);
}

/* Extract body from Case2 combined format: {"header": {...}, "body": "..."} */
static int parse_case2_body(const char *combined, size_t combined_len __attribute__((unused)),
                            char *header_json, size_t header_sz,
                            char *body_out, size_t body_sz) {
    /* Find "header": { ... } */
    const char *hp = strstr(combined, "\"header\":");
    if (!hp) return -1;
    hp += 9;
    while (*hp == ' ') hp++;
    if (*hp != '{') return -1;

    /* Find matching closing brace */
    int depth = 1;
    const char *hstart = hp;
    hp++;
    while (*hp && depth > 0) {
        if (*hp == '{') depth++;
        else if (*hp == '}') depth--;
        hp++;
    }
    size_t hlen = hp - hstart;
    if (hlen >= header_sz) hlen = header_sz - 1;
    memcpy(header_json, hstart, hlen);
    header_json[hlen] = '\0';

    /* Find "body": "..." */
    const char *bp = strstr(combined, "\"body\":");
    if (!bp) { body_out[0] = '\0'; return 0; }
    bp += 7;
    while (*bp == ' ') bp++;
    if (*bp == '"') {
        bp++;
        size_t i = 0;
        while (*bp && i < body_sz - 1) {
            if (*bp == '"' && (i == 0 || *(bp-1) != '\\')) break;
            if (*bp == '\\' && *(bp+1) == '"') {
                body_out[i++] = '"';
                bp += 2;
            } else {
                body_out[i++] = *bp++;
            }
        }
        body_out[i] = '\0';
    } else {
        body_out[0] = '\0';
    }
    return 0;
}


/* ====================================================================
 * dpumesh_ctx - internal state
 * ==================================================================== */

struct dpumesh_ctx {
    char        app_name[64];
    char        worker_id[128];
    int         pod_id;
    int         notify_fd;          /* pipe read end — register with event loop */
    int         notify_write_fd;    /* pipe write end — used by poller thread */
    pthread_t   poller_tid;
    volatile int poller_running;
    uint32_t    next_req_id;
    int         response_pipe_fds[DPUMESH_MAX_PENDING];  /* write-end, index = req_id % MAX */
    dpumesh_inbound_t inbound[DPUMESH_MAX_PENDING];     /* routing metadata, index = req_id % MAX */

    /* Host buffer pools (per-service, shared) */
    buffer_pool_t tx_header;
    buffer_pool_t tx_body;
    buffer_pool_t rx_header;
    buffer_pool_t rx_body;

    /* Worker SQs (per-worker) */
    desc_ring_t   tx_sq;
    desc_ring_t   rx_sq;
};


/* ====================================================================
 * Poller thread — polls SHM ring, signals notify pipe when data arrives
 * ==================================================================== */

static void *dpumesh_poller_fn(void *arg) {
    dpumesh_ctx_t *ctx = arg;
    int idle_count = 0;

    while (ctx->poller_running) {
        sw_descriptor_t desc;
        if (dr_get(&ctx->rx_sq, &desc) == 0) {
            if (desc.valid != 1) continue;
            idle_count = 0;

            int is_response = (desc.flags & 0xF0) == OP_RESPONSE;
            if (is_response) {
                /* per-request pipe: send descriptor to waiting caller */
                int idx = desc.req_id % DPUMESH_MAX_PENDING;
                int wfd = ctx->response_pipe_fds[idx];
                if (wfd >= 0) {
                    write(wfd, &desc, sizeof(desc));
                    close(wfd);
                    ctx->response_pipe_fds[idx] = -1;
                }
            } else {
                /* main notify pipe: send descriptor for request handling */
                write(ctx->notify_write_fd, &desc, sizeof(desc));
            }
        } else {
            idle_count++;
            if (idle_count > 10000)
                usleep(1000);
            else if (idle_count > 100)
                usleep(100);
        }
    }

    return NULL;
}


/* ====================================================================
 * Public API Implementation
 * ==================================================================== */

int dpumesh_init(dpumesh_ctx_t **out, const char *app_name, int worker_num) {
    dpumesh_ctx_t *ctx = calloc(1, sizeof(dpumesh_ctx_t));
    if (!ctx) return -1;

    snprintf(ctx->app_name, sizeof(ctx->app_name), "%s", app_name);
    ctx->next_req_id = 1;

    /* Allocate pod_id */
    ctx->pod_id = pod_registry_alloc_id();
    if (ctx->pod_id < 0) {
        free(ctx); return -1;
    }

    snprintf(ctx->worker_id, sizeof(ctx->worker_id),
             "%s-worker-%d", app_name, worker_num);

    /* Register with PodRegistry */
    pod_registry_register(ctx->worker_id, ctx->pod_id, app_name);

    /* Create service buffer pools */
    char name[128];
    snprintf(name, sizeof(name), "%s_tx_header", app_name);
    if (bp_init(&ctx->tx_header, name, 1) < 0) goto fail;

    snprintf(name, sizeof(name), "%s_tx_body", app_name);
    if (bp_init(&ctx->tx_body, name, 1) < 0) goto fail;

    snprintf(name, sizeof(name), "%s_rx_header", app_name);
    if (bp_init(&ctx->rx_header, name, 1) < 0) goto fail;

    snprintf(name, sizeof(name), "%s_rx_body", app_name);
    if (bp_init(&ctx->rx_body, name, 1) < 0) goto fail;

    /* Create worker SQs */
    snprintf(name, sizeof(name), "pod_%d_tx_sq", ctx->pod_id);
    if (dr_init(&ctx->tx_sq, name, 1) < 0) goto fail;

    snprintf(name, sizeof(name), "pod_%d_rx_sq", ctx->pod_id);
    if (dr_init(&ctx->rx_sq, name, 1) < 0) goto fail;

    /* Create notification pipe + poller thread */
    int pfd[2];
    if (pipe(pfd) < 0) goto fail;
    int flags = fcntl(pfd[0], F_GETFL, 0);
    fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(pfd[1], F_GETFL, 0);
    fcntl(pfd[1], F_SETFL, flags | O_NONBLOCK);

    ctx->notify_fd = pfd[0];
    ctx->notify_write_fd = pfd[1];
    ctx->poller_running = 1;
    memset(ctx->response_pipe_fds, -1, sizeof(ctx->response_pipe_fds));

    if (pthread_create(&ctx->poller_tid, NULL, dpumesh_poller_fn, ctx) != 0)
        goto fail;

    printf("[dpumesh] initialized: worker=%s pod_id=%d app=%s\n",
           ctx->worker_id, ctx->pod_id, ctx->app_name);

    *out = ctx;
    return 0;

fail:
    free(ctx);
    return -1;
}

void dpumesh_destroy(dpumesh_ctx_t *ctx) {
    if (!ctx) return;
    ctx->poller_running = 0;
    pthread_join(ctx->poller_tid, NULL);
    if (ctx->notify_fd >= 0) close(ctx->notify_fd);
    if (ctx->notify_write_fd >= 0) close(ctx->notify_write_fd);
    for (int i = 0; i < DPUMESH_MAX_PENDING; i++) {
        if (ctx->response_pipe_fds[i] >= 0)
            close(ctx->response_pipe_fds[i]);
    }
    bp_destroy(&ctx->tx_header);
    bp_destroy(&ctx->tx_body);
    bp_destroy(&ctx->rx_header);
    bp_destroy(&ctx->rx_body);
    dr_destroy(&ctx->tx_sq);
    dr_destroy(&ctx->rx_sq);
    free(ctx);
}

int dpumesh_get_notify_fd(dpumesh_ctx_t *ctx) {
    return ctx->notify_fd;
}

int dpumesh_get_pod_id(dpumesh_ctx_t *ctx) {
    return ctx->pod_id;
}

const char *dpumesh_get_worker_id(dpumesh_ctx_t *ctx) {
    return ctx->worker_id;
}

int dpumesh_read(dpumesh_ctx_t *ctx,
                 dpumesh_on_request_fn on_req, void *req_data) {
    int count = 0;
    sw_descriptor_t desc;

    /* Read descriptors from notify pipe (poller puts request descriptors here) */
    while (read(ctx->notify_fd, &desc, sizeof(desc)) == (ssize_t)sizeof(desc)) {
        if (desc.valid != 1) continue;
        count++;

        int case_flag = desc.flags & 0x0F;

        /* Incoming request */
        dpumesh_request_t req;
        memset(&req, 0, sizeof(req));
        req.req_id = desc.req_id;
        req.src_pod_id = desc.src_pod_id;
        req._header_slot = desc.header_buf_slot;
        req._body_slot = desc.body_buf_slot;
        req._case_flag = case_flag;

        /* Store routing metadata in inbound table */
        int idx = desc.req_id % DPUMESH_MAX_PENDING;
        req._desc_index = idx;

        static char req_hdr_buf[4096];
        static char req_body_buf[DPUMESH_SLOT_SIZE];
        static char method_buf[16];
        static char path_buf[256];
        static char qs_buf[512];

        if (case_flag == CASE_INGRESS) {
            /* Case 2: combined header+body in body buffer */
            if (desc.body_buf_slot >= 0 && desc.body_len > 0) {
                char combined[DPUMESH_SLOT_SIZE];
                int clen = bp_read(&ctx->rx_body, desc.body_buf_slot, combined,
                                   desc.body_len < sizeof(combined) ? desc.body_len : sizeof(combined) - 1);
                combined[clen] = '\0';

                char header_json[4096];
                parse_case2_body(combined, clen, header_json, sizeof(header_json),
                                 req_body_buf, sizeof(req_body_buf));

                json_get_str(header_json, "method", method_buf, sizeof(method_buf));
                json_get_str(header_json, "path", path_buf, sizeof(path_buf));
                json_get_str(header_json, "query_string", qs_buf, sizeof(qs_buf));
                json_get_str(header_json, "req_id_str", req.req_id_str, sizeof(req.req_id_str));
                json_get_str(header_json, "source_worker", req.source_worker, sizeof(req.source_worker));
                json_get_str(header_json, "dest_worker", req.dest_worker, sizeof(req.dest_worker));

                req.method = method_buf;
                req.path = path_buf;
                req.query_string = qs_buf;
                req.body = req_body_buf;
                req.body_len = strlen(req_body_buf);
            }
        } else {
            /* Case 3: header in rx_header, body in rx_body */
            if (desc.header_buf_slot >= 0 && desc.header_len > 0) {
                int hlen = bp_read(&ctx->rx_header, desc.header_buf_slot, req_hdr_buf,
                                   desc.header_len < sizeof(req_hdr_buf) ? desc.header_len : sizeof(req_hdr_buf) - 1);
                req_hdr_buf[hlen] = '\0';

                json_get_str(req_hdr_buf, "method", method_buf, sizeof(method_buf));
                json_get_str(req_hdr_buf, "path", path_buf, sizeof(path_buf));
                json_get_str(req_hdr_buf, "query_string", qs_buf, sizeof(qs_buf));
                json_get_str(req_hdr_buf, "req_id_str", req.req_id_str, sizeof(req.req_id_str));
                json_get_str(req_hdr_buf, "source_worker", req.source_worker, sizeof(req.source_worker));
                json_get_str(req_hdr_buf, "dest_worker", req.dest_worker, sizeof(req.dest_worker));

                req.method = method_buf;
                req.path = path_buf;
                req.query_string = qs_buf;
            }
            if (desc.body_buf_slot >= 0 && desc.body_len > 0) {
                int blen = bp_read(&ctx->rx_body, desc.body_buf_slot, req_body_buf,
                                   desc.body_len < sizeof(req_body_buf) ? desc.body_len : sizeof(req_body_buf) - 1);
                req_body_buf[blen] = '\0';
                req.body = req_body_buf;
                req.body_len = blen;
            }
        }

        /* Store routing metadata in inbound table for dpumesh_write() RESPONSE */
        ctx->inbound[idx].req_id = req.req_id;
        snprintf(ctx->inbound[idx].req_id_str, sizeof(ctx->inbound[idx].req_id_str),
                 "%s", req.req_id_str);
        snprintf(ctx->inbound[idx].source_worker, sizeof(ctx->inbound[idx].source_worker),
                 "%s", req.source_worker);
        ctx->inbound[idx].case_flag = case_flag;
        ctx->inbound[idx].active = 1;

        if (on_req) on_req(&req, req_data);

        /* Free RX buffers */
        if (desc.header_buf_slot >= 0)
            bp_free(&ctx->rx_header, desc.header_buf_slot);
        if (desc.body_buf_slot >= 0)
            bp_free(&ctx->rx_body, desc.body_buf_slot);
    }
    return count;
}

int dpumesh_read_response(dpumesh_ctx_t *ctx, int response_fd,
                          dpumesh_response_t *out_resp) {
    sw_descriptor_t desc;
    if (read(response_fd, &desc, sizeof(desc)) != (ssize_t)sizeof(desc))
        return -1;
    close(response_fd);

    memset(out_resp, 0, sizeof(*out_resp));
    out_resp->_header_slot = desc.header_buf_slot;
    out_resp->_body_slot = desc.body_buf_slot;

    /* Read header from RX header pool */
    if (desc.header_buf_slot >= 0 && desc.header_len > 0) {
        static char hbuf[4096];
        int hlen = bp_read(&ctx->rx_header, desc.header_buf_slot, hbuf,
                           desc.header_len < sizeof(hbuf) ? desc.header_len : sizeof(hbuf) - 1);
        hbuf[hlen] = '\0';
        out_resp->status_code = json_get_int(hbuf, "status_code");
        json_get_str(hbuf, "req_id_str", out_resp->req_id_str, sizeof(out_resp->req_id_str));
        json_get_str(hbuf, "source_worker", out_resp->source_worker, sizeof(out_resp->source_worker));
    }

    /* Read body from RX body pool */
    static char body_buf[DPUMESH_SLOT_SIZE];
    if (desc.body_buf_slot >= 0 && desc.body_len > 0) {
        int blen = bp_read(&ctx->rx_body, desc.body_buf_slot, body_buf,
                           desc.body_len < sizeof(body_buf) ? desc.body_len : sizeof(body_buf) - 1);
        body_buf[blen] = '\0';
        out_resp->body = body_buf;
        out_resp->body_len = blen;
    }

    /* Free RX buffers */
    if (desc.header_buf_slot >= 0)
        bp_free(&ctx->rx_header, desc.header_buf_slot);
    if (desc.body_buf_slot >= 0)
        bp_free(&ctx->rx_body, desc.body_buf_slot);

    return 0;
}

int dpumesh_write(dpumesh_ctx_t *ctx, const dpumesh_msg_t *msg,
                  int *out_response_fd) {
    int header_slot = -1, body_slot = -1;
    uint32_t req_id;
    int8_t flags;
    const void *body = msg->body;
    size_t body_len = msg->body_len;

    char hdr_data[4096];
    int hdr_len;

    if (msg->type == DPUMESH_MSG_REQUEST) {
        /* === REQUEST path (was dpumesh_send) === */
        req_id = ctx->next_req_id++;

        /* Parse URL for path and query */
        const char *url = msg->url ? msg->url : "";
        const char *path = "/";
        const char *query = "";
        const char *path_start = strstr(url, "://");
        if (path_start) {
            path_start += 3;
            path_start = strchr(path_start, '/');
            if (path_start) path = path_start;
        }
        char path_buf[256], query_buf[512];
        const char *qmark = strchr(path, '?');
        if (qmark) {
            size_t plen = qmark - path;
            if (plen >= sizeof(path_buf)) plen = sizeof(path_buf) - 1;
            memcpy(path_buf, path, plen);
            path_buf[plen] = '\0';
            snprintf(query_buf, sizeof(query_buf), "%s", qmark + 1);
            path = path_buf;
            query = query_buf;
        }

        char req_id_str[32];
        snprintf(req_id_str, sizeof(req_id_str), "creq-%u", req_id);

        hdr_len = serialize_header(hdr_data, sizeof(hdr_data),
                                   msg->method, url, path, query,
                                   0, req_id_str,
                                   ctx->worker_id, "");
        flags = OP_REQUEST | CASE_LOCAL;

        /* Create per-request pipe for response delivery */
        if (out_response_fd) {
            int rpfd[2];
            if (pipe(rpfd) < 0) return -1;
            int fl = fcntl(rpfd[0], F_GETFL, 0);
            fcntl(rpfd[0], F_SETFL, fl | O_NONBLOCK);

            int idx = req_id % DPUMESH_MAX_PENDING;
            ctx->response_pipe_fds[idx] = rpfd[1];  /* write-end for poller */
            *out_response_fd = rpfd[0];              /* read-end for caller */
        }

    } else {
        /* === RESPONSE path (was dpumesh_respond) === */
        const dpumesh_request_t *orig = msg->orig_req;
        int didx = orig->_desc_index;
        dpumesh_inbound_t *ib = &ctx->inbound[didx];
        req_id = ib->req_id;

        const char *dest_worker = ib->source_worker;
        int case_flag = CASE_INGRESS;  /* default: back to bridge */
        if (dest_worker[0] != '\0') {
            case_flag = CASE_LOCAL;  /* back to another worker */
        }

        hdr_len = serialize_header(hdr_data, sizeof(hdr_data),
                                   "", "", "", "",
                                   msg->status_code, ib->req_id_str,
                                   ctx->worker_id, dest_worker);
        flags = OP_RESPONSE | case_flag;

        /* Release inbound slot */
        ib->active = 0;
    }

    /* === Common: alloc TX buffers, build descriptor, enqueue === */
    header_slot = bp_alloc(&ctx->tx_header);
    if (header_slot < 0) return -1;
    bp_write(&ctx->tx_header, header_slot, hdr_data, hdr_len);

    body_slot = bp_alloc(&ctx->tx_body);
    if (body_slot < 0) { bp_free(&ctx->tx_header, header_slot); return -1; }
    if (body && body_len > 0)
        bp_write(&ctx->tx_body, body_slot, body, body_len);
    else
        bp_write(&ctx->tx_body, body_slot, "", 1);

    sw_descriptor_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.header_buf_slot = header_slot;
    desc.header_len = hdr_len;
    desc.body_buf_slot = body_slot;
    desc.body_len = (body && body_len > 0) ? body_len : 1;
    desc.req_id = req_id;
    desc.step_id = 0;
    desc.dst_pod_id = 0;
    desc.src_pod_id = ctx->pod_id;
    desc.flags = flags;
    desc.valid = 1;
    desc.src_header_pool_type = POOL_HOST_TX_HEADER;
    desc.src_header_pod_id = ctx->pod_id;
    desc.src_header_buf_slot = header_slot;
    desc.src_body_pool_type = POOL_HOST_TX_BODY;
    desc.src_body_pod_id = ctx->pod_id;
    desc.src_body_buf_slot = body_slot;

    if (dr_put(&ctx->tx_sq, &desc) < 0) {
        bp_free(&ctx->tx_header, header_slot);
        bp_free(&ctx->tx_body, body_slot);
        return -1;
    }

    return 0;
}

void dpumesh_request_free(dpumesh_ctx_t *ctx, const dpumesh_request_t *req) {
    (void)ctx; (void)req;
    /* Buffers already freed in dpumesh_poll */
}

void dpumesh_response_free(dpumesh_ctx_t *ctx, const dpumesh_response_t *resp) {
    (void)ctx; (void)resp;
    /* Buffers already freed in dpumesh_poll */
}
