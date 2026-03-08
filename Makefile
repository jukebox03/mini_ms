CC = gcc
CFLAGS = -O2 -Wall -Wextra -Icommon
COMMON_SRC = common/json_util.c common/http_parse.c common/epoll_server.c
DPUMESH_SRC = common/json_util.c common/http_parse.c common/epoll_server.c common/dpumesh.c common/epoll_server_dpumesh.c

all: tcp dpumesh

tcp: frontend/frontend id_service/id_service attend_service/attend_service

dpumesh: frontend/frontend_dpumesh id_service/id_service_dpumesh attend_service/attend_service_dpumesh

# === TCP versions ===
frontend/frontend: frontend/main.c $(COMMON_SRC)
	$(CC) $(CFLAGS) -o $@ $^

id_service/id_service: id_service/main.c $(COMMON_SRC)
	$(CC) $(CFLAGS) -o $@ $^

attend_service/attend_service: attend_service/main.c $(COMMON_SRC)
	$(CC) $(CFLAGS) -o $@ $^

# === DPUmesh versions ===
frontend/frontend_dpumesh: frontend/main_dpumesh.c $(DPUMESH_SRC)
	$(CC) $(CFLAGS) -o $@ $^

id_service/id_service_dpumesh: id_service/main_dpumesh.c $(DPUMESH_SRC)
	$(CC) $(CFLAGS) -o $@ $^

attend_service/attend_service_dpumesh: attend_service/main_dpumesh.c $(DPUMESH_SRC)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f frontend/frontend id_service/id_service attend_service/attend_service
	rm -f frontend/frontend_dpumesh id_service/id_service_dpumesh attend_service/attend_service_dpumesh

.PHONY: all tcp dpumesh clean
