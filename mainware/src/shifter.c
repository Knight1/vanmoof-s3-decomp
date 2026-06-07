/*
 * shifter.c — VanMoof S3 mainware shifter / drivetrain Modbus master.
 *
 * The master side of the flagship shifterware: a Modbus-RTU master to the
 * eShifter / MT-shifter module (slave 0x20) over the USART3 inter-module bus
 * (9600 baud). Mirrors the structure of battery.c (the BMS Modbus master on the
 * other bus) but on a distinct bus/context. Behaviour-equivalent reconstruction
 * of the live disassembly: exact control flow, ctx/frame offsets + widths,
 * Modbus framing/CRC, register numbers, state numbers and log strings (every
 * literal-pool string re-resolved byte-for-byte from the OEM image).
 *
 * Memory model (resolved from the OEM literal pool):
 *   g_shift_modbus_ctx 0x20005E3C  master byte-SM state [0], RX byte counter [2],
 *                                   bus ring handle [0xE74], txn-pump state [0xE78],
 *                                   request/response PDU frame [0xE7C], func [0xE7D]
 *   g_modbus_crc       0x2000008C  u16 Modbus CRC-16 accumulator (init 0xFFFF,
 *                                   poly 0xA001); +2 == the queue/RX-timeout slot
 *   g_shift_rx_timeout 0x2000008E  RX-flush one-shot scheduler slot (== g_modbus_crc+2)
 *   g_shifter_sm       0x2000001C  control-SM slot-record: +1 step, +3/4/5/7/9/0xA
 *                                   scheduler slots, +6 substate, +8 current gear
 *   g_shifter_ctx      0x2000019C  larger subsystem state: +0 update step, +1 commit,
 *                                   +2 result, +4 active flag, +8 pack header,
 *                                   +0x14 image len, +0x30 prog offset, +0x34/0x35
 *                                   retries, +0x37 seq-poll sub-step, +0x38 link-fail,
 *                                   +0x39 consecutive-comm-fail
 *   g_bus_ring_slots   0x20006E00  shared 4-entry frame-ring descriptor pool
 *   g_usart3_handle    0x20009824  USART3 device handle ([0]->USART3 @ 0x40004800)
 *   g_usart3_rings     0x2000094C  byte ring holder: +0xC1C TX ring, +0xC20 RX ring
 *   G_APP_CTX          0x200083A8  the session context (== g_app_state.ctx_sub):
 *                                   the SMs' ctx param + the func-3 reply-register
 *                                   destination (reg N -> +0x51E + N*2)
 *   g_log/io vtable    0x20009D98  [0] = g_log_func, [1] = RX-flush byte sink
 */

#include <stdint.h>
#include <string.h>

#include "app.h"        /* maybe_set_state_if_unlocked, maybe_get_bike_state */
#include "app_state.h"  /* g_app_state, struct session_ctx (ctx_sub @ +0x2DC) */
#include "flash.h"      /* pack_validate */
#include "log.h"        /* g_log_func, log_print_timestamp_prefix, log_func_t */
#include "scheduler.h"  /* scheduler_alloc/release/start/slot_is_idle/set_timer_name */
#include "sensor.h"     /* supply_voltage_read */
#include "systick.h"    /* systick_delay */
#include "util.h"       /* ringbuf_t, ringbuf_get_byte, ringbuf_push_byte */
#include "watchdog.h"   /* watchdog_kick */
#include "shifter.h"

/* ── shifter driver SRAM state (pinned; accessed by byte offset) ───────────── */
#define g_shift_modbus_ctx       ((uint8_t *)0x20005E3Cu)
#define g_modbus_crc             ((uint8_t *)0x2000008Cu)
#define g_shift_rx_timeout_slot  (*(uint8_t *)0x2000008Eu) /* == g_modbus_crc[2] */
#define g_shifter_sm             ((uint8_t *)0x2000001Cu)
#define g_shifter_ctx            ((uint8_t *)0x2000019Cu)
#define g_bus_ring_slots         ((uint8_t *)0x20006E00u)
#define g_shift_aux_state        ((uint8_t *)0x20006DC0u)
#define g_usart3_handle          ((uint8_t *)0x20009824u)
#define g_usart3_rings           ((uint8_t *)0x2000094Cu)
#define G_APP_CTX                ((uint8_t *)0x200083A8u)

/* Scheduler slot the `shifterstatus` console command parks its dump task in
 * (SCHED_SLOT_REC+1, in the console-state region); each dump releases it. */
#define SHIFTERSTATUS_TASK_SLOT  ((uint8_t *)0x2000010Fu)

/* GPIO ports driving the shifter rail/enable + update power-cycle path. */
#define GPIOD_BASE   0x40020C00u   /* PD7 (0x80)    shifter 24V rail */
#define GPIOE_BASE   0x40020400u   /* PE14 (0x4000) shifter enable    */

/* Staged eShifter firmware image flash base (pack_validate region + the
 * func-0x10 write-multiple data source). */
#define SHIFTER_IMAGE_BASE   0x08010000u

/* The log/IO vtable at 0x20009D98: [0] is g_log_func (log.h); [1] is the raw
 * byte sink the RX-flush path streams each drained byte through. */
typedef void (*io_fn_t)();
#define G_LOG_IO_VTABLE  ((io_fn_t *)0x20009D98u)

/* ── cross-module helpers (defined elsewhere; K&R-extern to dodge the varied
 *    arg styles, exactly as battery.c does; all gc-section out anyway) ──────── */
extern int  HAL_GPIO_WritePin();
extern int  HAL_GPIO_ReadPin();
extern int  state_flags_set();
extern int  state_flags_clear();
extern int  power_state_get_clamped();
extern int  modem_info_ready();
extern int  testmode_command_dispatch();
extern int  save_state_record_to_eeprom();
extern int  ssp_ble_enqueue_tx_packet();
extern int  amp_volume_brownout_apply();
extern void matrix_set_corner_led();
extern int  state_table_ptr_get();           /* writes a record ptr to *out */
extern void announce_records_reset();                   /* state-record reset(mask)    */
extern uint8_t gpio_pc0_is_low(void);            /* manual-shift button state A */
extern void usart3_init(void);                /* 0x08033358 */
extern void hal_usart_reinit(void *handle);   /* 0x08026d5a */
extern void obj_set_field34();                /* lighting/display teardown   */
extern void obj_set_field38();
extern void led_channel3_set_brightness();

/* ── forward declarations (this module's own members) ─────────────────────── */
uint32_t bus_queue_init(void *buf, uint16_t capacity, uint16_t elem_size, void **out);
uint32_t bus_queue_push(void *handle, const void *src);
uint32_t bus_queue_pop(void *handle, void *dst);
uint32_t bus_queue_reset(void *handle);
uint32_t bus_queue_peek(void *handle, uint16_t *out);

void     shifter_crc_reset(void);
uint16_t shifter_crc_update(uint16_t byte);
uint16_t shifter_crc_value(void);
void     modbus_frame_flush(void);
void     shift_rx_flush_timeout_cb(void);
void     shifter_uart_tx_byte(uint8_t b);
void     shifter_uart_tx_buf(const uint8_t *buf, uint32_t count);
int      shifter_uart_rx_byte(uint8_t *out);
uint32_t shifter_modbus_rtu_step(uint8_t *frame);

uint32_t shifter_modbus_pump(void);
int      shifter_bus_reset(void);
uint16_t shift_bus_peek_response(void);
void     shift_aux_state_reset(void);
void     shifter_response_unpack(uint8_t *frame);

void     shifter_usart3_reinit(void);
void     shifter_send_gear(uint8_t gear);
uint32_t shifter_seq_status_poll_step(void);
void     shifter_control_step(uint8_t *ctx);
void     shifter_update_sm_step(uint8_t *ctx, int txn_result);

/* ════════════════════════════════════════════════════════════════════════════
 * Section A — generic frame-ring queue (shared bus-master primitives)
 * ════════════════════════════════════════════════════════════════════════════
 * 16-byte descriptor (drawn from g_bus_ring_slots):
 *   [+0] void* buf  [+4] u16 capacity  [+6] u16 elem_size
 *   [+8] u16 count  [+0xA] u16 wr      [+0xC] u16 rd
 */

/*
 * bus_queue_init @ 0x08037e2c
 * Allocate + initialise one of the 4 shared frame-ring descriptors. Returns 0 ok,
 * 1 on NULL out/buf, 2 when the descriptor pool is exhausted.
 */
uint32_t bus_queue_init(void *buf, uint16_t capacity, uint16_t elem_size, void **out)
{
    uint32_t i;
    uint8_t *desc;

    if (out == (void **)0) {
        return 1;
    }
    *out = (void *)0;
    if (buf == (void *)0) {
        return 1;
    }

    /* first-call init: clear the in-use marker (+4) of all 4 descriptors */
    if (g_bus_ring_slots[0] == 0) {
        for (i = 0; i < 4; i++) {
            uint8_t *p = g_bus_ring_slots + i * 0x10 + 4;
            p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0;
        }
        g_bus_ring_slots[0] = 1;
    }

    /* find the first free descriptor (in-use marker == 0) */
    desc = (uint8_t *)0;
    for (i = 0; i < 4; i++) {
        if (*(int *)(g_bus_ring_slots + i * 0x10 + 4) == 0) {
            desc = (g_bus_ring_slots + 4) + i * 0x10;   /* descriptor word[0] */
            break;
        }
    }
    if (i == 4) {
        return 2;
    }

    *(uint32_t *)(desc + 0x0) = (uint32_t)buf;
    *(uint16_t *)(desc + 0x4) = capacity;
    *(uint16_t *)(desc + 0x6) = elem_size;
    *(uint16_t *)(desc + 0x8) = 0;       /* count */
    *(uint16_t *)(desc + 0xA) = 0;       /* wr    */
    *(uint16_t *)(desc + 0xC) = 0;       /* rd    */
    *out = (void *)desc;
    return 0;
}

/*
 * bus_queue_push @ 0x08037ea0
 * Enqueue one element (elem_size bytes) at the write cursor; wrap + count++.
 * Returns 0 ok, 3 when full, 1 on NULL handle.
 */
uint32_t bus_queue_push(void *handle, const void *src)
{
    uint8_t *h = (uint8_t *)handle;
    uint16_t wr, capacity, elem_size;

    if (h == (uint8_t *)0) {
        return 1;
    }
    capacity  = *(uint16_t *)(h + 0x4);
    elem_size = *(uint16_t *)(h + 0x6);

    if (*(uint16_t *)(h + 0x8) < capacity) {
        wr = *(uint16_t *)(h + 0xA);
        *(uint16_t *)(h + 0xA) = wr + 1;
        memcpy((void *)(*(uint32_t *)(h + 0x0) + (uint32_t)elem_size * wr),
               src, (uint32_t)elem_size);
        if (capacity <= *(uint16_t *)(h + 0xA)) {
            *(uint16_t *)(h + 0xA) = 0;
        }
        *(uint16_t *)(h + 0x8) = *(uint16_t *)(h + 0x8) + 1;
        return 0;
    }
    return 3;
}

/*
 * bus_queue_pop @ 0x08037ede
 * Dequeue one element from the read cursor into dst; wrap + count--.
 * Returns 0 ok, 4 when empty, 1 on NULL handle/dst.
 */
