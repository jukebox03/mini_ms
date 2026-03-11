CC = gcc
CFLAGS = -O2 -Wall -Wextra -Icommon
COMMON_SRC = common/json_util.c common/http_parse.c common/tcp_handler.c
DPUMESH_SRC = $(COMMON_SRC) common/dpumesh.c common/dpumesh_handler.c

all: tcp dpumesh

tcp: frontend/frontend id_service/id_service attend_service/attend_service

dpumesh: frontend/frontend_dpumesh id_service/id_service_dpumesh attend_service/attend_service_dpumesh

# === TCP versions ===
frontend/frontend: frontend/main.c $(COMMON_SRC)
	$(CC) $(CFLAGS) -o $@ $^ -levent

id_service/id_service: id_service/main.c $(COMMON_SRC)
	$(CC) $(CFLAGS) -o $@ $^ -levent

attend_service/attend_service: attend_service/main.c $(COMMON_SRC)
	$(CC) $(CFLAGS) -o $@ $^ -levent

# === DPUmesh versions ===
frontend/frontend_dpumesh: frontend/main_dpumesh.c $(DPUMESH_SRC)
	$(CC) $(CFLAGS) -o $@ $^ -levent -lpthread

id_service/id_service_dpumesh: id_service/main_dpumesh.c $(DPUMESH_SRC)
	$(CC) $(CFLAGS) -o $@ $^ -levent -lpthread

attend_service/attend_service_dpumesh: attend_service/main_dpumesh.c $(DPUMESH_SRC)
	$(CC) $(CFLAGS) -o $@ $^ -levent -lpthread

clean:
	rm -f frontend/frontend id_service/id_service attend_service/attend_service
	rm -f frontend/frontend_dpumesh id_service/id_service_dpumesh attend_service/attend_service_dpumesh

.PHONY: all tcp dpumesh clean
