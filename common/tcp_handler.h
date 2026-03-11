#ifndef TCP_HANDLER_H
#define TCP_HANDLER_H

#include <event2/event.h>
#include "http_parse.h"

#define BUF_SIZE 8192

typedef struct conn conn_t;
typedef void (*request_handler_fn)(conn_t *client, http_request_t *req);
typedef void (*upstream_handler_fn)(conn_t *upstream, http_response_t *resp);

struct conn {
    int                  fd;
    struct event_base   *base;
    struct event        *ev;
    char                 rbuf[BUF_SIZE];
    int                  rlen;
    char                 wbuf[BUF_SIZE];
    int                  wlen, wpos;
    void                *handler_ctx;
    conn_t              *client_conn;
};

extern request_handler_fn  g_request_handler;
extern upstream_handler_fn g_upstream_handler;

int    make_listen_socket(int port);
conn_t *conn_alloc(int fd, struct event_base *base);
void   conn_free(conn_t *c);

void conn_start_response(conn_t *c, int status, const char *body, size_t body_len);
int  conn_start_upstream(conn_t *client, const char *host, int port,
                         const char *method, const char *path, void *handler_ctx);

/* Register listen fd with event_base */
void tcp_listen_start(struct event_base *base, int listen_fd,
                      request_handler_fn handler);

/* Set upstream response handler (setter alternative to direct g_upstream_handler assignment) */
void tcp_handler_set_upstream(upstream_handler_fn handler);

/* Backward-compatible wrapper (minimal change for existing service code) */
void epoll_run(int listen_fd, request_handler_fn handler);

#endif
