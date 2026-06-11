#include <stdint.h>

#include "app.h"
#include "log.h"
#include "motor.h"
#include "panic.h"
#include "scheduler.h"
#include "ssp.h"
#include "systick.h"

/* Helpers sourced in their home modules (the FSM below drives them). */
extern void HAL_GPIO_WritePin(void *GPIOx, uint16_t pin_mask, int state);
extern int  state_flags_test(uint32_t set_mask, uint32_t clr_mask);  /* OEM 0x0802A28C */
extern unsigned int maybe_enqueue_tx_message(uint16_t id, uint32_t len,
                                             const void *payload, uint8_t type);  /* ssp.c */

/* One-shot scheduler callback armed 200 ticks after the motor reset completes,
 * for both the success (state 0xC) and failure (state 0xD) exits. Defined below,
 * after the FSM control-block macros it touches (OEM 0x08030994). */
static void motor_post_update_cb(void);

/* OEM assert filename for this translation unit (rodata 0x08050D0C). */
static const char F2806X_C[] = "src/F2806/f2806x.c";

/* The download transfer context (OEM SRAM 0x20000654). All four pumps share this
 * one block; each owns a disjoint slice, so only one transfer phase is live at a
 * time. A single scheduler-slot byte (0x20000075) paces them with a per-call
 * timeout — it is allocated by motor_fw_update_fsm_step before any pump runs, so
 * the pumps assert (rather than allocate) if it is still SCHED_SLOT_NONE.
 *
 *   recv-buffer pump : +0x00 state, +0x04 cursor, +0x08 remaining
 *   recv-byte pump   : +0x0C state, +0x0D (cleared scratch)
 *   recv-u16 pump    : +0x0D byte index, +0x0E state, +0x10 u16 staging
 *   send-buffer pump : +0x14 state, +0x18 cursor, +0x1C remaining
 */
#define F2806_CTX      ((volatile uint8_t *)0x20000654u)
#define F2806_DL_SLOT  (*(volatile uint8_t *)0x20000075u)

/* Send `count` bytes from `src` over the SSPM bus (OEM 0x08030B70). State at
 * ctx+0x14, cursor ctx+0x18, remaining ctx+0x1C. */
int motor_dl_send_buf_step(const uint8_t *src, int count, uint32_t timeout_ms)
{
    volatile uint8_t *state = F2806_CTX + 0x14;

    if (*state == 0) {
        *(const uint8_t **)(F2806_CTX + 0x18) = src;
        *(int *)(F2806_CTX + 0x1C) = count;
        if (F2806_DL_SLOT == SCHED_SLOT_NONE) {
            muco_assert_fail(F2806X_C, 0x161);
        }
        scheduler_start(F2806_DL_SLOT, timeout_ms, (sched_cb_t)0);
        *state = (uint8_t)(*state + 1);
        return 1;
    }

    if (*state == 1) {
        if (scheduler_slot_is_idle(F2806_DL_SLOT) == 0) {
            const uint8_t *p = *(const uint8_t **)(F2806_CTX + 0x18);
            if (sspm_bus_send_byte(*p) != 0) {
                *(const uint8_t **)(F2806_CTX + 0x18) = p + 1;
                int rem = *(int *)(F2806_CTX + 0x1C) - 1;
                *(int *)(F2806_CTX + 0x1C) = rem;
                if (rem == 0) {
                    *state = 0;
                    return 0;
                }
            }
            return 1;
        }
        *state = 0;          /* timer expired -> timeout */
        return 2;
    }

    return 1;
}

/* Receive `count` bytes into `dst` (OEM 0x080309C4). State at ctx+0x00, cursor
 * ctx+0x04, remaining ctx+0x08. */
