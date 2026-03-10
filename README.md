# mini_ms — libevent 기반 마이크로서비스 + DPUmesh SHM 통합

3개 서비스 체인(Frontend → ID Service → Attend Service)으로 구성된 벤치마크.
TCP와 DPUmesh(SHM) 두 가지 데이터 경로를 동일한 핸들러 코드로 지원한다.

## 아키텍처

```
Layer 3:  서비스 코드        main.c / main_dpumesh.c
Layer 2:  핸들러             tcp_handler.c  |  dpumesh_adapter.c
Layer 1:  이벤트 루프        libevent (event_base_dispatch)
Layer 0:  데이터 경로        TCP socket     |  SHM (dpumesh.c)
```

TCP와 DPUmesh 모두 libevent의 `event_base`를 공유하며, 핸들러는 동일한 `conn_t *`를 받는다.

## 파일 구조

| 파일 | 역할 |
|------|------|
| `common/tcp_handler.h/c` | libevent 콜백 기반 TCP 핸들러 (accept, read, write, upstream) |
| `common/dpumesh_adapter.h/c` | DPUmesh → libevent 통합 (poller thread + pipe notification) |
| `common/dpumesh.h/c` | Low-level SHM 통신 (BufferPool, DescriptorRing, PodRegistry) |
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

## API Reference

### TCP Handler (`tcp_handler.h`)

서비스 코드가 직접 사용하는 API.

```c
// 핸들러 시그니처
typedef void (*request_handler_fn)(conn_t *client, http_request_t *req);
typedef void (*upstream_handler_fn)(conn_t *upstream, http_response_t *resp);

// 리슨 소켓 생성
int make_listen_socket(int port);

// 클라이언트에 응답 전송 (완료 후 conn 자동 해제)
void conn_start_response(conn_t *c, int status, const char *body, size_t body_len);

// 업스트림 HTTP 요청 (응답은 g_upstream_handler로 전달)
int conn_start_upstream(conn_t *client, const char *host, int port,
                        const char *method, const char *path, void *handler_ctx);

// 이벤트 루프 시작 (블로킹)
void epoll_run(int listen_fd, request_handler_fn handler);

// 글로벌 업스트림 핸들러 (사용 전 설정 필요)
extern upstream_handler_fn g_upstream_handler;
```

`conn_t` 구조체:

```c
struct conn {
    int                  fd;
    struct event_base   *base;
    struct event        *ev;
    char                 rbuf[BUF_SIZE];   // 수신 버퍼
    int                  rlen;
    char                 wbuf[BUF_SIZE];   // 송신 버퍼
    int                  wlen, wpos;
    void                *handler_ctx;      // 서비스별 컨텍스트
    conn_t              *client_conn;      // upstream → 원래 client 연결
};
```

### DPUmesh Adapter (`dpumesh_adapter.h`)

DPUmesh 서비스에서 TCP handler 대신 사용하는 API.

```c
// dpumesh adapter 초기화 (poller thread 시작, pipe notification 등록)
int dpumesh_adapter_init(struct event_base *base, dpumesh_ctx_t *dpu_ctx,
                         request_handler_fn handler);

// 업스트림 응답 핸들러 설정 (gateway 서비스만 필요)
void dpumesh_adapter_set_upstream_handler(upstream_handler_fn handler);

// SHM으로 응답 전송 (conn_start_response 대체, 전송 후 conn 자동 해제)
void dpumesh_conn_respond(conn_t *client, dpumesh_ctx_t *ctx,
                          int status, const char *body, size_t body_len);

// SHM으로 업스트림 요청 (conn_start_upstream 대체)
int dpumesh_conn_upstream(conn_t *client, dpumesh_ctx_t *ctx,
                          const char *method, const char *url, void *handler_ctx);
```

### Low-level SHM API (`dpumesh.h`)

adapter 내부에서 사용. 직접 호출할 일은 드물다.