uint32_t bus_queue_pop(void *handle, void *dst)
{
    uint8_t *h = (uint8_t *)handle;
    uint16_t rd, capacity, elem_size;

    if (h == (uint8_t *)0) {
        return 1;
    }
    if (dst == (void *)0) {
        return 1;
    }
    if (*(uint16_t *)(h + 0x8) == 0) {
        return 4;
    }
    capacity  = *(uint16_t *)(h + 0x4);
    elem_size = *(uint16_t *)(h + 0x6);

    rd = *(uint16_t *)(h + 0xC);
    *(uint16_t *)(h + 0xC) = rd + 1;
    memcpy(dst, (void *)(*(uint32_t *)(h + 0x0) + (uint32_t)elem_size * rd),
           (uint32_t)elem_size);
    if (capacity <= *(uint16_t *)(h + 0xC)) {
        *(uint16_t *)(h + 0xC) = 0;
    }
    *(uint16_t *)(h + 0x8) = *(uint16_t *)(h + 0x8) - 1;
    return 0;
}

/*
 * bus_queue_reset @ 0x08037f22
 * Reset a handle to empty (count/wr/rd = 0). Returns 0 ok, 1 on NULL handle.
 */
uint32_t bus_queue_reset(void *handle)
{
    uint8_t *h = (uint8_t *)handle;

    if (h != (uint8_t *)0) {
        *(uint16_t *)(h + 0x8) = 0;
        *(uint16_t *)(h + 0xA) = 0;
        *(uint16_t *)(h + 0xC) = 0;
        return 0;
    }
    return 1;
}

/*
 * bus_queue_peek @ 0x08037f34
 * Read the live element count (offset +8) into *out without consuming. Returns
 * 0 ok, 1 on NULL handle/out.
 */