int motor_dl_recv_buf_step(uint8_t *dst, int count, uint32_t timeout_ms)
{
    volatile uint8_t *state = F2806_CTX + 0x00;

    if (*state == 0) {
        *(uint8_t **)(F2806_CTX + 0x04) = dst;
        *(int *)(F2806_CTX + 0x08) = count;
        if (F2806_DL_SLOT == SCHED_SLOT_NONE) {
            muco_assert_fail(F2806X_C, 0x197);
        }
        scheduler_start(F2806_DL_SLOT, timeout_ms, (sched_cb_t)0);
        *state = (uint8_t)(*state + 1);
        return 1;
    }

    if (*state == 1) {
        if (scheduler_slot_is_idle(F2806_DL_SLOT) == 0) {
            uint8_t *p = *(uint8_t **)(F2806_CTX + 0x04);
            if (sspm_bus_get_byte(p) != 0) {
                *(uint8_t **)(F2806_CTX + 0x04) = p + 1;
                int rem = *(int *)(F2806_CTX + 0x08);
                *(int *)(F2806_CTX + 0x08) = rem - 1;
                if (rem - 1 == 0) {
                    *state = 0;
                    return 0;
                }
            }
            return 1;
        }
        *state = 0;          /* timer expired -> timeout */
        return 2;
    }

    return 1;
}

/* Receive one byte into `*out` (OEM 0x08030A50). State at ctx+0x0C. */
int motor_dl_recv_byte_step(uint8_t *out, uint32_t timeout_ms)
{
    volatile uint8_t *state = F2806_CTX + 0x0C;
    uint8_t b = 0;

    if (*state == 0) {
        F2806_CTX[0x0D] = 0;
        if (F2806_DL_SLOT == SCHED_SLOT_NONE) {
            muco_assert_fail(F2806X_C, 0x1C9);
        }
        scheduler_start(F2806_DL_SLOT, timeout_ms, (sched_cb_t)0);
        *state = (uint8_t)(*state + 1);
        return 1;
    }

    if (*state == 1) {
        if (scheduler_slot_is_idle(F2806_DL_SLOT) == 0) {
            if (sspm_bus_get_byte(&b) != 0) {
                *out = b;
                *state = 0;
                return 0;
            }
            return 1;
        }
        *state = 0;          /* timer expired -> timeout */
        return 2;
    }

    return 1;
}

/* Receive a little-endian u16 into `*out` (OEM 0x08030ADC). State at ctx+0x0E,
 * byte index ctx+0x0D, 2-byte staging ctx+0x10. */
int motor_dl_recv_u16_step(uint16_t *out, uint32_t timeout_ms)
{
    volatile uint8_t *state = F2806_CTX + 0x0E;

    if (*state == 0) {
        F2806_CTX[0x0D] = 0;
        if (F2806_DL_SLOT == SCHED_SLOT_NONE) {
            muco_assert_fail(F2806X_C, 0x1F5);
        }
        scheduler_start(F2806_DL_SLOT, timeout_ms, (sched_cb_t)0);
        *state = 1;
        return 1;
    }

    if (*state == 1) {
        if (scheduler_slot_is_idle(F2806_DL_SLOT) == 0) {
            uint8_t idx = F2806_CTX[0x0D];
            if (sspm_bus_get_byte((uint8_t *)(F2806_CTX + 0x10 + idx)) != 0) {
                uint8_t n = (uint8_t)(F2806_CTX[0x0D] + 1);
                F2806_CTX[0x0D] = n;
                if (n == 2) {
                    *out = *(volatile uint16_t *)(F2806_CTX + 0x10);
                    *state = 0;
                    return 0;
                }
            }
            return 1;
        }
        *state = 0;          /* timer expired -> timeout */
        return 2;
    }

    return 1;
}

/* ===========================================================================
 * Transaction layer — handshake / block-transfer state machines on top of the
 * pumps above. They occupy higher slices of the same download context:
 *   send-verify  : +0x20 state, +0x24 cursor, +0x28 remaining
 *   autobaud     : +0x2C state
 *   stream-block : +0x2D state, +0x30 cursor, +0x34 checksum(u16),
 *                  +0x38 counter, +0x3C block length(u16)
 * =========================================================================== */