```c
int  dpumesh_init(dpumesh_ctx_t **ctx, const char *app_name, int worker_id);
void dpumesh_destroy(dpumesh_ctx_t *ctx);

dpumesh_req_id dpumesh_send(dpumesh_ctx_t *ctx, const char *method,
                            const char *url, const char *headers_json,
                            const void *body, size_t body_len);

int dpumesh_respond(dpumesh_ctx_t *ctx, const dpumesh_request_t *req,
                    int status_code, const char *body, size_t body_len);

int dpumesh_poll(dpumesh_ctx_t *ctx,
                 dpumesh_on_response_fn on_resp, void *resp_data,
                 dpumesh_on_request_fn  on_req,  void *req_data);
```

환경변수 `SHM_PREFIX`로 SHM namespace 분리 (기본값 `"dpumesh"`).

## TCP → DPUmesh 전환 가이드

### Leaf service (응답만 하는 서비스)

```diff
-#include "tcp_handler.h"
+#include "tcp_handler.h"
+#include "dpumesh_adapter.h"
+static dpumesh_ctx_t *dpu_ctx;

 static void handle_request(conn_t *client, http_request_t *req) {
-    conn_start_response(client, 200, body, len);
+    dpumesh_conn_respond(client, dpu_ctx, 200, body, len);
 }

 int main(void) {
-    int fd = make_listen_socket(port);
-    epoll_run(fd, handle_request);
+    dpumesh_init(&dpu_ctx, app, worker_id);
+    struct event_base *base = event_base_new();
+    dpumesh_adapter_init(base, dpu_ctx, handle_request);
+    event_base_dispatch(base);
+    event_base_free(base);
 }
```

### Gateway service (업스트림 호출이 있는 서비스)

위 변경에 추가:

```diff
-    conn_start_upstream(client, host, port, "GET", path, ctx);
+    dpumesh_conn_upstream(client, dpu_ctx, "GET", url, ctx);

-    g_upstream_handler = handle_upstream_response;
+    dpumesh_adapter_set_upstream_handler(handle_upstream_response);
```

핸들러 시그니처 변경 없음 — 비즈니스 로직 수정 0줄.

## 참고

### `dpumesh_ctx_t` 내부 구조

모든 DPUmesh API의 첫 번째 인자. 워커가 SHM에 접근하기 위한 상태를 캡슐화한다.

```c
struct dpumesh_ctx {
    char        app_name[64];       // 서비스 이름 ("frontend")
    char        worker_id[128];     // 워커 식별자 ("frontend-worker-0")
    int         pod_id;             // PodRegistry에서 할당받은 고유 ID
    int         timer_fd;           // timerfd (notify용)
    uint32_t    next_req_id;        // 요청 ID 카운터

    buffer_pool_t tx_header, tx_body;   // 송신 버퍼 풀 (SHM mmap)
    buffer_pool_t rx_header, rx_body;   // 수신 버퍼 풀 (SHM mmap)
    desc_ring_t   tx_sq, rx_sq;         // 디스크립터 링 (SHM mmap)
};
```

서비스 코드에서는 `dpumesh_ctx_t *ctx` 포인터만 다루며 내부 필드에 직접 접근하지 않는다.

### DOCA 전환 시 변경 범위

`dpumesh_ctx_t`는 데이터 경로 추상화이므로, DOCA 구현에서도 동일한 구조를 유지한다.
내부 필드만 교체되고 서비스 코드는 변경 없음.

| 필드 | 현재 (SHM) | DOCA |
|------|-----------|------|
| `app_name`, `worker_id`, `pod_id` | 동일 | 동일 |
| `next_req_id` | 동일 | 동일 |
| `tx/rx_header`, `tx/rx_body` | `/dev/shm` mmap | `doca_mmap` + `doca_buf` |
| `tx_sq`, `rx_sq` | SHM 기반 링 버퍼 | `doca_dma` work queue |
| `timer_fd` | timerfd (1ms 폴링) | `doca_pe` completion fd |
