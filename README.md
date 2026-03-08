# mini_ms — DPUmesh C Non-blocking API Demo

C epoll 기반 마이크로서비스 벤치마크. DPUmesh C API가 기존 event-driven 서버에
**fd 하나 + case 하나**만으로 통합됨을 보인다.

## Microservice Architecture

```
                    TCP version (mini-ms)
┌────────┐  HTTP   ┌──────────┐  HTTP   ┌────────────┐  HTTP   ┌────────────────┐
│ Client ├────────→│ Frontend ├────────→│ ID Service ├────────→│ Attend Service │
└────────┘         │ :8080    │         │ :8081      │         │ :8082          │
                   └──────────┘         └────────────┘         └────────────────┘

                    DPUmesh version (mini-ms-dpumesh)
┌────────┐  TCP    ┌───────────┐  SHM   ┌──────────┐  SHM   ┌────────────┐  SHM   ┌────────────────┐
│ Client ├────────→│ DPU Daemon├───────→│ Frontend ├───────→│ ID Service ├───────→│ Attend Service │
└────────┘  :5060  │ TCP Bridge│  DMA   │ Worker   │  DMA   │ Worker     │  DMA   │ Worker         │
                   └───────────┘        └──────────┘        └────────────┘        └────────────────┘
                         ↑                    ↑                   ↑                      ↑
                   ┌───────────┐         /dev/shm (hostPath volume, SHM_PREFIX=minims)
                   │ DPA Daemon│         BufferPool + DescriptorRing
                   │ DMA Mgr   │
                   └───────────┘
```

**Flow:** Client → `name=alice` → Frontend → ID Service(`alice→1`) → Attend Service(`1→true`) → `{"name":"alice","id":1,"attended":true}`

---

## DPUmesh C API

### Core Functions

```c
dpumesh_init(ctx, app_name, worker_id)   // SHM pools + SQs 생성, PodRegistry 등록
dpumesh_get_notify_fd(ctx)               // epoll에 등록할 fd 반환
dpumesh_poll(ctx, on_resp, .., on_req, ..) // RX SQ drain → callback 호출
dpumesh_send(ctx, method, url, ..)       // TX SQ에 descriptor 제출 (non-blocking)
dpumesh_respond(ctx, req, status, body, ..) // 응답 전송
dpumesh_destroy(ctx)                     // 정리
```

### Event Loop Integration

epoll server에 DPUmesh를 통합하는 데 필요한 코드 전부:

```c
// fd 하나 등록
int dpu_fd = dpumesh_get_notify_fd(ctx);
epoll_ctl(epfd, EPOLL_CTL_ADD, dpu_fd, &ev);

// case 하나 추가
if (c->state == CONN_DPUMESH_NOTIFY) {
    dpumesh_poll(ctx, on_response, NULL, on_request, NULL);
}
```

기존 socket 처리 로직은 변경 없음. 이 패턴은 **모든 fd 기반 event loop에 동일하게 적용**된다:

| Framework | 통합 코드 |
|-----------|----------|
| **epoll (C)** | `epoll_ctl(epfd, ADD, notify_fd, &ev)` |
| **nginx C module** | `ngx_add_event(c->read, NGX_READ_EVENT, 0)` |
| **Python asyncio** | `loop.add_reader(notify_fd, callback)` |
| **libevent** | `event_new(base, notify_fd, EV_READ, cb, NULL)` |
| **Go netpoll** | `rawConn.Read(func(fd uintptr) bool { ... })` |

### Why This Pattern?

OS network stack을 우회하는 기술은 모두 같은 패턴을 사용한다:

| 기술 | Submit | Completion |
|------|--------|------------|
| io_uring | SQ에 SQE 제출 | CQ에서 CQE 수신 (fd) |
| RDMA | Send Queue에 WR | Completion Queue (fd) |
| DPDK | TX ring에 mbuf | RX ring polling |
| **DPUmesh** | TX SQ에 descriptor | RX SQ + notify_fd |

---

## Project Structure