/* Send `count` bytes from `buf`, verifying each is echoed back (OEM 0x08030BFC).
 * Two interleaved sub-phases per byte: send one (state 1) then receive+compare
 * one (state 2). On an echo mismatch that is not the final byte it logs and
 * aborts; on the final-byte mismatch it silently completes (OEM quirk). */
int motor_dl_send_verify_step(const uint8_t *buf, int count)
{
    uint8_t state = F2806_CTX[0x20];

    if (state == 1) {
        uint32_t rc = motor_dl_send_buf_step(*(const uint8_t **)(F2806_CTX + 0x24), 1, 100);
        if (rc == 0) {
            F2806_CTX[0x20] = (uint8_t)(F2806_CTX[0x20] + 1);
            return state;                  /* == 1, keep pumping */
        }
        if (rc == 2) {
            F2806_CTX[0x20] = 0;
            g_log_func("Fail 1\r\n");
        }
        return (int)rc;
    }

    if (state == 2) {
        uint8_t rx = 0;
        uint32_t rc = motor_dl_recv_buf_step(&rx, 1, 100);
        if (rc == 0) {
            const uint8_t *cur = *(const uint8_t **)(F2806_CTX + 0x24);
            if (*cur == rx) {
                *(const uint8_t **)(F2806_CTX + 0x24) = cur + 1;
                int rem = *(int *)(F2806_CTX + 0x28) - 1;
                *(int *)(F2806_CTX + 0x28) = rem;
                F2806_CTX[0x20] = (uint8_t)(rem != 0);   /* more -> resend, else done */
                return rem != 0;
            }
            int rem = *(int *)(F2806_CTX + 0x28);
            if (rem == 1) {
                F2806_CTX[0x20] = 0;
                return (int)rc;            /* rc == 0 -> complete */
            }
            g_log_func("Fail 2 %d\r\n", rem);
            F2806_CTX[0x20] = 0;
            return state;                  /* == 2 */
        }
        if (rc == 2) {
            F2806_CTX[0x20] = 0;
            g_log_func("Fail 3 %d\r\n", *(int *)(F2806_CTX + 0x28));
        }
        return (int)rc;
    }

    if (state == 0) {
        *(const uint8_t **)(F2806_CTX + 0x24) = buf;
        *(int *)(F2806_CTX + 0x28) = count;
        if (count != 0) {
            F2806_CTX[0x20] = (uint8_t)(state + 1);
        }
        return 1;
    }

    return 1;
}

/* Autobaud lock against the C2000 SCI bootloader (OEM 0x08030CEC): send 'A'
 * (0x41), then expect 'A' echoed. The OEM sends from a 1-byte flash literal. */
static const uint8_t k_motor_autobaud_byte = 'A';   /* OEM flash literal 0x08050D44 */

int motor_dl_autobaud_step(void)
{
    uint8_t state = F2806_CTX[0x2C];

    if (state == 0) {
        int rc = motor_dl_send_buf_step(&k_motor_autobaud_byte, 1, 100);
        if (rc == 0) {
            F2806_CTX[0x2C] = 1;
            return 1;
        }
        return rc;
    }

    if (state == 1) {
        uint8_t b = 0;
        int rc = motor_dl_recv_byte_step(&b, 100);
        if (rc == 0) {
            if (b == 'A') {
                F2806_CTX[0x2C] = 0;
                g_log_func("Autobaud ok\r\n");
                return rc;                 /* 0 */
            }
            g_log_func("Err Autobaud [%d]\r\n", b);
            F2806_CTX[0x2C] = 0;
            return 2;
        }
        if (rc == 2) {
            g_log_func("Autobaud no answer\r\n");
            F2806_CTX[0x2C] = 0;
        }
        return rc;
    }

    return 1;
}

