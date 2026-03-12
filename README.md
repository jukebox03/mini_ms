# mini_ms — libevent 기반 마이크로서비스 + DPUmesh SHM 통합

3개 서비스 체인(Frontend → ID Service → Attend Service)으로 구성된 벤치마크.
TCP와 DPUmesh(SHM) 두 가지 데이터 경로를 동일한 핸들러 코드로 지원한다.

## 아키텍처

```
                          TCP                        DPUmesh
                    ─────────────────        ─────────────────────
Layer 3  서비스       main.c                   main_dpumesh.c
Layer 2  핸들러       tcp_handler.c            dpumesh_handler.c
Layer 1  이벤트 루프  libevent                 libevent
Layer 0  데이터 경로  socket (kernel)          SHM + poller (dpumesh.c)
```

TCP와 DPUmesh 모두 libevent의 `event_base`를 공유하며, 핸들러는 동일한 `conn_t *`를 받는다.

## 파일 구조

| 파일 | 역할 |
|------|------|
| `common/tcp_handler.h/c` | libevent 기반 TCP 핸들러 (accept, read, write, upstream) |
| `common/dpumesh_handler.h/c` | libevent 기반 DPUmesh 핸들러 (per-request fd + callback) |
| `common/dpumesh.h/c` | DPUmesh 라이브러리 (SHM 통신, poller thread, per-request pipe) |
| `common/http_parse.h/c` | HTTP/1.1 파서 |
| `common/json_util.h/c` | JSON 로더 + query string 파서 |
| `dpumesh/common.py` | SHM 레이아웃 정의 (Python 데몬과 공유) |
| `dpumesh/dpa_daemon.py` | DMA 매니저 데몬 |
| `dpumesh/dpu_daemon.py` | TCP 브리지 + 라우터 데몬 |

## 빌드

```bash
make tcp      # TCP 버전 (3개 바이너리)
make dpumesh  # DPUmesh 버전 (3개 바이너리)
make all      # 둘 다
```

의존성: `libevent-dev` (빌드), `libevent-2.1-7` (런타임)

## 배포

```bash
./deploy_tcp.sh       # TCP (k8s)
./deploy_dpumesh.sh   # DPUmesh (k8s, daemon 필요)
```

## API 비교

### Layer 0: 라이브러리 API (socket vs dpumesh)

개발자가 라이브러리 수준에서 알아야 하는 API. TCP는 OS가 제공하는 socket API, DPUmesh는 `dpumesh.h`가 동일한 역할을 한다.

| 역할 | socket API (OS) | dpumesh API (`dpumesh.h`) |
|------|----------------|--------------------------|
| 초기화 | `socket()` + `bind()` + `listen()` | `dpumesh_init(&ctx, app, worker_id)` |
| notify fd 획득 | listen fd (socket 자체) | `dpumesh_get_notify_fd(ctx)` |
| 요청 수신 | `read(fd, buf, len)` | `dpumesh_read(ctx, on_req, ..)` (notify fd에서 descriptor 읽기) |
| 응답 수신 | `read(fd, buf, len)` | `dpumesh_read_response(ctx, response_fd, &resp)` (per-request fd) |
| 데이터 송신 | `write(fd, buf, len)` | `dpumesh_write(ctx, &msg, &req_id, &response_fd)` (REQUEST 시 per-request response_fd 반환) |
| 정리 | `close(fd)` | `dpumesh_destroy(ctx)` |
| 알림 메커니즘 | kernel이 fd를 readable로 전환 | 내부 poller thread가 pipe fd에 signal |

핵심 공통점: **두 경로 모두 fd를 반환**하므로, 이벤트 루프(epoll, libevent 등)에 동일한 방식으로 등록할 수 있다.

```c
// TCP:  listen_fd readable → 새 연결 도착
// DPUmesh: notify_fd readable → dpumesh_read()로 RX SQ 처리
```

### Layer 2: 핸들러 API (tcp_handler vs dpumesh_handler)

libevent와 통합하기 위한 glue 코드. 개발자가 작성하는(작성하게 될) 핸들러.

**Init/등록**

| 역할 | tcp_handler | dpumesh_handler |
|------|------------|-----------------|
| fd + handler 등록 | `tcp_listen_start(base, fd, handler)` | `dpumesh_handler_init(base, ctx, handler)` |
| upstream 핸들러 설정 | `tcp_handler_set_upstream(fn)` | `dpumesh_handler_set_upstream(fn)` |
| 편의 래퍼 | `epoll_run(fd, handler)` | `dpumesh_run(ctx, handler)` |

