#ifndef EPOLL_SERVER_H
#define EPOLL_SERVER_H

#include <sys/epoll.h>
#include "http_parse.h"

#define MAX_EVENTS 64
#define BUF_SIZE   8192

typedef enum {
    CONN_LISTENING,
    CONN_CLIENT_READING,
    CONN_CLIENT_WRITING,
    CONN_UPSTREAM_CONNECTING,
    CONN_UPSTREAM_WRITING,
    CONN_UPSTREAM_READING,
    /* Future: CONN_DPUMESH_NOTIFY */
} conn_state_t;

typedef struct conn {
    int            fd;
    conn_state_t   state;
    char           rbuf[BUF_SIZE];
    int            rlen;
    char           wbuf[BUF_SIZE];
    int            wlen;
    int            wpos;
    void          *handler_ctx;     /* service-specific context */
    struct conn   *client_conn;     /* upstream → originating client */
    int            epfd;            /* reference to epoll fd */
} conn_t;

/* Called when a complete HTTP request is received from a client.
 * Handler should either:
 *   - Call conn_start_response() to send response immediately, or
 *   - Call conn_start_upstream() to make an upstream call first.
 */
typedef void (*request_handler_fn)(conn_t *client, http_request_t *req);

/* Called when an upstream HTTP response is received.
 * upstream->client_conn points to the originating client connection.
 */
typedef void (*upstream_handler_fn)(conn_t *upstream, http_response_t *resp);

/* Global upstream response handler (set by service) */
extern upstream_handler_fn g_upstream_handler;

/* Create listening socket on port. Returns fd or -1. */
int make_listen_socket(int port);

/* Start the epoll event loop. Never returns. */
void epoll_run(int listen_fd, request_handler_fn handler);

/* Queue response to client and switch to writing state. */
void conn_start_response(conn_t *c, int status, const char *body, size_t body_len);

/* Initiate non-blocking upstream HTTP request.
 * client_conn is the original client connection waiting for the result.
 * Returns 0 on success, -1 on error.
 */
int conn_start_upstream(conn_t *client, const char *host, int port,
                        const char *method, const char *path,
                        void *handler_ctx);

/* Allocate a connection struct */
conn_t *conn_alloc(int fd, conn_state_t state, int epfd);

/* Free a connection struct and close fd */
void conn_free(conn_t *c);

#endif