/* Stream a length-prefixed firmware block with a running 16-bit additive
 * checksum, verified against the DSP's echoed checksum (OEM 0x08030D88).
 *   0: latch the source buffer.
 *   1: send a 22-byte header, then re-sum it into the checksum.
 *   2: receive + verify the header checksum (15 s window).
 *   3: stream the block one byte at a time; the first two bytes are the block
 *      length, every byte feeds the checksum; end of block = counter reaches
 *      (length+3)*2, with a periodic (every 2048 bytes) checkpoint to state 5.
 *   4: per-block checksum verify -> loop back to state 3.
 *   5: periodic checksum checkpoint -> back to state 3. */
int motor_dl_stream_block_step(const uint8_t *buf, int start)
{
    uint16_t rx = 0xFFFF;
    uint8_t state = F2806_CTX[0x2D];

    switch (state) {
    case 0:
        *(const uint8_t **)(F2806_CTX + 0x30) = buf;
        if (start != 0) {
            F2806_CTX[0x2D] = (uint8_t)(state + 1);
        }
        return 1;

    case 1: {
        uint32_t rc = motor_dl_send_buf_step(*(const uint8_t **)(F2806_CTX + 0x30), 0x16, 100);
        if (rc == 0) {
            *(uint16_t *)(F2806_CTX + 0x34) = 0;
            for (uint8_t i = 0; i < 0x16; i++) {
                const uint8_t *p = *(const uint8_t **)(F2806_CTX + 0x30);
                *(const uint8_t **)(F2806_CTX + 0x30) = p + 1;
                *(uint16_t *)(F2806_CTX + 0x34) =
                    (uint16_t)(*p + *(uint16_t *)(F2806_CTX + 0x34));
            }
            F2806_CTX[0x2D] = (uint8_t)(F2806_CTX[0x2D] + 1);
            return state;                  /* == 1 */
        }
        if (rc == 2) {
            g_log_func("Fail 4\r\n");
            F2806_CTX[0x2D] = 0;
        }
        return (int)rc;
    }

    case 2: {
        uint32_t rc = motor_dl_recv_u16_step(&rx, 15000);
        if (rc == 0) {
            if (rx == *(uint16_t *)(F2806_CTX + 0x34)) {
                *(uint16_t *)(F2806_CTX + 0x34) = 0;
                *(int *)(F2806_CTX + 0x38) = 0;
                F2806_CTX[0x2D] = (uint8_t)(F2806_CTX[0x2D] + 1);
                return 1;
            }
            g_log_func("Fail 0x%04X 0x%04X\r\n", rx, *(uint16_t *)(F2806_CTX + 0x34));
            F2806_CTX[0x2D] = 0;
            return state;                  /* == 2 */
        }
        if (rc == 2) {
            g_log_func("Fail 5\r\n");
            F2806_CTX[0x2D] = 0;
        }
        return (int)rc;
    }

    case 3: {
        uint32_t rc = motor_dl_send_buf_step(*(const uint8_t **)(F2806_CTX + 0x30), 1, 100);
        if (rc == 0) {
            int idx = *(int *)(F2806_CTX + 0x38);
            if (idx == 0) {
                *(uint16_t *)(F2806_CTX + 0x3C) =
                    (uint16_t)**(const uint8_t **)(F2806_CTX + 0x30);
            } else if (idx == 1) {
                *(uint16_t *)(F2806_CTX + 0x3C) =
                    (uint16_t)(*(uint16_t *)(F2806_CTX + 0x3C) |
                               (**(const uint8_t **)(F2806_CTX + 0x30) << 8));
            }

            const uint8_t *p = *(const uint8_t **)(F2806_CTX + 0x30);
            *(const uint8_t **)(F2806_CTX + 0x30) = p + 1;
            *(uint16_t *)(F2806_CTX + 0x34) =
                (uint16_t)(*p + *(uint16_t *)(F2806_CTX + 0x34));

            uint32_t cnt = (uint32_t)(idx + 1);
            *(int *)(F2806_CTX + 0x38) = (int)cnt;
            uint16_t len = *(uint16_t *)(F2806_CTX + 0x3C);

            if (len == 0 && cnt > 1) {
                F2806_CTX[0x2D] = (uint8_t)len;          /* state 0; rc == 0 -> done */
                return (int)rc;
            }
            if (cnt == (uint32_t)((len + 3) * 2)) {
                F2806_CTX[0x2D] = 4;
                return 1;
            }
            if (((cnt - 6u) & 0x7FFu) == 0u) {           /* == ((idx-5) & 0x7FF) */
                if (cnt < 7) {
                    return 1;
                }
                F2806_CTX[0x2D] = 5;
            }
            return 1;
        }
        if (rc == 2) {
            g_log_func("Fail 6\r\n");
            F2806_CTX[0x2D] = 0;
        }
        return (int)rc;
    }

    case 4: {
        uint32_t rc = motor_dl_recv_u16_step(&rx, 1000);
        if (rc == 0) {
            if (rx == *(uint16_t *)(F2806_CTX + 0x34)) {
                *(uint16_t *)(F2806_CTX + 0x34) = 0;
                *(int *)(F2806_CTX + 0x38) = 0;
                F2806_CTX[0x2D] = 3;
                return 1;
            }
            F2806_CTX[0x2D] = 0;
            return 2;
        }
        if (rc == 2) {
            F2806_CTX[0x2D] = 0;
            g_log_func("Fail 7\r\n");
        }
        return (int)rc;
    }

    case 5: {
        uint32_t rc = motor_dl_recv_u16_step(&rx, 1000);
        if (rc == 0) {
            if (rx == *(uint16_t *)(F2806_CTX + 0x34)) {
                *(uint16_t *)(F2806_CTX + 0x34) = 0;
                F2806_CTX[0x2D] = 3;
                return 1;
            }
            g_log_func("Fail_2 0x%04X 0x%04X\r\n", rx, *(uint16_t *)(F2806_CTX + 0x34));
            F2806_CTX[0x2D] = 0;
            return 2;
        }
        if (rc == 2) {
            g_log_func("Fail 8\r\n");
            F2806_CTX[0x2D] = 0;
        }
        return (int)rc;
    }

    default:
        return 1;
    }
}

