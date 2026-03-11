#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "json_util.h"
#include "http_parse.h"
#include "tcp_handler.h"

static kv_store_t store;

static void handle_request(conn_t *client, http_request_t *req) {
    if (strcmp(req->method, "GET") != 0 || strcmp(req->path, "/id") != 0) {
        conn_start_response(client, 404, "{\"error\":\"not found\"}", 20);
        return;
    }

    char name[64];
    if (query_get(req->query, "name", name, sizeof(name)) != 0) {
        conn_start_response(client, 400, "{\"error\":\"missing name\"}", 23);
        return;
    }

    const int *id = kv_store_get(&store, name);
    if (!id) {
        conn_start_response(client, 404, "{\"error\":\"name not found\"}", 26);
        return;
    }

    char body[128];
    int blen = snprintf(body, sizeof(body), "{\"id\":%d}", *id);
    conn_start_response(client, 200, body, blen);
}

int main(void) {
    const char *data_file = getenv("DATA_FILE");
    if (!data_file) data_file = "data/ids.json";

    if (kv_store_load(&store, data_file) != 0) {
        fprintf(stderr, "Failed to load %s\n", data_file);
        return 1;
    }
    printf("[id_service] loaded %d entries from %s\n", store.count, data_file);

    int port = 8080;
    const char *port_env = getenv("PORT");
    if (port_env) port = atoi(port_env);

    int fd = make_listen_socket(port);
    if (fd < 0) return 1;

    epoll_run(fd, handle_request);
    close(fd);
    return 0;
}