**Per-request 연산** (서비스 핸들러 안에서 호출)

| 역할 | tcp_handler | dpumesh_handler |
|------|------------|-----------------|
| 응답 | `conn_start_response(c, status, body, len)` | `dpumesh_conn_respond(c, status, body, len)` |
| 업스트림 요청 | `conn_start_upstream(c, host, port, method, path, hctx)` | `dpumesh_conn_upstream(c, method, url, hctx)` |

응답 함수의 시그니처가 동일하고, 업스트림 함수는 주소 지정 방식만 다르다 (host+port vs URL).

### Layer 3: 서비스 코드 비교

**Leaf service** (응답만 하는 서비스, 예: id_service)

```c
// TCP (id_service/main.c)              // DPUmesh (id_service/main_dpumesh.c)
// ─────────────────────────────        // ──────────────────────────────────────
#include "tcp_handler.h"                #include "tcp_handler.h"
                                        #include "dpumesh_handler.h"
                                        static dpumesh_ctx_t *dpu_ctx;

static void handle_request(             static void handle_request(
    conn_t *c, http_request_t *req) {       conn_t *c, http_request_t *req) {
  // 비즈니스 로직 동일                     // 비즈니스 로직 동일
  conn_start_response(c, 200, b, len);    dpumesh_conn_respond(c, 200, b, len);
}                                       }

int main(void) {                        int main(void) {
  int fd = make_listen_socket(port);      dpumesh_init(&dpu_ctx, app, worker_id);
  epoll_run(fd, handle_request);          dpumesh_run(dpu_ctx, handle_request);
}                                         dpumesh_destroy(dpu_ctx);
                                        }
```

**Gateway service** (업스트림 호출이 있는 서비스, 예: frontend)

```c
// TCP (frontend/main.c)                // DPUmesh (frontend/main_dpumesh.c)
// ─────────────────────────────        // ──────────────────────────────────────

// 핸들러 안에서:
conn_start_upstream(client,             dpumesh_conn_upstream(client,
    host, port, "GET", path, ctx);          "GET", url, ctx);

// main에서:
tcp_handler_set_upstream(handle_upstream);  dpumesh_handler_set_upstream(handle_upstream);
```

핸들러 시그니처(`request_handler_fn`, `upstream_handler_fn`)는 양쪽 동일 — 비즈니스 로직 수정 없음.

## 환경변수

- 공통: `PORT`, `APP`, `WORKER_ID`
- TCP: `ID_SERVICE_HOST/PORT`, `ATTEND_SERVICE_HOST/PORT`
- DPUmesh: `SHM_PREFIX` (SHM namespace, 기본값 `"dpumesh"`), `NAMESPACE` (k8s)

## DPUmesh 핵심 설계: 두 채널 구조

### `dpumesh_read` vs `dpumesh_read_response` — 왜 따로 있는가?

두 함수는 **서로 다른 알림 채널**을 처리하기 때문에 분리되어 있다.

| | `dpumesh_read` | `dpumesh_read_response` |
|---|---|---|
| **역할** | 외부에서 들어오는 **요청** 수신 | 업스트림에 보낸 **요청의 응답** 수신 |
| **채널** | 공유 notify_fd (메인 파이프, 1개) | per-request response_fd (요청마다 1개) |
| **호출 주체** | 이벤트 루프 → notify_fd readable 시 자동 호출 | 이벤트 루프 → response_fd readable 시 자동 호출 |
| **읽는 대상** | SHM rx_sq에서 온 **inbound request** descriptor | 특정 요청에 대한 **response** descriptor |
| **비유** | TCP의 `accept()` + `read()` on listen_fd | TCP의 `read()` on accepted connection fd |

이 분리는 TCP 소켓의 동작 방식을 그대로 모방한 것이다:

```
TCP:                               DPUmesh:
listen_fd  (broadcast)             notify_fd       (broadcast)
  └─ accept() → conn_fd              └─ dpumesh_read() → on_request callback
       └─ read(conn_fd)                    └─ dpumesh_write() → response_fd (unicast)
                                                └─ dpumesh_read_response(response_fd)
```