/* ===========================================================================
 * Top-level motor-controller (F2806x) firmware-update FSM (OEM 0x08030FF4),
 * ticked once per super-loop iteration. Sequence: reset-pulse the DSP into its
 * SCI ROM bootloader (GPIOB PB9 = reset, PB10 = boot-mode select), autobaud-lock
 * ('A'/'A'), upload a fixed handshake payload, validate the staged motor pack
 * header (magic 0xAA55AA55, type 0xA1, "Motorpcb Application" version), stream
 * the C28x boot-stream payload, then release the DSP. Progress is logged as
 * F_init/F_reset/F_autobaud/F_upload/F_ready.
 *
 * Its own little control block sits at 0x20000075: [0] = the download timer slot
 * the transfer pumps share (F2806_DL_SLOT), [1] = FSM state, [2] = an auxiliary
 * timer slot, [3] = a one-shot flag. The +0x3E byte of the download context
 * (0x20000654) records whether the bike was put into the "updating" state, which
 * gates the exit transition (0x1D ok / 0x1B fail).
 * =========================================================================== */
#define MOTOR_FSM   ((volatile uint8_t *)0x20000075u)   /* [0] dl slot, [1] state, [2] aux slot, [3] flag */
#define MOTOR_GPIO  ((void *)0x40020400u)               /* GPIOB: PB9 (0x200) reset, PB10 (0x400) boot-mode */
#define MOTOR_PACK  ((volatile uint8_t *)0x080A0000u)   /* staged motor pack: magic +0, ver +4, size +0xC, data +0x28 */
#define MOTOR_UPLOAD_BUF ((const uint8_t *)0x0804463Cu) /* 0x95E-byte handshake payload sent before the pack stream */

/* Armed (200 ticks) by both FSM exits (states 0xC/0xD) once the DSP is released:
 * log the reset, free the shared download timer slot, and drop PB9 (reset) low. */
