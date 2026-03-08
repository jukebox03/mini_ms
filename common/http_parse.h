#ifndef HTTP_PARSE_H
#define HTTP_PARSE_H

#include <stddef.h>

typedef struct {
    char method[8];        /* GET, POST */
    char path[256];        /* /lookup */
    char query[512];       /* name=alice&... */
    char body[4096];
    size_t body_len;
    size_t content_length;
    int headers_complete;  /* 1 if \r\n\r\n found */
    int complete;          /* 1 if full request received */
} http_request_t;

/* Parse raw data into request. Returns 0 if complete, 1 if need more data, -1 on error. */
int http_parse_request(http_request_t *req, const char *buf, size_t len);

/* Format HTTP response into buf. Returns bytes written. */
int http_format_response(char *buf, size_t bufsz, int status, const char *body, size_t body_len);

/* Parse HTTP response (client side). Returns 0 if complete, 1 if need more, -1 error. */
typedef struct {
    int status_code;
    char body[4096];
    size_t body_len;
    size_t content_length;
    int headers_complete;
    int complete;
} http_response_t;

int http_parse_response(http_response_t *resp, const char *buf, size_t len);

#endif