```
mini_ms/
├── common/
│   ├── epoll_server.{h,c}          # epoll event loop
│   ├── epoll_server_dpumesh.{h,c}  # + DPUmesh notify_fd 통합
│   ├── dpumesh.{h,c}               # DPUmesh C API (Python SHM binary-compatible)
│   ├── http_parse.{h,c}            # 최소 HTTP/1.1 파서
│   └── json_util.{h,c}             # JSON loader + query string 파서
├── frontend/
│   ├── main.c                      # TCP 버전
│   └── main_dpumesh.c              # DPUmesh 버전
├── id_service/
│   ├── main.c / main_dpumesh.c
│   └── data/ids.json               # name → id 매핑 데이터
├── attend_service/
│   ├── main.c / main_dpumesh.c
│   └── data/attendance.json        # id → 출석여부 데이터
├── k8s/
│   ├── mini-ms.yaml                # TCP 버전 (namespace: mini-ms)
│   └── mini-ms-dpumesh.yaml        # DPUmesh 버전 (namespace: mini-ms-dpumesh)
├── Makefile
└── test_client.py
```

---

## Build & Run

### TCP version (로컬)

```bash
make tcp
DATA_FILE=id_service/data/ids.json PORT=8081 ./id_service/id_service &
DATA_FILE=attend_service/data/attendance.json PORT=8082 ./attend_service/attend_service &
ID_SERVICE_HOST=127.0.0.1 ID_SERVICE_PORT=8081 \
ATTEND_SERVICE_HOST=127.0.0.1 ATTEND_SERVICE_PORT=8082 \
PORT=8080 ./frontend/frontend &
curl "http://localhost:8080/lookup?name=alice"
```

### TCP version (k8s)

```bash
docker build -t mini-ms-frontend:latest -f frontend/Dockerfile .
docker build -t mini-ms-id-service:latest -f id_service/Dockerfile .
docker build -t mini-ms-attend-service:latest -f attend_service/Dockerfile .
docker save mini-ms-frontend:latest | sudo ctr -n k8s.io images import -
docker save mini-ms-id-service:latest | sudo ctr -n k8s.io images import -
docker save mini-ms-attend-service:latest | sudo ctr -n k8s.io images import -
kubectl apply -f k8s/mini-ms.yaml
curl "http://localhost:30080/lookup?name=alice"
```

### DPUmesh version (k8s)

```bash
make dpumesh
docker build -t mini-ms-frontend-dpumesh:latest -f frontend/Dockerfile.dpumesh .
docker build -t mini-ms-id-service-dpumesh:latest -f id_service/Dockerfile.dpumesh .
docker build -t mini-ms-attend-service-dpumesh:latest -f attend_service/Dockerfile.dpumesh .
docker save mini-ms-frontend-dpumesh:latest | sudo ctr -n k8s.io images import -
docker save mini-ms-id-service-dpumesh:latest | sudo ctr -n k8s.io images import -
docker save mini-ms-attend-service-dpumesh:latest | sudo ctr -n k8s.io images import -
kubectl apply -f k8s/mini-ms-dpumesh.yaml
# Test via DPU daemon TCP bridge (port 5060)
python3 test_client.py localhost 5060
```

### Load test

```bash
python3 test_client.py localhost 30080      # TCP version
python3 test_client.py localhost 5060       # DPUmesh version
python3 test_client.py localhost 30080 50   # 50 concurrent
```

---

## TCP vs DPUmesh: Code Diff

서비스 코드의 변경은 I/O 계층뿐이다. 비즈니스 로직은 동일:

```diff
 // id_service
-static void handle_request(conn_t *client, http_request_t *req) {
+static void on_request(const dpumesh_request_t *req, void *ud) {
     const int *id = kv_store_get(&store, name);
-    conn_start_response(client, 200, body, blen);
+    dpumesh_respond(dpu_ctx, req, 200, body, blen);
 }

 int main() {
-    int fd = make_listen_socket(port);
-    epoll_run(fd, handle_request);
+    dpumesh_init(&dpu_ctx, app, worker_id);
+    epoll_run_dpumesh(-1, NULL, dpu_ctx, on_resp, NULL, on_req, NULL);
 }
```

---

## SHM Isolation

`SHM_PREFIX` 환경변수로 `/dev/shm` 파일 namespace를 분리:

| 배포 | SHM_PREFIX | SHM 파일 | Bridge 포트 |
|------|------------|----------|-------------|
| dpumesh-system (muBench) | `dpumesh` | `/dev/shm/dpumesh_*` | 5050 |
| mini-ms-dpumesh | `minims` | `/dev/shm/minims_*` | 5060 |

동일 노드에서 독립적으로 동시 운영 가능.
