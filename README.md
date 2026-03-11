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
| `common/dpumesh_handler.h/c` | libevent 기반 DPUmesh 핸들러 (notify fd + callback) |
| `common/dpumesh.h/c` | DPUmesh 라이브러리 (SHM 통신, poller thread, notify fd) |
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
| 데이터 수신 | `read(fd, buf, len)` | `dpumesh_poll(ctx, on_resp, .., on_req, ..)` |
| 데이터 송신 | `write(fd, buf, len)` | `dpumesh_send(ctx, method, url, ..)` |
| 응답 송신 | `write(fd, buf, len)` | `dpumesh_respond(ctx, req, status, body, len)` |
| 정리 | `close(fd)` | `dpumesh_destroy(ctx)` |
| 알림 메커니즘 | kernel이 fd를 readable로 전환 | 내부 poller thread가 pipe fd에 signal |

핵심 공통점: **두 경로 모두 fd를 반환**하므로, 이벤트 루프(epoll, libevent 등)에 동일한 방식으로 등록할 수 있다.

```c
// TCP:  listen_fd readable → 새 연결 도착
// DPUmesh: notify_fd readable → SHM에 새 데이터 도착
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

## 참고

### `dpumesh_ctx_t` 내부 구조

`dpumesh_init()`이 반환하는 opaque 핸들. 서비스 코드에서는 포인터만 다룬다.

```c
struct dpumesh_ctx {
    char        app_name[64];       // 서비스 이름 ("frontend")
    char        worker_id[128];     // 워커 식별자 ("frontend-worker-0")
    int         pod_id;             // PodRegistry에서 할당받은 고유 ID
    int         notify_fd;          // pipe read end (event loop에 등록)
    int         notify_write_fd;    // pipe write end (poller thread가 signal)
    pthread_t   poller_tid;         // SHM polling thread
    uint32_t    next_req_id;        // 요청 ID 카운터

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