- **`dpumesh_read()`**: notify_fd 하나에서 모든 수신 요청을 배치로 드레인한다. poller thread가 SHM rx_sq를 모니터링하다가 **request** descriptor가 오면 notify_fd pipe에 write → event loop가 깨어나 이 함수를 호출한다.
- **`dpumesh_read_response()`**: **단 하나의 응답**만 읽는다. 해당 요청 전용 pipe fd에서 descriptor를 읽고, 읽은 즉시 pipe를 닫는다.

### `response_fd`란 무엇인가?

`response_fd`는 `dpumesh_write()`가 **DPUMESH_MSG_REQUEST** 타입(업스트림 요청)을 보낼 때 생성하는 **단일 요청 전용 pipe의 read-end fd**다.

```
                   dpumesh_write() 호출
                         │
                         ├─ tx_sq에 request descriptor 삽입 (SHM)
                         │
                         └─ pipe(rpfd) 생성
                               ├─ rpfd[1] (write-end) → ctx->response_pipe_fds[req_id % DPUMESH_MAX_PENDING] 저장
                               └─ rpfd[0] (read-end)  → *out_response_fd 로 반환  ← 이것이 response_fd
```

반환된 `response_fd`는 dpumesh_handler.c에서 libevent에 등록된다:

```c
// dpumesh_conn_upstream() 내부 동작 흐름
int response_fd;
dpumesh_write(ctx, &msg, &req_id, &response_fd);   // response_fd 획득

uc->base.fd = response_fd;
event_new(base, response_fd, EV_READ,              // libevent에 등록
          on_upstream_response_cb, uc);
```

업스트림 서비스가 응답을 보내면:

```
업스트림 응답 → SHM rx_sq → poller thread 감지
    → ctx->response_pipe_fds[req_id % DPUMESH_MAX_PENDING] 에 descriptor write
    → response_fd readable
    → libevent 콜백(on_upstream_response_cb) 호출
    → dpumesh_read_response(ctx, response_fd, &resp) 로 응답 역직렬화
```

이 구조 덕분에 **동시에 여러 개의 업스트림 요청이 in-flight 상태**여도 각각이 독립적인 fd를 가지므로 libevent가 별도로 다중화할 수 있다. `DPUMESH_MAX_PENDING`(기본 256)이 동시에 처리할 수 있는 최대 in-flight 요청 수다.

## 참고

### `dpumesh_ctx_t` 내부 구조

`dpumesh_init()`이 반환하는 opaque 핸들. 서비스 코드에서는 포인터만 다룬다.

```c
struct dpumesh_ctx {
    char        app_name[64];       // 서비스 이름 ("frontend")
    char        worker_id[128];     // 워커 식별자 ("frontend-worker-0")
    int         pod_id;             // PodRegistry에서 할당받은 고유 ID
    int         notify_fd;          // pipe read end (event loop에 등록, request descriptor 수신)
    int         notify_write_fd;    // pipe write end (poller thread가 request descriptor 전달)
    pthread_t   poller_tid;         // SHM polling thread
    uint32_t    next_req_id;        // 요청 ID 카운터
    int         response_pipe_fds[MAX_PENDING]; // per-request pipe write-end (poller가 response descriptor 전달)
    dpumesh_inbound_t inbound[MAX_PENDING];    // 인바운드 라우팅 메타데이터 (req_id, source_worker 등)

    buffer_pool_t tx_header, tx_body;   // 송신 버퍼 풀 (SHM mmap)
    buffer_pool_t rx_header, rx_body;   // 수신 버퍼 풀 (SHM mmap)
    desc_ring_t   tx_sq, rx_sq;         // 디스크립터 링 (SHM mmap)
};
```

### DOCA 전환 시 변경 범위

`dpumesh_ctx_t`는 데이터 경로 추상화이므로, DOCA 구현에서도 동일한 구조를 유지한다.
내부 필드만 교체되고 서비스 코드는 변경 없음.

| 필드 | 현재 (SHM) | DOCA |
|------|-----------|------|
| `app_name`, `worker_id`, `pod_id` | 동일 | 동일 |
| `next_req_id` | 동일 | 동일 |
| `tx/rx_header`, `tx/rx_body` | `/dev/shm` mmap | `doca_mmap` + `doca_buf` |
| `tx_sq`, `rx_sq` | SHM 기반 링 버퍼 | `doca_dma` work queue |
| `notify_fd` | pipe (poller thread → main) | `doca_pe` completion fd |
