# Memory Free Issue: SHM → HW DMA 전환 시 버퍼 해제 타이밍

## 문제 요약

현재 SHM 시뮬레이션에서는 `memcpy`가 동기적이므로 데이터를 읽은 즉시 `bp_free()`를 호출할 수 있다.
HW DMA에서는 비동기 전송이므로 **DMA 완료가 확인된 후에만** buffer slot을 free할 수 있다.

## Free가 필요한 대상: descriptor가 아니라 data buffer

TX 경로에서 free 대상은 2개:
- **TX Header Pool slot** — header JSON이 담긴 슬롯
- **TX Body Pool slot** — body 데이터가 담긴 슬롯

```
Host가 요청을 보낼 때:

1. TX Header Pool에서 slot 할당 → header JSON 기록
2. TX Body Pool에서 slot 할당 → body 기록
3. TX SQ에 descriptor 삽입 (slot 번호 포함)
4. DPA가 descriptor를 읽음
5. DPA가 header slot에서 DMA read → DPU로 복사
6. DPA가 body slot에서 DMA read → 목적지 RX pool로 복사
7. DMA 완료
   → 이제서야 TX Header slot free 가능
   → 이제서야 TX Body slot free 가능
   → 이제서야 descriptor slot 재사용 가능
```

## TX 측 vs RX 측

| | 송신 측 (TX buffer) | 수신 측 (RX buffer) |
|---|---|---|
| **할당 시점** | `dpumesh_send()` / `dpumesh_respond()` | DPA가 DMA로 기록 |
| **사용 중** | DPA가 DMA read 하는 동안 | App이 데이터를 처리하는 동안 |
| **free 조건** | DPA로부터 completion 수신 | App 콜백이 데이터 소비 완료 |
| **free 주체** | Host의 completion callback | `dpumesh_poll()` 내부 (현재와 동일) |

- **RX 측**: DPA가 RX buffer에 DMA write를 완료한 뒤 RX SQ에 descriptor를 넣으므로, Host가 RX SQ에서 꺼낼 시점엔 이미 DMA가 끝난 상태. 현재 `dpumesh_poll()` 패턴 유지 가능.
- **TX 측**: 새로운 completion 메커니즘이 필요함.

## Completion 메시지 설계 (안)

```c
// DPA → Host completion 메시지
struct dma_completion {
    uint32_t type;              // COMCH_MSG_TYPE_DMA_COMPLETED
    uint32_t req_id;            // 어떤 요청의 completion인지
    int32_t  header_pool_slot;  // free할 TX header slot
    int32_t  body_pool_slot;    // free할 TX body slot
};
```

## 흐름

```
Host (송신)                     DPA                        Host (수신)

tx_header[3]에 write
tx_body[7]에 write
desc = {
  header_slot=3,
  body_slot=7,
  req_id=42,
  valid=1
}
  ── desc via TX SQ ──────►  polling으로 감지
                              DMA: tx_header[3] → rx_header[5]
                              DMA: tx_body[7] → rx_body[12]
                              DMA 완료
                                │
  ◄── completion{           ◄──┤──► RX SQ에 desc 삽입 ──►
        req_id=42,              │    {header_slot=5,        pe_progress()
        header_slot=3,          │     body_slot=12}         callback 호출
        body_slot=7             │                           app이 data 처리
      }                         │                           bp_free(rx_header,5)
                                │                           bp_free(rx_body,12)
pe_progress()
callback에서:
  bp_free(tx_header, 3) ✓
  bp_free(tx_body, 7) ✓
```

## 구현 옵션

### Option A: Descriptor에 done 플래그 추가

```c
struct dma_desc {
    ...
    volatile uint8_t valid;   // Host → DPA: "읽어가라"
    volatile uint8_t done;    // DPA → Host: "읽어갔다, free해도 됨"
};
```
DPA가 DMA 완료 후 PCIe write로 `done=1` 설정. Host는 다음 desc 사용 전에 확인.

### Option B: Completion Ring (역방향 ring)

TX SQ와 별도로 CQ(Completion Queue)를 두어, DPA가 완료된 slot 정보를 넣고 Host가 꺼내서 free.

### Option C: comch_producer + imm data (기존 코드 방향)

`dpa_common.h`에 이미 정의된 `comch_dma_comp_msg` 확장. DPA가 DMA 완료 후 comch producer로 Host에 completion 메시지 전송.

## 서비스 코드에는 영향 없음

`dpumesh_send()`/`dpumesh_respond()`는 내부에서 user data를 TX slot에 memcpy하고 즉시 return.
호출자의 버퍼는 즉시 재사용 가능 (socket write와 동일한 시맨틱).
TX slot의 free는 API 내부에서 completion 기반으로 처리 — 서비스 코드는 모른다.