uint32_t bus_queue_peek(void *handle, uint16_t *out)
{
    uint8_t *h = (uint8_t *)handle;

    if (h == (uint8_t *)0) {
        return 1;
    }
    if (out != (uint16_t *)0) {
        *out = *(uint16_t *)(h + 8);
        return 0;
    }
    return 1;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Section B — Modbus CRC-16 + USART3 byte I/O
 * ════════════════════════════════════════════════════════════════════════════
 */

/* shifter_crc_reset @ 0x080373b8 — init the Modbus CRC-16 accumulator to 0xFFFF. */
void shifter_crc_reset(void)
{
    *(uint16_t *)g_modbus_crc = 0xFFFFu;
}

/*
 * shifter_crc_update @ 0x080373c8
 * Fold one byte into the Modbus CRC-16/RTU accumulator (reflected, poly 0xA001).
 */
uint16_t shifter_crc_update(uint16_t byte)
{
    uint16_t *crc = (uint16_t *)g_modbus_crc;
    int bit;

    *crc ^= byte;
    for (bit = 8; bit != 0; bit--) {
        if ((*crc & 1u) == 0) {
            *crc = (uint16_t)(*crc >> 1);
        } else {
            *crc = (uint16_t)((*crc >> 1) ^ 0xA001u);
        }
    }
    return *crc;
}

/* shifter_crc_value @ 0x08037404 — return the current CRC-16 accumulator. */
uint16_t shifter_crc_value(void)
{
    return *(uint16_t *)g_modbus_crc;
}

/*
 * modbus_frame_flush @ 0x08037428
 * Frame-complete cleanup: release the RX-timeout one-shot slot and clear the
 * master byte-SM state byte.
 */
void modbus_frame_flush(void)
{
    scheduler_release(&g_shift_rx_timeout_slot);
    g_shift_modbus_ctx[0] = 0;
}

/*
 * shift_rx_flush_timeout_cb @ 0x08037411
 * RX-flush watchdog: armed (0x5DC ticks) at the start of every transmit. On
 * expiry it releases the RX-timeout slot and forces the master SM into state 8
 * (the flush/abort entry), so a stalled response can't wedge the bus.
 */
void shift_rx_flush_timeout_cb(void)
{
    scheduler_release(&g_shift_rx_timeout_slot);
    g_shift_modbus_ctx[0] = 8;
}

/*
 * shifter_uart_tx_byte @ 0x08036248
 * Locked single-byte TX into the USART3 TX ring: mask CR1.TXEIE (0x80) with a
 * DSB/ISB, push the byte, re-enable TXEIE. USART3 reg block via g_usart3_handle.
 */
void shifter_uart_tx_byte(uint8_t b)
{
    volatile uint32_t *usart3 = *(volatile uint32_t * volatile *)g_usart3_handle;

    usart3[0xC / 4] &= ~0x80u;
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    ringbuf_push_byte(*(ringbuf_t **)(g_usart3_rings + 0xC1Cu), b);

    usart3 = *(volatile uint32_t * volatile *)g_usart3_handle;
    usart3[0xC / 4] |= 0x80u;
}

/*
 * shifter_uart_tx_buf @ 0x08036280
 * Push `count` bytes from `buf` to the USART3 TX ring (func-0x10 payload).
 */
void shifter_uart_tx_buf(const uint8_t *buf, uint32_t count)
{
    while (count != 0) {
        shifter_uart_tx_byte(*buf);
        count = (count - 1) & 0xFFFFu;
        buf++;
    }
}

/*
 * shifter_uart_rx_byte @ 0x08036298
 * Locked single-byte RX from the USART3 RX ring: mask CR1.RXNEIE (0x20) with a
 * DSB/ISB, pop a byte, re-enable RXNEIE. The ring getter's status (1 = byte
 * produced, 0 = empty) is the return value; the master SM relies on it.
 */
int shifter_uart_rx_byte(uint8_t *out)
{
    volatile uint32_t *usart3 = *(volatile uint32_t * volatile *)g_usart3_handle;
    uint32_t rc;

    usart3[0xC / 4] &= ~0x20u;
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    rc = ringbuf_get_byte(*(ringbuf_t **)(g_usart3_rings + 0xC20u), out);

    usart3 = *(volatile uint32_t * volatile *)g_usart3_handle;
    usart3[0xC / 4] |= 0x20u;
    return (int)rc;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Section C — Modbus-RTU byte-level master state machine
 * ════════════════════════════════════════════════════════════════════════════
 */

/*
 * shifter_modbus_rtu_step @ 0x08037440
 * Modbus-RTU master byte state machine for the eShifter slave (0x20) on USART3.
 * Builds + CRC-frames a request (func 3 read / 6 write-single / 0x10
 * write-multiple) and pumps the response byte-by-byte, validating the
 * function-code echo (exception bit 0x80) and the trailing CRC-16.
 *
 * frame == &g_shift_modbus_ctx[0xE7C]:
 *   [0] slave 0x20  [1] func  [2..3] reg (native u16; sent big-endian hi,lo)
 *   [4..] request payload  [0x84] write byte-count  [0x85+] response data
 *   [0x105] response payload length  [0x106] Modbus exception flag
 * Master-SM state = g_shift_modbus_ctx[0], u16 byte counter = g_shift_modbus_ctx[2].
 *
 * Returns: 0 while busy/still pumping (no byte yet, or a byte consumed but the
 *          frame is incomplete); 1 when a frame completed OK (response received
 *          and CRC folded to 0, no exception); 2 after a final flush (state-8
 *          entry); 3 on a Modbus exception (frame[0x106] set, frame complete).
 */
uint32_t shifter_modbus_rtu_step(uint8_t *frame)
{
    uint8_t  rx[5];
    uint16_t flush_cnt;
    uint16_t crc;
    uint8_t  func;
    int16_t  prev_cnt;
    uint16_t cnt;
    uint32_t busy;
    uint32_t state;

    if (g_shift_modbus_ctx[0] == 8) {
        modbus_frame_flush();
        return 2;
    }

    switch (g_shift_modbus_ctx[0]) {

    case 0:
        shifter_crc_reset();
        frame[0x106] = 0;

        if (g_shift_rx_timeout_slot == SCHED_SLOT_NONE) {
            uint8_t slot = scheduler_alloc();
            g_shift_rx_timeout_slot = slot;
            scheduler_set_timer_name(slot, 0, "sbmodbus_tmr");
        }

        /* drain any stale RX bytes before transmitting; echo each via the sink */
        flush_cnt = 0;
        while ((shifter_uart_rx_byte(rx) != 0) &&
               ((flush_cnt = (uint16_t)(flush_cnt + 1)) < 0x200u)) {
            watchdog_kick();
            G_LOG_IO_VTABLE[1](rx[0]);
        }
        if (10 < flush_cnt) {
            log_print_timestamp_prefix();
            g_log_func("flush Shift rx %d\r\n", flush_cnt);
        }

        scheduler_start(g_shift_rx_timeout_slot, 0x5DC,
                        (sched_cb_t)shift_rx_flush_timeout_cb);

        func = frame[1];
        if (func == 6) {                /* write single register */
            shifter_uart_tx_byte(frame[0]);                        shifter_crc_update(frame[0]);
            shifter_uart_tx_byte(frame[1]);                        shifter_crc_update(frame[1]);
            shifter_uart_tx_byte((uint8_t)(*(uint16_t *)(frame + 2) >> 8));
            shifter_crc_update((uint8_t)(*(uint16_t *)(frame + 2) >> 8));
            shifter_uart_tx_byte(frame[2]);                        shifter_crc_update(frame[2]);
            shifter_uart_tx_byte(frame[5]);                        shifter_crc_update(frame[5]);
            shifter_uart_tx_byte(frame[4]);                        shifter_crc_update(frame[4]);
            crc = shifter_crc_value();
            shifter_uart_tx_byte((uint8_t)(crc & 0xFF));           /* CRC lo first */
            shifter_uart_tx_byte((uint8_t)((crc & 0xFFFF) >> 8));
            busy = 1;
            g_shift_modbus_ctx[0] = 1;
        } else if (func == 0x10) {      /* write multiple registers */
            shifter_uart_tx_byte(frame[0]);                        shifter_crc_update(frame[0]);
            shifter_uart_tx_byte(frame[1]);                        shifter_crc_update(frame[1]);
            shifter_uart_tx_byte((uint8_t)(*(uint16_t *)(frame + 2) >> 8));
            shifter_crc_update((uint8_t)(*(uint16_t *)(frame + 2) >> 8));
            shifter_uart_tx_byte(frame[2]);                        shifter_crc_update(frame[2]);
            shifter_uart_tx_byte(0);                               shifter_crc_update(0);
            shifter_uart_tx_byte((uint8_t)(frame[0x84] >> 1));     /* register count */
            shifter_crc_update((uint8_t)(frame[0x84] >> 1));
            shifter_uart_tx_byte(frame[0x84]);                     /* byte count   */
            shifter_crc_update(frame[0x84]);
            shifter_uart_tx_buf(frame + 4, frame[0x84]);
            *(uint16_t *)(g_shift_modbus_ctx + 2) = 0;
            while ((uint32_t)*(uint16_t *)(g_shift_modbus_ctx + 2) < (uint32_t)frame[0x84]) {
                shifter_crc_update(frame[*(uint16_t *)(g_shift_modbus_ctx + 2) + 4]);
                *(int16_t *)(g_shift_modbus_ctx + 2) =
                    (int16_t)(*(int16_t *)(g_shift_modbus_ctx + 2) + 1);
            }
            crc = shifter_crc_value();
            shifter_uart_tx_byte((uint8_t)(crc & 0xFF));
            shifter_uart_tx_byte((uint8_t)((crc & 0xFFFF) >> 8));
            busy = 1;
            g_shift_modbus_ctx[0] = 1;
        } else if (func == 3) {         /* read holding registers */
            shifter_uart_tx_byte(frame[0]);                        shifter_crc_update(frame[0]);
            shifter_uart_tx_byte(frame[1]);                        shifter_crc_update(frame[1]);
            shifter_uart_tx_byte((uint8_t)(*(uint16_t *)(frame + 2) >> 8));
            shifter_crc_update((uint8_t)(*(uint16_t *)(frame + 2) >> 8));
            shifter_uart_tx_byte(frame[2]);                        shifter_crc_update(frame[2]);
            shifter_uart_tx_byte(0);                               shifter_crc_update(0);
            shifter_uart_tx_byte(frame[0x84]);                     shifter_crc_update(frame[0x84]);
            crc = shifter_crc_value();
            shifter_uart_tx_byte((uint8_t)(crc & 0xFF));
            shifter_uart_tx_byte((uint8_t)((crc & 0xFFFF) >> 8));
            busy = 1;
            g_shift_modbus_ctx[0] = 1;
        } else {
            busy = 1;
        }
        break;

    case 1:                              /* echo: first byte = our slave addr */
        busy = (uint32_t)shifter_uart_rx_byte(rx);
        if (busy == 0) {
            busy = 1;
        } else if (frame[0] == rx[0]) {
            shifter_crc_reset();
            shifter_crc_update(rx[0]);
            g_shift_modbus_ctx[0] = 2;
        }
        break;

    case 2:                              /* function-code byte (exception bit 0x80) */
        busy = (uint32_t)shifter_uart_rx_byte(rx);
        if (busy == 0) {
            busy = 1;
        } else {
            if ((int8_t)rx[0] < 0) {
                frame[0x106] = 1;
                g_log_func("SH error rx \r\n");
            }
            if (frame[1] == (rx[0] & 0x7F)) {
                shifter_crc_update(rx[0]);
                func = frame[1];
                if (func == 3) {
                    g_shift_modbus_ctx[0] = 5;       /* byte-count next */
                }
                if (func == 6) {
                    frame[0x105] = 4;
                    *(uint16_t *)(g_shift_modbus_ctx + 2) = 0;
                    g_shift_modbus_ctx[0] = 3;
                }
                if (func == 0x10) {
                    frame[0x105] = 4;
                    *(uint16_t *)(g_shift_modbus_ctx + 2) = 0;
                    g_shift_modbus_ctx[0] = 4;
                }
            }
        }
        break;

    case 3:
    case 4:
    case 6:                              /* collect fixed-length payload */
        busy = (uint32_t)shifter_uart_rx_byte(rx);
        if (busy == 0) {
            busy = 1;
        } else {
            uint8_t exp = frame[0x105];
            shifter_crc_update(rx[0]);
            cnt = *(uint16_t *)(g_shift_modbus_ctx + 2);
            if ((cnt < exp) && (cnt < 0x80)) {
                *(uint16_t *)(g_shift_modbus_ctx + 2) =
                    (uint16_t)(*(uint16_t *)(g_shift_modbus_ctx + 2) + 1);
                frame[cnt + 0x85] = rx[0];
            }
            if ((uint32_t)exp == (uint32_t)*(uint16_t *)(g_shift_modbus_ctx + 2)) {
                g_shift_modbus_ctx[0] = 7;
            }
        }
        break;

    case 5:                              /* read: byte-count byte */
        busy = (uint32_t)shifter_uart_rx_byte(rx);
        if (busy == 0) {
            busy = 1;
        } else {
            shifter_crc_update(rx[0]);
            frame[0x105] = rx[0];
            *(uint16_t *)(g_shift_modbus_ctx + 2) = 0;
            if (rx[0] == 0) {
                g_shift_modbus_ctx[0] = 7;
            } else {
                g_shift_modbus_ctx[0] = 6;
            }
        }
        break;

    case 7:                              /* trailing CRC; verify on completion */
        busy = (uint32_t)shifter_uart_rx_byte(rx);
        if (busy == 0) {
            busy = 1;
        } else {
            shifter_crc_update(rx[0]);
            prev_cnt = *(int16_t *)(g_shift_modbus_ctx + 2);
            *(int16_t *)(g_shift_modbus_ctx + 2) = (int16_t)(prev_cnt + 1);
            if (((uint16_t)(prev_cnt + 1) == (uint16_t)(frame[0x105] + 2)) &&
                (shifter_crc_value() == 0)) {
                modbus_frame_flush();
                busy = 0;
                if (frame[0x106] != 0) {
                    return 3;
                }
            }
        }
        break;

    default:
        busy = 1;
        break;
    }

    state = (busy ^ 1) & 0xFF;
    return state;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Section D — submit / queue bring-up / transaction pump / response decode
 * ════════════════════════════════════════════════════════════════════════════
 */

/*
 * modbus_shift_submit @ 0x080378a0
 * Enqueue a built Modbus PDU into the shifter bus ring (handle @ +0xE74).
 * Returns 1 on queue-full/error, else 0.
 */
int modbus_shift_submit(void *frame)
{
    int rc = (int)bus_queue_push(*(void **)(g_shift_modbus_ctx + 0xE74), frame);
    return (rc != 0);
}

/*
 * smodbus_queue_timer_init @ 0x08037980
 * One-time bring-up: allocate the request/response frame-ring (0xE entries of
 * 0x108 bytes, backing store at +4, handle at +0xE74) and the queue scheduler
 * slot (g_modbus_crc+2, named "smodbus_tmr"). Logs "Error queue" on ring-alloc
 * failure. Returns true once the slot is allocated.
 */
int smodbus_queue_timer_init(void)
{
    uint8_t slot;

    if ((int)bus_queue_init((void *)(g_shift_modbus_ctx + 4), 0xE, 0x108,
                            (void **)(g_shift_modbus_ctx + 0xE74)) != 0) {
        g_log_func("Error queue\r\n");
    }

    if (g_modbus_crc[2] == SCHED_SLOT_NONE) {
        slot = scheduler_alloc();
        g_modbus_crc[2] = slot;
        scheduler_set_timer_name(slot, 0, "smodbus_tmr");
    }
    return g_modbus_crc[2] != SCHED_SLOT_NONE;
}

/*
 * shifter_modbus_pump @ 0x080378f4
 * Per-call 4-state transaction driver (txn-SM state @ +0xE78):
 *   0 = pop the next queued request into the in-flight frame (+0xE7C)
 *   1 = drive the byte-level master SM; 1 => advance to 2, status 2/3 => 3
 *   2 = success: if func (+0xE7D) == 3, decode the reply into the app context
 *   3 = error/abort: rewind to idle
 * Returns the master step's status propagated to the link monitor.
 */
uint32_t shifter_modbus_pump(void)
{
    int rc;
    uint32_t status = (uint32_t)g_shift_modbus_ctx[0xE78];

    switch (status) {
    case 0:
        rc = (int)bus_queue_pop(*(void **)(g_shift_modbus_ctx + 0xE74),
                                (void *)(g_shift_modbus_ctx + 0xE7C));
        if (rc == 0) {
            g_shift_modbus_ctx[0xE78] = 1;
            status = 0;
        }
        break;

    case 1:
        status = shifter_modbus_rtu_step((uint8_t *)(g_shift_modbus_ctx + 0xE7C));
        if (status == 1) {
            g_shift_modbus_ctx[0xE78] = 2;
        }
        if (((status - 2) & 0xff) < 2) {     /* status == 2 or 3 */
            g_shift_modbus_ctx[0xE78] = 3;
        }
        break;

    case 2:
        if (*(int8_t *)(g_shift_modbus_ctx + 0xE7D) == 3) {
            shifter_response_unpack((uint8_t *)(g_shift_modbus_ctx + 0xE7C));
        }
        status = 0;
        g_shift_modbus_ctx[0xE78] = 0;
        break;

    case 3:
        status = 0;
        g_shift_modbus_ctx[0xE78] = 0;
        break;

    default:
        status = 0;
        break;
    }
    return status;
}

/*
 * shifter_bus_reset @ 0x080378dc
 * Reset the shifter bus ring (count/wr/rd) via the handle at +0xE74.
 * Returns true on success.
 */
int shifter_bus_reset(void)
{
    return (int)bus_queue_reset(*(void **)(g_shift_modbus_ctx + 0xE74)) == 0;
}

/*
 * shift_bus_peek_response @ 0x080378bc
 * Peek the shifter bus ring's live frame count without consuming. Returns the
 * number of frames still queued (0 == the response queue has drained); the
 * update SM uses it to confirm a write block was sent.
 */
uint16_t shift_bus_peek_response(void)
{
    uint16_t cnt = 0;
    bus_queue_peek(*(void **)(g_shift_modbus_ctx + 0xE74), &cnt);
    return cnt;
}

/*
 * shift_aux_state_reset @ 0x08037e04
 * Tear down the shifter aux-state object: zero bytes +0xC/+0x20/+0x34 then clear
 * the linked display/LED state.
 */
void shift_aux_state_reset(void)
{
    g_shift_aux_state[0x0C] = 0;
    g_shift_aux_state[0x20] = 0;
    g_shift_aux_state[0x34] = 0;
    obj_set_field34(0);
    obj_set_field38(0);
    led_channel3_set_brightness(0);
}

/*
 * shifter_response_unpack @ 0x0803dce8
 * Decode a func-3 (read holding registers) reply into the session context
 * (G_APP_CTX). Each register R is stored big-endian at +((R+0x28c)*2+6) (so
 * reg 1 -> +0x520, reg 2 -> +0x522, ...). Side effects while unpacking:
 *   reg 0x10 (and counter +0x338 < 3): seed +0x338 from +0x53C:+0x53E
 *   reg 2:  mirror reported gear (+0x522 low byte) into current-gear (+0x3CC)
 *   reg 6:  copy MT result (+0x52A) into reported version (+0x336)
 * When the `shiftdebug` stream flag (+0x34E) is set, log each register as
 * "MBS 0x%04X 0x%02X " followed by "\r\n".
 */
void shifter_response_unpack(uint8_t *frame)
{
    uint8_t *app = G_APP_CTX;
    uint16_t start = *(uint16_t *)(frame + 2);
    int i;

    for (i = 0; i < (int)(uint32_t)frame[0x105]; i += 2) {
        uint16_t reg = (uint16_t)(start + i / 2);
        *(uint16_t *)(app + (reg + 0x28c) * 2 + 6) =
            (uint16_t)(((uint16_t)frame[0x85 + i] << 8) | frame[0x86 + i]);

        if ((reg == 0x10) && (*(uint32_t *)(app + 0x338) < 3)) {
            *(uint32_t *)(app + 0x338) =
                (uint32_t)((int)*(int16_t *)(app + 0x53e) |
                           ((int)*(int16_t *)(app + 0x53c) << 16));
        }
        if (reg == 2) {
            app[0x3cc] = (uint8_t)*(uint16_t *)(app + 0x522);
        }
        if (reg == 6) {
            *(uint16_t *)(app + 0x336) = *(uint16_t *)(app + 0x52a);
        }
    }

    if (app[0x34e] != 0) {               /* `shiftdebug` register stream on */
        unsigned r;
        watchdog_kick();
        log_print_timestamp_prefix();
        for (r = start; (int)r < (int)(start + (frame[0x105] >> 1)); r++) {
            g_log_func("MBS 0x%04X 0x%02X ", r,
                       *(uint16_t *)(app + (r + 0x28c) * 2 + 6));
        }
        g_log_func("\r\n");
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Section E — control-SM step + subsystem flag accessors
 * ════════════════════════════════════════════════════════════════════════════
 */

/* shifter_sm_get_step @ 0x0802844C — read the control-SM step (g_shifter_sm+1). */
uint8_t shifter_sm_get_step(void)
{
    return g_shifter_sm[1];
}

/* shifter_sm_set_step_13 @ 0x080284A4 — force step 0x0D (MT calibration entry). */
void shifter_sm_set_step_13(void)
{
    g_shifter_sm[1] = 0x0D;
}

/* shifter_sm_set_step_10 @ 0x08029534 — force step 10 (apply gear). */
void shifter_sm_set_step_10(void)
{
    g_shifter_sm[1] = 10;
}

/* shifter_sm_set_step_3 @ 0x0802954C — force step 3 (begin power-up/bring-up). */
void shifter_sm_set_step_3(void)
{
    g_shifter_sm[1] = 3;
}

/* shifter_get_active_flag @ 0x08029558 — link-alive flag (g_shifter_ctx+4). */
uint8_t shifter_get_active_flag(void)
{
    return g_shifter_ctx[4];
}

/* shifter_update_request @ 0x08029528 — kick the update FSM (g_shifter_ctx+0). */
void shifter_update_request(uint8_t arm)
{
    g_shifter_ctx[0] = arm;
}

/* shifter_update_status_get @ 0x08029540 — update result byte (g_shifter_ctx+2). */
uint8_t shifter_update_status_get(void)
{
    return g_shifter_ctx[2];
}

/* ════════════════════════════════════════════════════════════════════════════
 * Section F — firmware-update pre-check gate
 * ════════════════════════════════════════════════════════════════════════════
 */

/*
 * shifter_firmware_update_step @ 0x08032A0C
 * Pre-check gate for a shifter (eShifter / MT) firmware update, all required:
 *   - supply rail above 25000 (supply_voltage_read);
 *   - the staged PACK at flash 0x08010000 validates (pack_validate == 0);
 *   - the PACK type byte matches the attached hardware (0xC1 <-> HW 0x200,
 *     0xC9 <-> HW 0x201, read from ctx+0x520);
 *   - the PACK version (header[1] >> 16) differs from the running version
 *     reported by the shifter (ctx+0x336).
 * Already up to date -> "No need for Shifter update". Brand/HW mismatch ->
 * "Wrong shifter brand file". Otherwise bump ctx+0x334; while it was < 4 request
 * state 0x19 and arm the update, once exhausted log "Shifter out of retries".
 *
 * pack_validate writes a full 0x28-byte (10-word) normalised header into hdr, so
 * the stack buffer must cover all 10 words (matching the OEM `sub sp,#0x28`).
 * ctx == the session context (G_APP_CTX).
 */
void shifter_firmware_update_step(uint8_t *ctx)
{
    uint32_t hdr[10];
    uint8_t  retries;
    int16_t  hw_ver;
    uint8_t  type;

    if (supply_voltage_read() <= 25000) {
        return;
    }
    if (pack_validate(hdr, (uint32_t *)SHIFTER_IMAGE_BASE) != 0) {
        return;
    }

    type   = (uint8_t)(hdr[1] & 0xFF);
    hw_ver = *(int16_t *)(ctx + 0x520);

    if (!((type == 0xC1 && hw_ver == 0x200) ||
          (type == 0xC9 && hw_ver == 0x201))) {
        g_log_func("Wrong shifter brand file\r\n");
        return;
    }
    if (*(uint16_t *)(ctx + 0x336) == (hdr[1] >> 16)) {
        g_log_func("No need for Shifter update\r\n");
        return;
    }

    retries = *(uint8_t *)(ctx + 0x334);
    *(uint8_t *)(ctx + 0x334) = (uint8_t)(retries + 1);
    if (retries < 4) {
        maybe_set_state_if_unlocked(0x19);
        shifter_update_request(1);
    } else {
        g_log_func("Shifter out of retries\r\n");
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Section G — USART3 re-init, gear actuator, sequential status poller
 * ════════════════════════════════════════════════════════════════════════════
 */

/* shifter_usart3_reinit @ 0x080338b4 — re-init/flush the USART3 bus device. */
void shifter_usart3_reinit(void)
{
    hal_usart_reinit((void *)g_usart3_handle);
}

/*
 * shifter_send_gear @ 0x08028458
 * Build + submit a func-6 write to register 2 (the current-gear register):
 * {0x20, 0x06, reg=2, data=gear, count=1}. The OEM gear actuator. On submit
 * failure logs "  ERR MS overflow".
 */
void shifter_send_gear(uint8_t gear)
{
    uint8_t frame[0x110];

    frame[0x00] = 0x20;
    frame[0x01] = 0x06;
    *(uint16_t *)(frame + 0x02) = 2;
    frame[0x84] = 1;
    frame[0x05] = 0;
    frame[0x04] = gear;

    if (modbus_shift_submit(frame) != 0) {
        log_print_timestamp_prefix();
        g_log_func("  ERR MS overflow\r\n");
    }
}

/*
 * shifter_seq_status_poll_step @ 0x080284b0
 * Sequential register-status poller: an 18-state sub-machine (sub-step at
 * g_shifter_ctx+0x37) that func-3 reads regs 1,2,3,4,5,6,0xE,0xF one at a time
 * at a 100 ms cadence, gated on the 'process_tmr' slot (g_shifter_sm+3). Returns
 * 1 only when the full sweep completes. Submit failures log "  ERR MS overflow".
 */
uint32_t shifter_seq_status_poll_step(void)
{
    uint8_t  slot;
    uint8_t  frame[0x110];
    uint32_t ret;

    if (g_shifter_sm[3] == SCHED_SLOT_NONE) {
        slot = scheduler_alloc();
        g_shifter_sm[3] = slot;
        scheduler_set_timer_name(slot, 0x14, "process_tmr");
        g_shifter_ctx[0x37] = 0;
    }

    switch (g_shifter_ctx[0x37]) {
    case 0:
        scheduler_start(g_shifter_sm[3], 100, (sched_cb_t)0);
        g_shifter_ctx[0x37]++;
        ret = 0;
        break;
    case 1:
        ret = 0;
        if (scheduler_slot_is_idle(g_shifter_sm[3]) != 0) g_shifter_ctx[0x37]++;
        break;
    case 2:
        frame[0x00] = 0x20; frame[0x01] = 0x03; *(uint16_t *)(frame + 0x02) = 1; frame[0x84] = 1;
        if (modbus_shift_submit(frame) != 0) g_log_func("  ERR MS overflow\r\n");
        scheduler_start(g_shifter_sm[3], 100, (sched_cb_t)0);
        g_shifter_ctx[0x37]++; ret = 0;
        break;
    case 3:
        ret = 0;
        if (scheduler_slot_is_idle(g_shifter_sm[3]) != 0) g_shifter_ctx[0x37]++;
        break;
    case 4:
        frame[0x00] = 0x20; frame[0x01] = 0x03; *(uint16_t *)(frame + 0x02) = 2; frame[0x84] = 1;
        if (modbus_shift_submit(frame) != 0) g_log_func("  ERR MS overflow\r\n");
        scheduler_start(g_shifter_sm[3], 100, (sched_cb_t)0);
        g_shifter_ctx[0x37]++; ret = 0;
        break;
    case 5:
        ret = 0;
        if (scheduler_slot_is_idle(g_shifter_sm[3]) != 0) g_shifter_ctx[0x37]++;
        break;
    case 6:
        frame[0x00] = 0x20; frame[0x01] = 0x03; *(uint16_t *)(frame + 0x02) = 3; frame[0x84] = 1;
        if (modbus_shift_submit(frame) != 0) g_log_func("  ERR MS overflow\r\n");
        scheduler_start(g_shifter_sm[3], 100, (sched_cb_t)0);
        g_shifter_ctx[0x37]++; ret = 0;
        break;
    case 7:
        ret = 0;
        if (scheduler_slot_is_idle(g_shifter_sm[3]) != 0) g_shifter_ctx[0x37]++;
        break;
    case 8:
        frame[0x00] = 0x20; frame[0x01] = 0x03; *(uint16_t *)(frame + 0x02) = 4; frame[0x84] = 1;
        if (modbus_shift_submit(frame) != 0) g_log_func("  ERR MS overflow\r\n");
        scheduler_start(g_shifter_sm[3], 100, (sched_cb_t)0);
        g_shifter_ctx[0x37]++; ret = 0;
        break;
    case 9:
        ret = 0;
        if (scheduler_slot_is_idle(g_shifter_sm[3]) != 0) g_shifter_ctx[0x37]++;
        break;
    case 10:
        frame[0x00] = 0x20; frame[0x01] = 0x03; *(uint16_t *)(frame + 0x02) = 5; frame[0x84] = 1;
        if (modbus_shift_submit(frame) != 0) g_log_func("  ERR MS overflow\r\n");
        scheduler_start(g_shifter_sm[3], 100, (sched_cb_t)0);
        g_shifter_ctx[0x37]++; ret = 0;
        break;
    case 0x0b:
        ret = 0;
        if (scheduler_slot_is_idle(g_shifter_sm[3]) != 0) g_shifter_ctx[0x37]++;
        break;
    case 0x0c:
        frame[0x00] = 0x20; frame[0x01] = 0x03; *(uint16_t *)(frame + 0x02) = 6; frame[0x84] = 1;
        if (modbus_shift_submit(frame) != 0) g_log_func("  ERR MS overflow\r\n");
        scheduler_start(g_shifter_sm[3], 100, (sched_cb_t)0);
        g_shifter_ctx[0x37]++; ret = 0;
        break;
    case 0x0d:
        ret = 0;
        if (scheduler_slot_is_idle(g_shifter_sm[3]) != 0) g_shifter_ctx[0x37]++;
        break;
    case 0x0e:
        frame[0x00] = 0x20; frame[0x01] = 0x03; *(uint16_t *)(frame + 0x02) = 0xE; frame[0x84] = 1;
        if (modbus_shift_submit(frame) != 0) g_log_func("  ERR MS overflow\r\n");
        scheduler_start(g_shifter_sm[3], 100, (sched_cb_t)0);
        g_shifter_ctx[0x37]++; ret = 0;
        break;
    case 0x0f:
        ret = 0;
        if (scheduler_slot_is_idle(g_shifter_sm[3]) != 0) g_shifter_ctx[0x37]++;
        break;
    case 0x10:
        frame[0x00] = 0x20; frame[0x01] = 0x03; *(uint16_t *)(frame + 0x02) = 0xF; frame[0x84] = 2;
        if (modbus_shift_submit(frame) != 0) g_log_func("  ERR MS overflow\r\n");
        scheduler_start(g_shifter_sm[3], 100, (sched_cb_t)0);
        g_shifter_ctx[0x37]++; ret = 0;
        break;
    case 0x11:
        g_shifter_ctx[0x37] = 0;
        scheduler_release(&g_shifter_sm[3]);
        ret = 1;
        break;
    default:
        ret = 0;
        break;
    }
    return ret;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Section H — link monitor + drivetrain control SM + OTA update SM
 * ════════════════════════════════════════════════════════════════════════════
 */

/*
 * modbus_shifter_link_monitor @ 0x0802945c
 * Per-super-loop shifter service entry. Gated by the link-enable flags
 * (ctx+0x402==1 || ctx+0x400!=0), uptime ctx+0x3b0 >= 0x61a9 (25001) and the
 * active flag (g_shifter_ctx+4). Pumps the transaction SM and reacts:
 *   3 = rejected -> " ERR Modbus Shift reject"
 *   2 = error    -> "  ERR Modbus Shift", bump comm-fail counter (+0x39); on the
 *                   6th+ error flush the bus (" ERR MBS flush" if that fails),
 *                   raise state flag 0x1000 and log " ERR MBS reset"
 *   1 = success  -> clear the comm-fail counter and state flag 0x1000
 * It always runs the control + update SMs (forwarding the txn result to the
 * latter). ctx == the session context (G_APP_CTX).
 */
void modbus_shifter_link_monitor(uint8_t *ctx)
{
    uint8_t prev_fail;
    int     result;

    if ((*(int16_t *)(ctx + 0x402) == 1) || (*(int16_t *)(ctx + 0x400) != 0)) {
        if (*(uint16_t *)(ctx + 0x3b0) < 0x61a9) {
            result = 0;
        } else if (g_shifter_ctx[4] == 0) {
            result = 0;
        } else {
            result = (int)shifter_modbus_pump();
            if (result == 3) {
                g_log_func(" ERR Modbus Shift reject\r\n");
            }
            if (result == 2) {
                log_print_timestamp_prefix();
                g_log_func("  ERR Modbus Shift\r\n");
                prev_fail = g_shifter_ctx[0x39];
                g_shifter_ctx[0x39] = (uint8_t)(prev_fail + 1);
                if (prev_fail > 4) {
                    if (shifter_bus_reset() == 0) {
                        g_log_func(" ERR MBS flush\r\n");
                    }
                    state_flags_set(0, 0x1000);
                    log_print_timestamp_prefix();
                    g_log_func(" ERR MBS reset\r\n");
                }
            } else if (result == 1) {
                g_shifter_ctx[0x39] = 0;
                state_flags_clear(0, 0x1000);
            }
        }
    } else {
        result = 0;
    }

    shifter_control_step(ctx);
    shifter_update_sm_step(ctx, result);
}

/*
 * shifter_control_step @ 0x08028870
 * The drivetrain control state machine (g_shifter_sm+1). Drives shifter
 * power-up, the ID/HW-version handshake (read into ctx+0x520: 0x200 = std
 * eShifter, 0x201 = MT), the auto-shift logic (compares bike speed ctx+0x3c2
 * against the per-region gear UP/DOWN speed thresholds at ctx + region*6 +
 * {0x10e..0x112}/{0x126..0x12a}, region = ctx+0x109), and the MT zeropos
 * calibration sequence (cases 0xD-0x10). Gear writes go out via func-6 reg 2
 * (shifter_send_gear), func-6 reg 0x50/0x30/0x14 and func-0x10 reg 0x40.
 * ctx == the session context (G_APP_CTX).
 */
void shifter_control_step(uint8_t *ctx)
{
    uint8_t  frame[0x110];
    uint8_t *state_rec;
    uint8_t  slot;
    uint8_t  gear_arg[5];

    state_table_ptr_get(&state_rec);
    if (state_rec[1] == 0x01) {
        announce_records_reset(3);
    }

    switch (g_shifter_sm[1]) {

    case 0:
        scheduler_start(g_shifter_sm[5], 1000, (sched_cb_t)0);
        log_print_timestamp_prefix();
        g_log_func("Shift ask ID\r\n");
        frame[0x00] = 0x20; frame[0x01] = 3; *(uint16_t *)(frame + 0x02) = 1; frame[0x84] = 1;
        if (modbus_shift_submit(frame) != 0) {
            log_print_timestamp_prefix();
            g_log_func("  ERR MS overflow\r\n");
        }
        g_shifter_sm[1] = 4;
        break;

    case 1:
        if (scheduler_slot_is_idle(g_shifter_sm[5]) != 0) {
            HAL_GPIO_WritePin((void *)GPIOD_BASE, 0x80, 1);
            scheduler_start(g_shifter_sm[5], 2000, (sched_cb_t)0);
            g_shifter_sm[1] = 2;
        }
        break;

    case 2:
        if (scheduler_slot_is_idle(g_shifter_sm[5]) != 0) {
            g_shifter_ctx[4] = 1;
            g_shifter_sm[1] = 0;
        }
        break;

    case 3:
        g_shifter_sm[6] = 5;
        if (g_shifter_sm[5] == SCHED_SLOT_NONE) {
            slot = scheduler_alloc();
            g_shifter_sm[5] = slot;
            scheduler_set_timer_name(slot, 2000, "pwron_tmr");
            scheduler_start(g_shifter_sm[5], 2000, (sched_cb_t)0);
            HAL_GPIO_WritePin((void *)GPIOD_BASE, 0x80, 1);
            HAL_GPIO_WritePin((void *)GPIOE_BASE, 0x4000, 1);
            shifter_usart3_reinit();
            log_print_timestamp_prefix();
            g_log_func("Shift pwr on\r\n");
        }
        if (scheduler_slot_is_idle(g_shifter_sm[5]) != 0) {
            usart3_init();
            g_shifter_ctx[4] = 1;
            g_shifter_sm[1] = 0;
        }
        break;

    case 4:
        if (*(int16_t *)(ctx + 0x520) == 0x200) {
            log_print_timestamp_prefix();
            g_log_func("Shifter Charge-on\r\n");
            state_flags_clear(0, 0x1000);
            scheduler_start(g_shifter_sm[5], 500, (sched_cb_t)0);
            g_shifter_sm[1] = 7;
        }
        if (*(int16_t *)(ctx + 0x520) == 0x201) {
            log_print_timestamp_prefix();
            g_log_func("Shifter MT\r\n");
            state_flags_clear(0, 0x1000);
            scheduler_start(g_shifter_sm[5], 500, (sched_cb_t)0);
            g_shifter_sm[1] = 8;
        }
        if (scheduler_slot_is_idle(g_shifter_sm[5]) != 0) {
            uint8_t prev = g_shifter_ctx[0x38];
            g_shifter_ctx[0x38] = (uint8_t)(prev + 1);
            if (prev < 6) {
                g_log_func("Ask again %02X\r\n", (int)*(int16_t *)(ctx + 0x520));
                g_shifter_sm[1] = 0;
            } else {
                state_flags_set(0, 0x1000);
                g_shifter_sm[1] = 0x11;
                g_log_func("Shifter Stop\r\n");
            }
        }
        break;

    case 6:
        if (scheduler_slot_is_idle(g_shifter_sm[5]) != 0) {
            scheduler_release(&g_shifter_sm[5]);
            if (*(int16_t *)(ctx + 0x520) == 0x201) {
                log_print_timestamp_prefix();
                g_log_func("MT shifter %d,%d,%d,%d\r\n",
                    (uint32_t)(((uint32_t)*(uint16_t *)(ctx + 0x59e) << 16) |
                               (uint32_t)*(uint16_t *)(ctx + 0x5a0)),
                    (uint32_t)(((uint32_t)*(uint16_t *)(ctx + 0x5a2) << 16) |
                               (uint32_t)*(uint16_t *)(ctx + 0x5a4)),
                    (uint32_t)(((uint32_t)*(uint16_t *)(ctx + 0x5a6) << 16) |
                               (uint32_t)*(uint16_t *)(ctx + 0x5a8)),
                    (uint32_t)(((uint32_t)*(uint16_t *)(ctx + 0x5aa) << 16) |
                               (uint32_t)*(uint16_t *)(ctx + 0x5ac)));
            }
            if (*(int8_t *)(ctx + 0x108) == 0) {
                g_shifter_sm[1] = 9;    /* auto-shift */
            } else {
                g_shifter_sm[1] = 0xb;  /* manual    */
            }
        }
        break;

    case 7:
        if (shifter_seq_status_poll_step() != 0) {
            g_shifter_sm[1] = 6;
            scheduler_start(g_shifter_sm[5], 500, (sched_cb_t)0);
        }
        break;

    case 8:
        frame[0x00] = 0x20; frame[0x01] = 3; *(uint16_t *)(frame + 0x02) = 1; frame[0x84] = 0x16;
        if (modbus_shift_submit(frame) != 0) g_log_func("  ERR MS overflow\r\n");
        frame[0x00] = 0x20; frame[0x01] = 3; *(uint16_t *)(frame + 0x02) = 0x30; frame[0x84] = 0x20;
        if (modbus_shift_submit(frame) != 0) g_log_func("  ERR MS overflow\r\n");
        scheduler_start(g_shifter_sm[5], 500, (sched_cb_t)0);
        g_shifter_sm[1] = 6;
        break;

    case 9: {
        uint16_t reported;
        if (*(int8_t *)(ctx + 0x108) == 0x01) {
            g_shifter_sm[1] = 0xb;
        }
        if (g_shifter_sm[7] == SCHED_SLOT_NONE) {
            slot = scheduler_alloc();
            g_shifter_sm[7] = slot;
            scheduler_start(slot, 3000, (sched_cb_t)0);
            *(uint16_t *)(ctx + 0x522) = 0;
        }
        if (scheduler_slot_is_idle(g_shifter_sm[7]) != 0) {
            scheduler_start(g_shifter_sm[7], 60000, (sched_cb_t)0);
            frame[0x00] = 0x20; frame[0x01] = 3; *(uint16_t *)(frame + 0x02) = 2; frame[0x84] = 1;
            if (modbus_shift_submit(frame) != 0) {
                log_print_timestamp_prefix();
                g_log_func("  ERR MS overflow\r\n");
            }
            *(uint16_t *)(ctx + 0x522) = 0;
            if (*(int16_t *)(ctx + 0x520) == 0x201) {
                *(uint16_t *)(frame + 0x02) = 0x30;
                if (modbus_shift_submit(frame) != 0) {
                    log_print_timestamp_prefix();
                    g_log_func("  ERR MS overflow\r\n");
                }
            }
        }
        reported = *(uint16_t *)(ctx + 0x522);
        if ((reported != 0) && (reported != 0xff) && (reported != g_shifter_sm[8])) {
            log_print_timestamp_prefix();
            g_log_func("Error gear %d = %d, retry\r\n", g_shifter_sm[8],
                       (int)*(int16_t *)(ctx + 0x522));
            g_shifter_sm[8] = (uint8_t)*(uint16_t *)(ctx + 0x522);
            *(uint16_t *)(ctx + 0x522) = 0;
            if (*(int16_t *)(ctx + 0x520) == 0x201) {
                log_print_timestamp_prefix();
                g_log_func("ERR Position = %u\r\n", *(uint16_t *)(ctx + 0x57e));
            }
        }
        switch (g_shifter_sm[8]) {
        case 1:
            if (*(uint16_t *)(ctx + (uint32_t)*(uint8_t *)(ctx + 0x109) * 6 + 0x10e) <
                *(uint16_t *)(ctx + 0x3c2)) {
                g_shifter_sm[8] = 2;
                g_shifter_sm[1] = 10;
            }
            break;
        case 2: {
            uint16_t spd = *(uint16_t *)(ctx + 0x3c2);
            uint8_t  region = *(uint8_t *)(ctx + 0x109);
            if (spd < *(uint16_t *)(ctx + (uint32_t)region * 6 + 0x126)) {
                g_shifter_sm[8] = 1;
                g_shifter_sm[1] = 10;
            }
            if (*(uint16_t *)(ctx + (uint32_t)region * 6 + 0x110) < spd) {
                g_shifter_sm[8] = 3;
                g_shifter_sm[1] = 10;
            }
            break;
        }
        case 3: {
            uint16_t spd = *(uint16_t *)(ctx + 0x3c2);
            uint32_t region = (uint32_t)*(uint8_t *)(ctx + 0x109);
            if (spd < *(uint16_t *)(ctx + region * 6 + 0x126)) {
                g_shifter_sm[8] = 1;
                g_shifter_sm[1] = 10;
            } else if (spd < *(uint16_t *)(ctx + region * 6 + 0x128)) {
                g_shifter_sm[8] = 2;
                g_shifter_sm[1] = 10;
            }
            if (*(uint16_t *)(ctx + region * 6 + 0x112) < spd) {
                g_shifter_sm[8] = 4;
                g_shifter_sm[1] = 10;
            }
            break;
        }
        case 4: {
            uint16_t spd = *(uint16_t *)(ctx + 0x3c2);
            uint32_t region = (uint32_t)*(uint8_t *)(ctx + 0x109);
            if (spd < *(uint16_t *)(ctx + region * 6 + 0x126)) {
                g_shifter_sm[8] = 1;
                g_shifter_sm[1] = 10;
            } else if (spd < *(uint16_t *)(ctx + region * 6 + 0x128)) {
                g_shifter_sm[8] = 2;
                g_shifter_sm[1] = 10;
            } else if (spd < *(uint16_t *)(ctx + region * 6 + 0x12a)) {
                g_shifter_sm[8] = 3;
                g_shifter_sm[1] = 10;
            }
            break;
        }
        case 5:
            g_shifter_sm[8] = 1;
            g_shifter_sm[1] = 10;
            break;
        }
        break;
    }

    case 10: {
        uint32_t pstate;
        scheduler_release(&g_shifter_sm[7]);
        pstate = (uint32_t)power_state_get_clamped();
        if (pstate < 2) {
            g_shifter_sm[6] = 2;
            g_shifter_sm[8] = 2;
        } else {
            if ((modem_info_ready() != 0) || (*(int8_t *)(ctx + 0x310) != '\v')) {
                g_shifter_sm[6] = 1;
                g_shifter_sm[8] = 1;
            }
        }
        *(int *)(ctx + 0x338) += 1;
        shifter_send_gear(g_shifter_sm[8]);
        g_shifter_sm[1] = 0xc;
        break;
    }

    case 0x0b: {
        uint16_t reported;
        if (*(int8_t *)(ctx + 0x108) == 0) {
            g_shifter_sm[1] = 9;
        }
        if (g_shifter_sm[7] == SCHED_SLOT_NONE) {
            slot = scheduler_alloc();
            g_shifter_sm[7] = slot;
            scheduler_start(slot, 3000, (sched_cb_t)0);
            *(uint16_t *)(ctx + 0x522) = 0;
        }
        if (scheduler_slot_is_idle(g_shifter_sm[7]) != 0) {
            scheduler_start(g_shifter_sm[7], 60000, (sched_cb_t)0);
            frame[0x00] = 0x20; frame[0x01] = 3; *(uint16_t *)(frame + 0x02) = 2; frame[0x84] = 1;
            if (modbus_shift_submit(frame) != 0) {
                log_print_timestamp_prefix();
                g_log_func("  ERR MS overflow\r\n");
            }
            *(uint16_t *)(ctx + 0x522) = 0;
            if (*(int16_t *)(ctx + 0x520) == 0x201) {
                *(uint16_t *)(frame + 0x02) = 0x30;
                if (modbus_shift_submit(frame) != 0) {
                    log_print_timestamp_prefix();
                    g_log_func("  ERR MS overflow\r\n");
                }
            }
        }
        reported = *(uint16_t *)(ctx + 0x522);
        if ((reported != 0) && (reported != 0xff) && (reported != g_shifter_sm[8])) {
            log_print_timestamp_prefix();
            g_log_func("Error gear %d = %d, retry\r\n", g_shifter_sm[8],
                       (int)*(int16_t *)(ctx + 0x522));
            shifter_send_gear(g_shifter_sm[8]);
            *(uint16_t *)(ctx + 0x522) = 0;
            if (*(int16_t *)(ctx + 0x520) == 0x201) {
                log_print_timestamp_prefix();
                g_log_func("ERR Position = %u\r\n", *(uint16_t *)(ctx + 0x57e));
            }
        }
        state_table_ptr_get(&state_rec);
        if (state_rec[1] == 0x05) {
            announce_records_reset(3);
            scheduler_release(&g_shifter_sm[7]);
            if (gpio_pc0_is_low() == 0) {     /* manual-shift button state A */
                if ((g_shifter_sm[8] < 4) &&
                    (maybe_get_bike_state() == '\f') &&
                    (*(int16_t *)(ctx + 0x3c2) != 0)) {
                    gear_arg[0] = ' ';
                    amp_volume_brownout_apply(gear_arg);
                    {
                        uint8_t pkt[2];
                        pkt[1] = 1;
                        pkt[0] = 1;
                        if (ssp_ble_enqueue_tx_packet(200, 2, pkt, '\0') > 0x10) {
                            g_log_func("  ERROR SSP place\r\n");
                        }
                    }
                    g_shifter_sm[8] = (uint8_t)(g_shifter_sm[8] + 1);
                    *(uint8_t *)(ctx + 0x3cc) = g_shifter_sm[8];
                    *(int *)(ctx + 0x338) += 1;
                    shifter_send_gear(g_shifter_sm[8]);
                    log_print_timestamp_prefix();
                    g_log_func("Shift up %d\r\n", g_shifter_sm[8]);
                }
            } else if (1 < g_shifter_sm[8]) {
                gear_arg[0] = ' ';
                amp_volume_brownout_apply(gear_arg);
                {
                    uint8_t pkt[2];
                    pkt[1] = 1;
                    pkt[0] = 1;
                    if (ssp_ble_enqueue_tx_packet(200, 2, pkt, '\0') > 0x10) {
                        g_log_func("  ERROR SSP place\r\n");
                    }
                }
                g_shifter_sm[8] = (uint8_t)(g_shifter_sm[8] - 1);
                *(uint8_t *)(ctx + 0x3cc) = g_shifter_sm[8];
                *(int *)(ctx + 0x338) += 1;
                shifter_send_gear(g_shifter_sm[8]);
                log_print_timestamp_prefix();
                g_log_func("Shift down %d\r\n", g_shifter_sm[8]);
            }
        }
        if ((*(int16_t *)(ctx + 0x3c2) == 0) && (g_shifter_sm[8] != 1)) {
            scheduler_release(&g_shifter_sm[7]);
            g_shifter_sm[8] = 1;
            *(uint8_t *)(ctx + 0x3cc) = 1;
            *(int *)(ctx + 0x338) += 1;
            shifter_send_gear(g_shifter_sm[8]);
            log_print_timestamp_prefix();
            g_log_func("Shift back to 1\r\n");
        }
        break;
    }

    case 0x0c:
        if (g_shifter_sm[9] == SCHED_SLOT_NONE) {
            slot = scheduler_alloc();
            g_shifter_sm[9] = slot;
            scheduler_set_timer_name(slot, 600, "poll_change_gear_tmr");
            scheduler_start(g_shifter_sm[9], 600, (sched_cb_t)0);
        }
        if (scheduler_slot_is_idle(g_shifter_sm[9]) != 0) {
            scheduler_release(&g_shifter_sm[9]);
            *(uint8_t *)(ctx + 0x3cc) = g_shifter_sm[8];
            if (g_shifter_sm[6] == 0x05) {
                g_shifter_sm[1] = 9;
            } else {
                log_print_timestamp_prefix();
                g_log_func("Shifter OFF %d\r\n", g_shifter_sm[6]);
                matrix_set_corner_led(*(uint8_t *)(ctx + 0x3cc));
                g_shifter_sm[1] = 0x11;
            }
        }
        break;

    case 0x0d:
        *(int *)(ctx + 0x338) += 1;
        shifter_send_gear(1);
        if (g_shifter_sm[10] == SCHED_SLOT_NONE) {
            slot = scheduler_alloc();
            g_shifter_sm[10] = slot;
        }
        scheduler_start(g_shifter_sm[10], 1000, (sched_cb_t)0);
        g_shifter_sm[1] = 0xe;
        break;

    case 0x0e:
        if (scheduler_slot_is_idle(g_shifter_sm[10]) != 0) {
            frame[0x00] = 0x20; frame[0x01] = 6; *(uint16_t *)(frame + 0x02) = 0x50;
            frame[0x04] = 0x28; frame[0x05] = 0; frame[0x84] = 1;
            if (modbus_shift_submit(frame) != 0) g_log_func("  ERR MS overflow\r\n");
            *(uint16_t *)(frame + 0x02) = 0x30;
            frame[0x04] = 0xe8; frame[0x05] = 0xfd; frame[0x84] = 1;
            if (modbus_shift_submit(frame) != 0) g_log_func("  ERR MS overflow\r\n");
            if (g_shifter_sm[10] == SCHED_SLOT_NONE) {
                slot = scheduler_alloc();
                g_shifter_sm[10] = slot;
            }
            scheduler_start(g_shifter_sm[10], 0x898, (sched_cb_t)0);
            g_shifter_sm[1] = 0xf;
        }
        break;

    case 0x0f:
        if (scheduler_slot_is_idle(g_shifter_sm[10]) != 0) {
            frame[0x00] = 0x20; frame[0x01] = 3; *(uint16_t *)(frame + 0x02) = 0x30; frame[0x84] = 1;
            if (modbus_shift_submit(frame) != 0) g_log_func("  ERR MS overflow\r\n");
            scheduler_start(g_shifter_sm[10], 200, (sched_cb_t)0);
            g_shifter_sm[1] = 0x10;
        }
        break;

    case 0x10:
        if (scheduler_slot_is_idle(g_shifter_sm[10]) != 0) {
            log_print_timestamp_prefix();
            if ((uint16_t)(*(int16_t *)(ctx + 0x57e) + 0x234bU) < 0x224b) {
                g_log_func("MT zeropos %d\r\n");
                if (*(int16_t *)(ctx + 0x5a8) == 0) {
                    g_log_func("MT no actual value\r\n");
                } else {
                    uint32_t zp = (uint32_t)*(uint16_t *)(ctx + 0x57e);
                    int      d0 = (int)(zp - 2000);
                    int      d1 = (int)(zp - 22000);
                    uint32_t d2 = zp - 0x9a4c;
                    int      d3 = (int)(zp - 54000);
                    g_log_func("calc %d %d %d %d\r\n", d0, d1, d2, d3);
                    if (((int)*(int16_t *)(ctx + 0x5a8) + 0x110U < d2) ||
                        (d2 < (int)*(int16_t *)(ctx + 0x5a8) - 0x110U)) {
                        g_log_func("Saving new zeropos\r\n");
                        frame[0x00] = 0x20; frame[0x01] = 0x10;
                        *(uint16_t *)(frame + 0x02) = 0x40;
                        frame[0x84] = 0x10;
                        frame[0x04] = (uint8_t)((uint32_t)d0 >> 0x18);
                        frame[0x05] = (uint8_t)((uint32_t)d0 >> 0x10);
                        frame[0x06] = (uint8_t)((uint32_t)d0 >> 8);
                        frame[0x07] = (uint8_t)d0;
                        frame[0x08] = (uint8_t)((uint32_t)d1 >> 0x18);
                        frame[0x09] = (uint8_t)((uint32_t)d1 >> 0x10);
                        frame[0x0a] = (uint8_t)((uint32_t)d1 >> 8);
                        frame[0x0b] = (uint8_t)d1;
                        frame[0x0c] = (uint8_t)(d2 >> 0x18);
                        frame[0x0d] = (uint8_t)(d2 >> 0x10);
                        frame[0x0e] = (uint8_t)(d2 >> 8);
                        frame[0x0f] = (uint8_t)d2;
                        frame[0x10] = (uint8_t)((uint32_t)d3 >> 0x18);
                        frame[0x11] = (uint8_t)((uint32_t)d3 >> 0x10);
                        frame[0x12] = (uint8_t)((uint32_t)d3 >> 8);
                        frame[0x13] = (uint8_t)d3;
                        if (modbus_shift_submit(frame) != 0) g_log_func("  Error\r\n");
                    } else {
                        g_log_func("keep zeropos\r\n");
                    }
                }
            } else {
                g_log_func("MT zeropos %d out of range\r\n");
            }
            frame[0x00] = 0x20; frame[0x01] = 6; *(uint16_t *)(frame + 0x02) = 0x50;
            frame[0x84] = 1; frame[0x04] = 100; frame[0x05] = 0;
            if (modbus_shift_submit(frame) != 0) g_log_func("  ERR MS overflow\r\n");
            *(uint16_t *)(frame + 0x02) = 0x14;
            frame[0x84] = 1; frame[0x04] = 1; frame[0x05] = 0;
            if (modbus_shift_submit(frame) != 0) {
                log_print_timestamp_prefix();
                g_log_func("  ERR MS overflow\r\n");
            }
            scheduler_release(&g_shifter_sm[10]);
            g_shifter_sm[1] = 0x11;
        }
        break;

    case 0x11:
        if (g_shifter_sm[4] == SCHED_SLOT_NONE) {
            slot = scheduler_alloc();
            g_shifter_sm[4] = slot;
            scheduler_set_timer_name(slot, 2000, "pwroff_tmr");
            scheduler_start(g_shifter_sm[4], 2000, (sched_cb_t)0);
        }
        if (scheduler_slot_is_idle(g_shifter_sm[4]) != 0) {
            scheduler_release(&g_shifter_sm[4]);
            scheduler_release(&g_shifter_sm[5]);
            g_shifter_ctx[4] = 0;
            HAL_GPIO_WritePin((void *)GPIOE_BASE, 0x4000, 0);
            g_shifter_sm[1] = 5;
        }
        break;
    }
}

/*
 * shifter_update_sm_step @ 0x08027c78
 * eShifter OTA firmware-update state machine (update-FSM step @ g_shifter_ctx+0).
 * Called once per super-loop with the in-flight Modbus transaction result in
 * txn_result (1 == reply received). Probes the shifter, validates the staged
 * PACK at flash 0x08010000, streams it in 0x20-byte blocks via func-0x10
 * write-multiple to reg 0x82, polls the CRC/erase answer (ctx_app+0x620),
 * resets the shifter, reads back the version, then persists the state record to
 * EEPROM. The update SM's scheduler slot id is g_shifter_sm[0]. ctx == the
 * session context (G_APP_CTX).
 */
void shifter_update_sm_step(uint8_t *ctx_app, int txn_result)
{
    uint8_t  frame[0x110];
    uint8_t *ctx = g_shifter_ctx;
    uint8_t  slot;
    int      offset;
    uint32_t hdr_ver;
    int16_t  ans;

    switch (ctx[0]) {
    case 1:
        ctx[1] = 1;
        ctx[0] = 2;
        break;

    case 2:
        ctx[2] = 0;
        if (g_shifter_sm[0] == SCHED_SLOT_NONE) {
            if (HAL_GPIO_ReadPin((void *)GPIOE_BASE, 0x4000) == 0) {
                slot = scheduler_alloc();
                g_shifter_sm[0] = slot;
                scheduler_start(slot, 3000, (sched_cb_t)0);
                scheduler_set_timer_name(g_shifter_sm[0], 3000, "update_tmr");
                log_print_timestamp_prefix();
                g_log_func("Poweron shifter\r\n");
                HAL_GPIO_WritePin((void *)GPIOE_BASE, 0x4000, 1);
            } else {
                log_print_timestamp_prefix();
                g_log_func("Shifter is on\r\n");
                ctx[0] = 3;
            }
        }
        if (scheduler_slot_is_idle(g_shifter_sm[0]) != 0) {
            scheduler_release(&g_shifter_sm[0]);
            ctx[0] = 3;
            ctx[3] = 0;
        }
        break;

    case 3:
        if (g_shifter_sm[0] == SCHED_SLOT_NONE) {
            ctx[4] = 1;
            slot = scheduler_alloc();
            g_shifter_sm[0] = slot;
            scheduler_start(slot, 5000, (sched_cb_t)0);
            scheduler_set_timer_name(g_shifter_sm[0], 5000, "update_tmr");
            log_print_timestamp_prefix();
            g_log_func("Shift get manufacture\r\n");
            frame[0x00] = 0x20; frame[0x01] = 3; *(uint16_t *)(frame + 2) = 1; frame[0x84] = 1;
            if (modbus_shift_submit(frame) != 0) {
                g_log_func("  ERR MS overflow\r\n");
            }
        }
        if (scheduler_slot_is_idle(g_shifter_sm[0]) != 0) {
            ctx[3] = ctx[3] + 1;
            scheduler_release(&g_shifter_sm[0]);
        }
        if ((*(int16_t *)(ctx_app + 0x520) != 0) || (ctx[3] == 3)) {
            ctx[0] = 4;
        }
        break;

    case 4: {
        int16_t hw;
        ctx[2] = 0;
        if (pack_validate((uint32_t *)(ctx + 8), (uint32_t *)SHIFTER_IMAGE_BASE) == 0) {
            hw = *(int16_t *)(ctx_app + 0x520);
            if (hw == 0) {
                g_log_func("Unknown shifter ID\r\n");
                ctx[0] = 0x10;
            } else {
                hdr_ver = *(uint32_t *)(ctx + 0xc);
                if ((((hdr_ver & 0xff) == 0xc1) && (hw == 0x200)) ||
                    (((hdr_ver & 0xff) == 0xc9) && (hw == 0x201))) {
                    g_log_func("Shifterware: v%x.%02x.%02X (%s %s)",
                               hdr_ver >> 0x18,
                               (hdr_ver & 0xffffff) >> 0x10,
                               (hdr_ver & 0xffff) >> 8,
                               ctx + 0x18, ctx + 0x24);
                    g_log_func(" size %d bytes\r\n", *(uint32_t *)(ctx + 0x14));
                    ctx[0] = 5;
                } else {
                    log_print_timestamp_prefix();
                    g_log_func("Not a Shifter file\r\n");
                    ctx[0] = 6;
                }
            }
        } else {
            g_log_func("Shifter CRC error\r\n");
            ctx[0] = 0x10;
        }
        break;
    }

    case 5:
        log_print_timestamp_prefix();
        g_log_func("Send Shifter erase\r\n");
        frame[0x00] = 0x20; frame[0x01] = 6; *(uint16_t *)(frame + 2) = 0x95;
        frame[0x84] = 1; frame[0x04] = 1; frame[0x05] = 0;
        if (modbus_shift_submit(frame) != 0) g_log_func("  MS Error\r\n");
        scheduler_start(g_shifter_sm[0], 2000, (sched_cb_t)0);
        *(uint32_t *)(ctx + 0x30) = 0;
        ctx[0] = 7;
        break;

    case 6:
        scheduler_release(&g_shifter_sm[0]);
        ctx[2] = 3;
        ctx[0] = 0;
        break;

    case 7:
        if (txn_result == 1) {
            ctx[0] = 8;
        }
        if (scheduler_slot_is_idle(g_shifter_sm[0]) != 0) {
            uint8_t prev = ctx[0x34];
            ctx[0x34] = prev + 1;
            if (prev < 0xf) {
                g_log_func("MBS retry %d\r\n");
                scheduler_start(g_shifter_sm[0], 4000, (sched_cb_t)0);
                shifter_bus_reset();
                ctx[0] = 0x13;
            } else {
                g_log_func("MBS Erase timeout\r\n");
                ctx[0] = 0x10;
            }
        }
        break;

    case 8:
        offset = *(int *)(ctx + 0x30);
        frame[0x00] = 0x20; frame[0x01] = 0x10; *(uint16_t *)(frame + 2) = 0x82; frame[0x84] = 0x24;
        frame[0x04] = (uint8_t)((uint32_t)offset >> 0x18);
        frame[0x05] = (uint8_t)((uint32_t)offset >> 0x10);
        frame[0x06] = (uint8_t)((uint32_t)offset >> 8);
        frame[0x07] = (uint8_t)offset;
        *(uint32_t *)(frame + 0x08) = *(uint32_t *)(SHIFTER_IMAGE_BASE + offset + 0x00);
        *(uint32_t *)(frame + 0x0c) = *(uint32_t *)(SHIFTER_IMAGE_BASE + offset + 0x04);
        *(uint32_t *)(frame + 0x10) = *(uint32_t *)(SHIFTER_IMAGE_BASE + offset + 0x08);
        *(uint32_t *)(frame + 0x14) = *(uint32_t *)(SHIFTER_IMAGE_BASE + offset + 0x0c);
        *(uint32_t *)(frame + 0x18) = *(uint32_t *)(SHIFTER_IMAGE_BASE + offset + 0x10);
        *(uint32_t *)(frame + 0x1c) = *(uint32_t *)(SHIFTER_IMAGE_BASE + offset + 0x14);
        *(uint32_t *)(frame + 0x20) = *(uint32_t *)(SHIFTER_IMAGE_BASE + offset + 0x18);
        *(uint32_t *)(frame + 0x24) = *(uint32_t *)(SHIFTER_IMAGE_BASE + offset + 0x1c);
        if (modbus_shift_submit(frame) != 0) {
            g_log_func("  MS Error\r\n");
            ctx[0] = 0x10;
        }
        *(uint32_t *)(ctx + 0x30) = *(uint32_t *)(ctx + 0x30) + 0x20;
        scheduler_start(g_shifter_sm[0], 2000, (sched_cb_t)0);
        ctx[0] = 9;
        break;

    case 9:
        if ((txn_result == 1) && (shift_bus_peek_response() == 0)) {
            g_log_func("%d/%d\r",
                       *(uint32_t *)(ctx + 0x30), *(uint32_t *)(ctx + 0x14));
            if (*(uint32_t *)(ctx + 0x30) < *(uint32_t *)(ctx + 0x14)) {
                ctx[0] = 8;
            } else {
                ctx[0] = 10;
            }
        }
        if (scheduler_slot_is_idle(g_shifter_sm[0]) != 0) {
            uint8_t prev = ctx[0x34];
            ctx[0x34] = prev + 1;
            if (prev < 0xf) {
                g_log_func("MBS retry %d\r\n");
                scheduler_start(g_shifter_sm[0], 2000, (sched_cb_t)0);
                shifter_bus_reset();
                HAL_GPIO_WritePin((void *)GPIOD_BASE, 0x80, 0);
                HAL_GPIO_WritePin((void *)GPIOE_BASE, 0x4000, 0);
                systick_delay(0x32);
                HAL_GPIO_WritePin((void *)GPIOE_BASE, 0x4000, 1);
                HAL_GPIO_WritePin((void *)GPIOD_BASE, 0x80, 1);
                ctx[0] = 0x13;
            } else {
                g_log_func("MBS Write timeout\r\n");
                ctx[0] = 0x10;
            }
        }
        break;

    case 10:
        log_print_timestamp_prefix();
        g_log_func("Send shifter crc check\r\n");
        frame[0x00] = 0x20; frame[0x01] = 3; *(uint16_t *)(frame + 2) = 0x81; frame[0x84] = 1;
        if (modbus_shift_submit(frame) != 0) g_log_func("  MS Error\r\n");
        *(uint16_t *)(ctx_app + 0x620) = 0x2a6;
        scheduler_start(g_shifter_sm[0], 2000, (sched_cb_t)0);
        ctx[0] = 0xc;
        break;

    case 0xb:
        if ((uint16_t)(*(int16_t *)(ctx_app + 0x520) - 0x200) < 2) {
            ctx[0] = 5;
        }
        if (scheduler_slot_is_idle(g_shifter_sm[0]) != 0) {
            scheduler_start(g_shifter_sm[0], 2000, (sched_cb_t)0);
            ctx[0] = 0x13;
        }
        if (0xf < ctx[0x34]) {
            ctx[0] = 0x10;
        }
        break;

    case 0xc:
        ans = *(int16_t *)(ctx_app + 0x620);
        if (ans != 0x2a6) {
            if (ans == 1) {
                log_print_timestamp_prefix();
                g_log_func("checksum error\r\n");
                ctx[0] = 0x10;
            } else if (ans == 2) {
                log_print_timestamp_prefix();
                g_log_func("No app header\r\n");
                ctx[0] = 0x10;
            } else if (ans == 0) {
                log_print_timestamp_prefix();
                g_log_func("crc check ok\r\n");
                scheduler_start(g_shifter_sm[0], 2000, (sched_cb_t)0);
                ctx[0x35] = 0;
                ctx[0] = 0xd;
            } else {
                log_print_timestamp_prefix();
                g_log_func("Unknown answer (%d)\r\n", (int)*(int16_t *)(ctx_app + 0x620));
                ctx[0] = 0x10;
            }
        }
        if (scheduler_slot_is_idle(g_shifter_sm[0]) != 0) {
            g_log_func("  Ask crc timeout\r\n");
            ctx[0] = 0x10;
        }
        break;

    case 0xd:
        if (scheduler_slot_is_idle(g_shifter_sm[0]) != 0) {
            g_log_func("Send reset\r\n");
            frame[0x00] = 0x20; frame[0x01] = 6; *(uint16_t *)(frame + 2) = 0x80; frame[0x84] = 1;
            if (modbus_shift_submit(frame) != 0) g_log_func("  MS Error\r\n");
            if (*(int16_t *)(ctx_app + 0x520) == 0x200) {
                frame[0x01] = 3; *(uint16_t *)(frame + 2) = 6;
                if (modbus_shift_submit(frame) != 0) g_log_func("  MS Error\r\n");
            }
            scheduler_start(g_shifter_sm[0], 8000, (sched_cb_t)0);
            ctx[0] = 0xe;
        }
        break;

    case 0xe:
        if (scheduler_slot_is_idle(g_shifter_sm[0]) != 0) {
            *(uint16_t *)(ctx_app + 0x52a) = 0;
            g_log_func("Get version\r\n");
            ctx[0x35] = ctx[0x35] + 1;
            scheduler_release(&g_shifter_sm[0]);
            frame[0x00] = 0x20; frame[0x01] = 3; *(uint16_t *)(frame + 2) = 6; frame[0x84] = 1;
            if (modbus_shift_submit(frame) != 0) g_log_func("  MS Error\r\n");
            scheduler_start(g_shifter_sm[0], 2000, (sched_cb_t)0);
            ctx[0] = 0xf;
        }
        break;

    case 0xf: {
        int16_t ver;
        if (scheduler_slot_is_idle(g_shifter_sm[0]) != 0) {
            ctx[0] = 0xe;
        }
        ver = *(int16_t *)(ctx_app + 0x52a);
        if (ver != 0) {
            g_log_func("Shifter V%d.%d\r\n", (int)ver >> 8, (int8_t)(char)ver);
            ctx[0] = 0x12;
        }
        if (5 < ctx[0x35]) {
            g_log_func("No version received\r\n");
            ctx[0] = 0x10;
        }
        break;
    }

    case 0x10:
        scheduler_release(&g_shifter_sm[0]);
        ctx[2] = 2;
        if (ctx[1] != 0) {
            int rc = save_state_record_to_eeprom(
                *(uint32_t *)(ctx_app + 0x310), *(uint32_t *)(ctx_app + 0x314),
                *(uint32_t *)(ctx_app + 0x318), *(uint32_t *)(ctx_app + 0x31c),
                *(uint32_t *)(ctx_app + 0x320), *(uint32_t *)(ctx_app + 0x324),
                *(uint32_t *)(ctx_app + 0x328), *(uint32_t *)(ctx_app + 0x32c),
                *(uint32_t *)(ctx_app + 0x330), *(uint32_t *)(ctx_app + 0x334),
                *(uint32_t *)(ctx_app + 0x338), *(uint32_t *)(ctx_app + 0x33c),
                *(uint32_t *)(ctx_app + 0x340), *(uint32_t *)(ctx_app + 0x344),
                *(uint32_t *)(ctx_app + 0x348));
            if (rc != 0) {
                g_log_func(" ERROR Save values\r\n");
            }
            testmode_command_dispatch(10);
        }
        ctx[0] = 0;
        break;

    case 0x12:
        scheduler_release(&g_shifter_sm[0]);
        ctx[2] = 1;
        ctx[0] = 0;
        if (ctx[1] != 0) {
            maybe_set_state_if_unlocked(0x1d);
        }
        break;

    case 0x13:
        if (scheduler_slot_is_idle(g_shifter_sm[0]) != 0) {
            ctx[0x34] = ctx[0x34] + 1;
            log_print_timestamp_prefix();
            g_log_func("SHift ask ID\r\n");
            frame[0x00] = 0x20; frame[0x01] = 3; *(uint16_t *)(frame + 2) = 1; frame[0x84] = 1;
            modbus_shift_submit(frame);
            *(uint16_t *)(ctx_app + 0x520) = 0;
            scheduler_start(g_shifter_sm[0], 2000, (sched_cb_t)0);
            ctx[0] = 0xb;
        }
        break;

    default:
        break;
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Section I — console `shifterstatus` dump callbacks
 * ════════════════════════════════════════════════════════════════════════════
 * Armed by console_cmd_shifterstatus. Both release the scheduler slot parked at
 * 0x2000010F (SCHED_SLOT_REC+1) and stream the cached shifter status registers
 * out through g_log_func, read via the session context g_app_state.ctx_sub.
 */

/* shifterstatus_dump_v200 @ 0x080410C4 — standard eShifter (HW 0x200). */
void shifterstatus_dump_v200(void)
{
    uint8_t *ctx;

    scheduler_release(SHIFTERSTATUS_TASK_SLOT);
    ctx = (uint8_t *)g_app_state.ctx_sub;

    g_log_func("DEVICE_ID 0x%X\r\n",   (int)*(int16_t *)(ctx + 0x520));
    g_log_func("GEAR      %d\r\n",      (int)*(int16_t *)(ctx + 0x522));
    g_log_func("ERROR     %d\r\n",      (int)*(int16_t *)(ctx + 0x524));
    g_log_func("HWVERSION V%d.%d\r\n",  (int)*(int8_t *)(ctx + 0x529),
                                        (char)*(uint16_t *)(ctx + 0x528));
    g_log_func("FWVERSION V%d.%d\r\n",  (int)*(int8_t *)(ctx + 0x52B),
                                        (char)*(uint16_t *)(ctx + 0x52A));
    g_log_func("DATA_CODE %d\r\n",      (int)*(int16_t *)(ctx + 0x53A));
    g_log_func("SHIFTER_NBRSHIFTS %d\r\n",
               ((int)*(int16_t *)(ctx + 0x53C) << 16) | (int)*(int16_t *)(ctx + 0x53E));
    g_log_func("MAIN_NR_OF_SHIFTS %d\r\n", *(uint32_t *)(ctx + 0x338));
}

/* shifterstatus_dump_v201 @ 0x08040EEC — MT shifter (HW 0x201); superset of v200. */
void shifterstatus_dump_v201(void)
{
    uint8_t *ctx;
    int g;

    scheduler_release(SHIFTERSTATUS_TASK_SLOT);
    ctx = (uint8_t *)g_app_state.ctx_sub;

    g_log_func("DEVICE_ID    0x%X\r\n",  (int)*(int16_t *)(ctx + 0x520));
    g_log_func("GEAR         %d\r\n",    (int)*(int16_t *)(ctx + 0x522));
    g_log_func("ERROR        %d\r\n",    (int)*(int16_t *)(ctx + 0x524));
    g_log_func("HWVERSION    V%d.%d\r\n",(int)*(int8_t *)(ctx + 0x529),
                                         (char)*(uint16_t *)(ctx + 0x528));
    g_log_func("FWVERSION    V%d.%d\r\n",(int)*(int8_t *)(ctx + 0x52B),
                                         (char)*(uint16_t *)(ctx + 0x52A));
    g_log_func("DATA_CODE    %d\r\n",    (int)*(int16_t *)(ctx + 0x53A));
    g_log_func("OLD_NR_OF_SHIFTS %d\r\n",
               ((int)*(int16_t *)(ctx + 0x53C) << 16) | (int)*(int16_t *)(ctx + 0x53E));
    g_log_func("NR_OF_SHIFTS %d\r\n",   *(uint32_t *)(ctx + 0x338));
    g_log_func("ERROR_CNTR   %d\r\n",   (int)*(int16_t *)(ctx + 0x54A));
    g_log_func("\r\nMMOTOR_STATUS    %04X\r\n", (int)*(int16_t *)(ctx + 0x580));
    g_log_func("TIME_LAST_SHIFT %d\r\n",(int)*(int16_t *)(ctx + 0x582));
    g_log_func("Angle        %d sector %d\r\n",
               *(uint16_t *)(ctx + 0x57E),
               (char)*(uint16_t *)(ctx + 0x586));
    g_log_func("PID_KP       %d\r\n",   (int)*(int16_t *)(ctx + 0x588));
    g_log_func("PID_KI       %d\r\n",   (int)*(int16_t *)(ctx + 0x58A));
    g_log_func("PID_KD       %d\r\n",   (int)*(int16_t *)(ctx + 0x58C));
    g_log_func("HYSTERESE    %d\r\n",   (int)*(int16_t *)(ctx + 0x58E));
    g_log_func("STEP_TIME    %d\r\n",   (int)*(int16_t *)(ctx + 0x590));
    g_log_func("MAX_MOV_TIME %d\r\n",   (int)*(int16_t *)(ctx + 0x592));
    g_log_func("NUMBER_OF_GEARS %d\r\n",(int)*(int16_t *)(ctx + 0x59C));

    for (g = 0; ; g++) {
        ctx = (uint8_t *)g_app_state.ctx_sub;   /* OEM re-derefs ctx_sub each iteration */
        if (g >= (int)*(int16_t *)(ctx + 0x59C)) {
            break;
        }
        g_log_func("POSITION_GEAR_%d %u\r\n", g + 1,
                   ((uint32_t)*(uint16_t *)(ctx + 0x59E + 4 * g) << 16) |
                    (uint32_t)*(uint16_t *)(ctx + 0x5A0 + 4 * g));
    }
}
