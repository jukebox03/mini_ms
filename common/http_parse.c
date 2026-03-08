#include "http_parse.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *find_header_end(const char *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            return buf + i + 4;
    }
    return NULL;
}

static size_t parse_content_length(const char *headers, size_t hlen) {
    const char *p = headers;
    const char *end = headers + hlen;
    while (p < end) {
        if ((p == headers || *(p-1) == '\n') &&
            strncasecmp(p, "Content-Length:", 15) == 0) {
            p += 15;
            while (*p == ' ') p++;
            return (size_t)atol(p);
        }
        p++;
    }
    return 0;
}

int http_parse_request(http_request_t *req, const char *buf, size_t len) {
    memset(req, 0, sizeof(*req));
    const char *body_start = find_header_end(buf, len);
    if (!body_start) return 1; /* need more data */

    req->headers_complete = 1;
    size_t hlen = body_start - buf;

    /* Parse request line: "GET /path?query HTTP/1.1\r\n" */
    const char *p = buf;
    /* Method */
    int i = 0;
    while (*p != ' ' && i < 7) req->method[i++] = *p++;
    req->method[i] = '\0';
    while (*p == ' ') p++;

    /* Path + query */
    i = 0;
    while (*p != ' ' && *p != '?' && *p != '\r' && i < 255) req->path[i++] = *p++;
    req->path[i] = '\0';

    if (*p == '?') {
        p++;
        i = 0;
        while (*p != ' ' && *p != '\r' && i < 511) req->query[i++] = *p++;
        req->query[i] = '\0';
    }

    req->content_length = parse_content_length(buf, hlen);

    if (req->content_length > 0) {
        size_t body_available = len - hlen;
        if (body_available < req->content_length) return 1;
        size_t copy = req->content_length < sizeof(req->body) ? req->content_length : sizeof(req->body) - 1;
        memcpy(req->body, body_start, copy);
        req->body[copy] = '\0';
        req->body_len = copy;
    }

    req->complete = 1;
    return 0;
}

int http_format_response(char *buf, size_t bufsz, int status, const char *body, size_t body_len) {
    const char *reason;
    switch (status) {
        case 200: reason = "OK"; break;
        case 400: reason = "Bad Request"; break;
        case 404: reason = "Not Found"; break;
        case 500: reason = "Internal Server Error"; break;
        default:  reason = "Unknown"; break;
    }
    return snprintf(buf, bufsz,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%.*s",
        status, reason, body_len, (int)body_len, body);
}

int http_parse_response(http_response_t *resp, const char *buf, size_t len) {
    memset(resp, 0, sizeof(*resp));
    const char *body_start = find_header_end(buf, len);
    if (!body_start) return 1;

    resp->headers_complete = 1;
    size_t hlen = body_start - buf;

    /* Parse "HTTP/1.1 200 OK\r\n" */
    if (len < 12) return -1;
    resp->status_code = atoi(buf + 9);

    resp->content_length = parse_content_length(buf, hlen);

    if (resp->content_length > 0) {
        size_t body_available = len - hlen;
        if (body_available < resp->content_length) return 1;
        size_t copy = resp->content_length < sizeof(resp->body) ? resp->content_length : sizeof(resp->body) - 1;
        memcpy(resp->body, body_start, copy);
        resp->body[copy] = '\0';
        resp->body_len = copy;
    }

    resp->complete = 1;
    return 0;
}