static void motor_post_update_cb(void)
{
    g_log_func("Reset F2806\r\n");
    scheduler_release((uint8_t *)&MOTOR_FSM[0]);
    HAL_GPIO_WritePin(MOTOR_GPIO, 0x200, 0);
}

unsigned int motor_fw_update_fsm_step(void)
{
    switch (MOTOR_FSM[1]) {
    case 0:                                   /* wait for the timer slot, then announce the update */
        if (scheduler_slot_is_idle(MOTOR_FSM[2]) == 0) {
            return 3;
        } else {
            unsigned int slot = maybe_enqueue_tx_message(10, 0, 0, 1);
            if (slot > 0x10) {
                g_log_func("  ERROR SSP place\r\n");
            }
            scheduler_start(MOTOR_FSM[2], 1000, (sched_cb_t)0);
            MOTOR_FSM[1] = 1;
            return 3;
        }

    case 1:                                   /* wait for the module ack (flag 0x400000), then begin */
        if (state_flags_test(0x400000, 0) == 0) {
            log_print_timestamp_prefix();
            g_log_func("Recover Motor ok\r\n");
            scheduler_release((uint8_t *)&MOTOR_FSM[2]);
            F2806_CTX[0x3E] = 0;
            MOTOR_FSM[1] = 2;
        }
        if (scheduler_slot_is_idle(MOTOR_FSM[2]) == 0) {
            return 3;
        }
        scheduler_release((uint8_t *)&MOTOR_FSM[2]);
        maybe_set_state_if_unlocked(0x19);
        F2806_CTX[0x3E] = 1;
        MOTOR_FSM[1] = 4;
        return 3;

    case 2:                                   /* idle / settled */
        return 3;

    case 3:                                   /* reset pulse on PB9, then loop back to state 0 */
        log_print_timestamp_prefix();
        g_log_func("Resetting motor\r\n");
        HAL_GPIO_WritePin(MOTOR_GPIO, 0x200, 1);
        systick_delay(10);
        HAL_GPIO_WritePin(MOTOR_GPIO, 0x200, 0);
        if (MOTOR_FSM[2] == SCHED_SLOT_NONE) {
            MOTOR_FSM[2] = scheduler_alloc();
        }
        scheduler_start(MOTOR_FSM[2], 1000, (sched_cb_t)0);
        MOTOR_FSM[1] = 0;
        return 1;

    case 4: {                                 /* drive boot pins, drain RX, enter bootloader */
        int drained;
        HAL_GPIO_WritePin(MOTOR_GPIO, 0x400, 0);   /* PB10 low: select bootloader */
        HAL_GPIO_WritePin(MOTOR_GPIO, 0x200, 1);   /* PB9 high: release reset */
        do {
            drained = sspm_bus_get_byte((uint8_t *)0);   /* flush stale RX (OEM sink is NULL) */
        } while (drained != 0);
        MOTOR_FSM[0] = scheduler_alloc();
        scheduler_start(MOTOR_FSM[0], 100, (sched_cb_t)0);
        MOTOR_FSM[1] = 5;
        MOTOR_FSM[3] = 1;
        g_log_func("F_init\r\n");
        return 1;
    }

    case 5: {                                 /* settle delay -> drop PB9, start autobaud */
        unsigned int r = (unsigned int)((scheduler_slot_is_idle(MOTOR_FSM[0]) ^ 1) & 0xFF);
        if (r == 0) {
            HAL_GPIO_WritePin(MOTOR_GPIO, 0x200, 0);
            systick_delay(100);
            MOTOR_FSM[1] = 6;
            g_log_func("F_reset\r\n");
            r = 1;
        }
        return r;
    }

    case 6: {                                 /* autobaud handshake */
        int rc = motor_dl_autobaud_step();
        if (rc == 1) {
            return 1;
        }
        systick_delay(100);
        MOTOR_FSM[1] = (uint8_t)((rc == 0) ? 7 : 0x0D);
        g_log_func("F_autobaud\r\n");
        return 1;
    }

    case 7: {                                 /* upload the fixed handshake payload (echo-verified) */
        int rc = motor_dl_send_verify_step(MOTOR_UPLOAD_BUF, 0x95E);
        if (rc == 1) {
            return 1;
        }
        scheduler_start(MOTOR_FSM[0], 5000, (sched_cb_t)0);
        MOTOR_FSM[1] = (uint8_t)((rc == 0) ? 8 : 0x0D);
        g_log_func("F_upload\r\n");
        return 1;
    }

    case 8: {                                 /* settle -> second autobaud */
        unsigned int r = (unsigned int)((scheduler_slot_is_idle(MOTOR_FSM[0]) ^ 1) & 0xFF);
        if (r == 0) {
            MOTOR_FSM[1] = 9;
            g_log_func("F_ready\r\n");
            r = 1;
        }
        return r;
    }

    case 9: {                                 /* second autobaud -> validate the pack */
        int rc = motor_dl_autobaud_step();
        if (rc == 1) {
            return 1;
        }
        MOTOR_FSM[1] = (uint8_t)((rc == 0) ? 0x0A : 0x0D);
        return 1;
    }

    case 0x0A:                                /* validate the staged motor pack header */
        if (*(volatile uint32_t *)MOTOR_PACK != 0xAA55AA55u) {
            MOTOR_FSM[1] = 0x0D;
            g_log_func("No Motorpcb Application\r\n");
            return 1;
        } else {
            uint32_t ver = *(volatile uint32_t *)(MOTOR_PACK + 4);
            if ((ver & 0xFF) != 0xA1) {       /* low byte = firmware type; 0xA1 = motor */
                g_log_func("Not a F2806 file\r\n");
                MOTOR_FSM[1] = 0x0D;
                return 1;
            }
            g_log_func("Motorpcb Application: v%x.%02x.%02X (%s %s)",
                       ver >> 24, (ver >> 16) & 0xFF, (ver >> 8) & 0xFF,
                       (const char *)(MOTOR_PACK + 0x10),
                       (const char *)(MOTOR_PACK + 0x1C));
            g_log_func(" size %d bytes\r\n", *(volatile uint32_t *)(MOTOR_PACK + 0x0C));
            MOTOR_FSM[1] = 0x0B;
            return 1;
        }

    case 0x0B: {                              /* stream the C28x boot-stream payload */
        unsigned int rc = (unsigned int)motor_dl_stream_block_step(
            (const uint8_t *)(MOTOR_PACK + 0x28),
            *(int *)(MOTOR_PACK + 0x0C) - 0x28);
        if (rc == 0) {
            MOTOR_FSM[1] = 0x0C;
            return 1;
        }
        if (rc == 2) {
            MOTOR_FSM[1] = 0x0D;
            g_log_func("F2806-err\r\n");
            return 1;
        }
        return rc;                            /* busy (1) */
    }

    case 0x0C:                                /* success: release the DSP, log, settle to state 2 */
        scheduler_start(MOTOR_FSM[0], 200, motor_post_update_cb);
        HAL_GPIO_WritePin(MOTOR_GPIO, 0x200, 1);
        HAL_GPIO_WritePin(MOTOR_GPIO, 0x400, 1);
        g_log_func("F2806-OK\r\n");
        MOTOR_FSM[3] = 0;
        if (F2806_CTX[0x3E] != 0) {
            maybe_set_state_if_unlocked(0x1D);
        }
        MOTOR_FSM[1] = 2;
        return 0;

    case 0x0D:                                /* failure: release the DSP, settle / signal */
        scheduler_start(MOTOR_FSM[0], 200, motor_post_update_cb);
        HAL_GPIO_WritePin(MOTOR_GPIO, 0x200, 1);
        HAL_GPIO_WritePin(MOTOR_GPIO, 0x400, 1);
        MOTOR_FSM[3] = 2;
        if (F2806_CTX[0x3E] == 0) {
            MOTOR_FSM[1] = 2;
            return 2;
        }
        maybe_set_state_if_unlocked(0x1B);
        return 2;

    default:
        return 1;
    }
}
