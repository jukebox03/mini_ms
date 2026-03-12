# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Benchmark microservice chain (Frontend → ID Service → Attend Service) written in C with libevent. Supports two data paths—TCP sockets and DPUmesh (shared memory via `/dev/shm`)—using the same handler code.

## Build

```bash
make tcp      # TCP binaries (frontend, id_service, attend_service)
make dpumesh  # DPUmesh binaries (*_dpumesh), requires -lpthread
make all      # Both
make clean
```

Dependency: `libevent-dev` (build), `libevent-2.1-7` (runtime).

## Deploy & Test

```bash
./deploy_tcp.sh              # Deploy TCP version to k8s
./deploy_dpumesh.sh          # Deploy DPUmesh version (needs dpa/dpu daemons)
python3 test_client.py [host] [port] [concurrency]          # TCP load test
python3 test_client_dpumesh.py [host] [port] [concurrency]  # DPUmesh load test
```

K8s manifests: `k8s/mini-ms.yaml` (TCP), `k8s/mini-ms-dpumesh.yaml` (DPUmesh).

## Architecture

```
Layer 3:  Service code        {frontend,id_service,attend_service}/main.c | main_dpumesh.c
Layer 2:  Handler             common/tcp_handler.c  |  common/dpumesh_handler.c
Layer 1:  Event loop          libevent (event_base_dispatch)
Layer 0:  Data path           TCP socket            |  SHM (common/dpumesh.c)
```

- TCP and DPUmesh share the same `event_base` and `conn_t` abstraction.
- Each service has two entry points: `main.c` (TCP) and `main_dpumesh.c` (DPUmesh). Business logic (request/upstream handlers) is identical; only init and read/write calls differ.
- DPUmesh handler wraps low-level SHM into libevent via per-request pipe fds (responses) and a main notify pipe (requests), so handlers receive the same `conn_t *`. Upstream response delivery uses the same fd+libevent pattern as TCP.

## Key Files

| File | Role |
|------|------|
| `common/tcp_handler.h/c` | libevent TCP handler: accept, read, write, upstream relay |
| `common/dpumesh_handler.h/c` | DPUmesh→libevent bridge (descriptor index pattern, per-request pipe fd + libevent) |
| `common/dpumesh.h/c` | Low-level SHM API: `dpumesh_read`/`dpumesh_write`/`dpumesh_read_response` (BufferPool, DescriptorRing, PodRegistry, per-request pipe) |
| `common/http_parse.h/c` | HTTP/1.1 request/response parser |
| `common/json_util.h/c` | JSON file loader + query string parser |
| `dpumesh/common.py` | SHM layout constants shared with C (`dpumesh.h` constants must match) |
| `dpumesh/dpa_daemon.py` | DMA manager daemon |
| `dpumesh/dpu_daemon.py` | TCP bridge + router daemon |

## Service Pattern

**TCP service** (`main.c`):
```c
tcp_handler_set_upstream(handle_upstream_response);  // if service calls upstream
int fd = make_listen_socket(port);
epoll_run(fd, handle_request);
```

**DPUmesh service** (`main_dpumesh.c`):
```c
dpumesh_handler_set_upstream(handle_upstream_response);  // if needed
dpumesh_init(&dpu_ctx, app_name, worker_id);
dpumesh_run(dpu_ctx, handle_request);
dpumesh_destroy(dpu_ctx);
```

Response/upstream calls swap `conn_start_response(c, ...)` → `dpumesh_conn_respond(c, ...)` and `conn_start_upstream(c, ...)` → `dpumesh_conn_upstream(c, ...)`. Handler signatures stay the same. DPUmesh per-request calls no longer require `dpu_ctx` — the handler stores it internally. Routing metadata (req_id, source_worker, case_flag) lives in `ctx->inbound[]` inside the library; the handler only carries an opaque `dpu_desc_index`.

## Environment Variables

- Services: `PORT`, `ID_SERVICE_HOST/PORT`, `ATTEND_SERVICE_HOST/PORT`
- DPUmesh: `SHM_PREFIX` (namespace, default `"dpumesh"`), `APP_NAME`, `WORKER_ID`

## Critical Invariant

SHM struct layout constants in `common/dpumesh.h` (C) must match `dpumesh/common.py` (Python). The `sw_descriptor_t` is 64 bytes packed matching Python struct format `'<iIiIIIiibbBBiiii12x'`.
