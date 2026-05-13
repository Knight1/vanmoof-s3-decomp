/* modbus_dispatch.c — Top-of-stack PDU dispatcher.
 *
 * Once `FUN_08003eda` (the RX state machine) has reassembled a full
 * Modbus frame in the inbound scratch at `0x200000C8`, it calls
 * `modbus_dispatch_pdu(cmd, len)` where:
 *
 *   - `cmd` is the function-code byte (`*(uint8_t*)0x200000C8 + ...`
 *     — exact offset is TBD until the RX FSM is decomp'd, but this
 *     dispatch sees the byte already extracted).
 *   - `len` is the inbound PDU length (matched against per-command
 *     expected lengths to gate side effects).
 *
 * Two post-dispatch hooks always run regardless of which case matched:
 *
 *   - If `len == 6` or `len == 0x0F`, transmit a passthrough reply
 *     (`modbus_reply_passthrough`).
 *   - Clear the pending-request latch at `0x200000D8`.
 *
 * Several case handlers below trampoline into helpers that are not
 * yet decomp'd. They're stubbed as traps so the link succeeds; when
 * each helper lands in its proper module the corresponding stub here
 * gets removed and replaced by an `extern` to the real symbol.
 */

#include "modbus.h"
#include "image.h"
#include "flash_store.h"
#include <stdint.h>

/* ---- raw RAM globals touched by the dispatcher and the RX FSM -----
 *
 * Identified by resolving the literal pool at `0x080041FC..0x08004260`
 * from the OEM binary. Will move into typed `extern` declarations
 * once their owning modules are decomp'd.
 */

#define G_REQ_PENDING   (*(volatile uint8_t  *)0x200000D8u) /* 1 = PDU ready, 0 = idle */
#define G_RX_FRAME_MODE (*(volatile uint8_t  *)0x200000D9u) /* 0 = short PDU expected, 1 = long (OTA) */
#define G_RX_WAIT_CTR   (*(volatile uint32_t *)0x200000DCu) /* end-of-frame timeout counter */
#define G_OTA_WRITE_PTR (*(volatile uint32_t *)0x200000E0u) /* OTA staging write pointer */
#define G_RX_HEAD       (*(volatile uint32_t *)0x200000E4u) /* bytes received this frame (0..0x2D) */
#define G_LOOP_IDX      (*(volatile uint32_t *)0x200000E6u) /* scratch idx used by the copy loop */
#define G_CRC_LO        (*(volatile uint8_t  *)0x200000E7u) /* output of modbus_crc16_compute (lo) */
#define G_CRC_HI        (*(volatile uint8_t  *)0x200000E8u) /* output of modbus_crc16_compute (hi) */
#define G_5A_TARGET     (*(volatile uint8_t  *)0x200000EAu)
#define G_COUNTER       (*(volatile uint32_t *)0x200000F8u) /* incremented in case 0x14 */
#define G_5C_REGS       ((volatile uint8_t   *)0x20000100u) /* 3-byte register block */
#define G_14_FLAG_A     (*(volatile uint8_t  *)0x20000117u)
#define G_MODE          (*(volatile uint8_t  *)0x20000139u) /* gates 0x14, 0x5A */
#define G_14_FLAG_B     (*(volatile uint8_t  *)0x2000013Du)
#define G_OTA_OFF_LO    (*(volatile uint8_t  *)0x2000013Fu) /* OTA staging offset (lo) */
#define G_OTA_OFF_HI    (*(volatile uint8_t  *)0x20000140u) /* OTA staging offset (hi) */

#define G_RX_BUF        ((volatile uint8_t   *)0x200000C8u) /* short-frame post-validate buffer (8 B) */
#define G_LONG_BUF      ((volatile uint8_t   *)0x2000015Cu) /* long-frame post-validate buffer (45 B) */
#define G_RX_SCRATCH    ((const volatile uint8_t *)0x200001B2u) /* IRQ-filled inbound scratch */

#define G_RX_WAIT_MAX   0x00249F00u   /* end-of-frame wait threshold (~2,400,000 ticks) */

/* OTA-erase target: same flash slot used by `image_apply`'s post-
 * failure cleanup. */
#define FLASH_IMAGE_SLOT 0x08001800u

/* ---- not-yet-decomp'd helpers --------------------------------------
 *
 * Trap stubs. Real implementations will land later; when they do,
 * delete the stub here and add an `extern` decl pointing at the
 * proper module.
 */

/* OEM @ 0x08003C68 (50 B). Case 0x0F — emit a uint32_t report. */
static void cmd_0f_report_u32(uint32_t value)
{
    (void)value;
    for (;;) { /* TODO: implement (decomp pending) */ }
}

/* OEM @ 0x08003B86 (24 B). Case 0x5C, len==3 — accept a 3-byte
 * register write and trigger a status report. */
static void cmd_5c_write3(uint8_t a, uint8_t b, uint8_t c)
{
    (void)a; (void)b; (void)c;
    for (;;) { /* TODO: implement (decomp pending) */ }
}

/* OEM @ 0x080031E6 (118 B). Case 0x5C, len==0x0F — after the
 * dispatcher has staged 3 bytes into G_5C_REGS, kick the consumer. */
static void cmd_5c_consume(void)
{
    for (;;) { /* TODO: implement (decomp pending) */ }
}

/* OEM @ 0x08003BC4 (88 B). Case 0x5B — runs a 3-level self-test
 * cascade and emits one of {0, 0x32, 0x64, 0x96} as a status code. */
static void cmd_5b_selftest(void)
{
    for (;;) { /* TODO: implement (decomp pending) */ }
}

/* OEM @ 0x080039E6 (160 B). Case 0x82, len==0x10 — accept a 16-byte
 * payload (probably an OTA flash-write page). */
static void cmd_82_fw_page(void)
{
    for (;;) { /* TODO: implement (decomp pending) */ }
}

/* ---- the dispatcher itself ----------------------------------------
 *
 * OEM @ 0x08003C9A (278 B). The OEM emits this as a GCC-style
 * jump-table switch (the BL to `__gnu_thumb1_case_uqi` at 0x08005DB4
 * followed by a packed branch table). The C below is the
 * behaviour-equivalent flat switch; the compiler's choice of code
 * shape may differ.
 */
void modbus_dispatch_pdu(uint8_t cmd, uint8_t len)
{
    if (G_REQ_PENDING != 1u) {
        G_REQ_PENDING = 0u;
        return;
    }

    switch (cmd) {
    case 0x0Fu:
        cmd_0f_report_u32(G_COUNTER);
        break;

    case 0x14u:
        if (G_MODE == 0u) {
            G_COUNTER  = G_COUNTER + 1u;
            G_14_FLAG_A = 1u;
            G_14_FLAG_B = 1u;
        } else {
            G_14_FLAG_B = 0u;
        }
        break;

    case 0x5Au:
        if (G_MODE == 0u) {
            G_5A_TARGET = G_RX_BUF[5];
        }
        break;

    case 0x5Bu:
        cmd_5b_selftest();
        break;

    case 0x5Cu:
        if (len == 3u) {
            cmd_5c_write3(G_5C_REGS[0], G_5C_REGS[1], G_5C_REGS[2]);
        } else if (len == 0x0Fu) {
            G_5C_REGS[0] = G_RX_BUF[2];
            G_5C_REGS[1] = G_RX_BUF[4];
            G_5C_REGS[2] = G_RX_BUF[5];
            cmd_5c_consume();
        }
        break;

    case 0x81u:
        image_apply();
        break;

    case 0x82u:
        if (len == 0x10u) {
            cmd_82_fw_page();
        }
        break;

    case 0x95u:
        /* Erase the OTA staging slot and switch the RX FSM into
         * long-frame mode so the next inbound PDU(s) are accepted
         * as 45-byte OTA payloads. */
        flash_erase_pages(FLASH_IMAGE_SLOT, 12);
        G_RX_FRAME_MODE = 1u;
        break;

    default:
        /* Unknown cmd: silent ignore (matches OEM — the GCC jump
         * table falls through to the common epilogue). */
        break;
    }

    if (len == 6u || len == 0x0Fu) {
        modbus_reply_passthrough();
    }

    G_REQ_PENDING = 0u;
}

/* OEM @ 0x08003EDA (366 B). RX state machine.
 *
 * Driven from the main poll loop (caller is at 0x08004366, inside a
 * not-yet-decomp'd function). On each call it inspects the IRQ-filled
 * scratch at `G_RX_SCRATCH` and either:
 *   - waits and ticks the end-of-frame timeout, or
 *   - accepts a complete frame (8 bytes short / 45 bytes long), CRCs
 *     it, copies into the per-mode validated buffer, latches
 *     G_REQ_PENDING and hands off to `modbus_dispatch_pdu`.
 *
 * Short-frame layout (8 bytes): [0]=0x20 slave, [1]=len, [3]=cmd,
 * [6..7]=CRC. Long-frame layout (45 bytes): same fields, CRC at
 * [0x2B..0x2C]; used for OTA payloads. The "first byte must be 0x20"
 * check filters frames addressed elsewhere on the shared bus.
 */
static void rx_reset_ota_state(void)
{
    G_RX_FRAME_MODE = 0u;
    G_OTA_OFF_LO    = 0u;
    G_OTA_OFF_HI    = 0u;
    G_OTA_WRITE_PTR = FLASH_IMAGE_SLOT;
}

void modbus_rx_poll(void)
{
    if (G_RX_HEAD == 0u) return;

    if (G_RX_SCRATCH[0] != 0x20u) {
        G_RX_HEAD = 0u;
        return;
    }

    if (G_RX_FRAME_MODE == 0u) {
        /* short PDU (8 bytes) */
        if (G_RX_HEAD < 8u) {
            if (G_RX_WAIT_CTR < G_RX_WAIT_MAX) {
                G_RX_WAIT_CTR = G_RX_WAIT_CTR + 1u;
            } else {
                G_RX_WAIT_CTR = 0u;
                G_RX_HEAD     = 0u;
            }
            return;
        }

        G_RX_HEAD  = 0u;
        G_LOOP_IDX = 0u;
        while (G_LOOP_IDX < 8u) {
            G_RX_BUF[G_LOOP_IDX] = G_RX_SCRATCH[G_LOOP_IDX];
            G_LOOP_IDX = G_LOOP_IDX + 1u;
        }
        modbus_crc16_compute((const uint8_t *)G_RX_BUF, 6);
        if (G_RX_BUF[6] == G_CRC_LO && G_RX_BUF[7] == G_CRC_HI) {
            G_REQ_PENDING = 1u;
            modbus_dispatch_pdu(G_RX_BUF[3], G_RX_BUF[1]);
        }
        return;
    }

    if (G_RX_FRAME_MODE == 1u) {
        /* long PDU (45 bytes — OTA payload) */
        if (G_RX_HEAD < 0x2Du) {
            if (G_RX_WAIT_CTR < G_RX_WAIT_MAX) {
                G_RX_WAIT_CTR = G_RX_WAIT_CTR + 1u;
            } else {
                G_RX_WAIT_CTR = 0u;
                G_RX_HEAD     = 0u;
                rx_reset_ota_state();
            }
            return;
        }

        G_RX_HEAD  = 0u;
        G_LOOP_IDX = 0u;
        while (G_LOOP_IDX < 0x2Du) {
            G_LONG_BUF[G_LOOP_IDX] = G_RX_SCRATCH[G_LOOP_IDX];
            G_LOOP_IDX = G_LOOP_IDX + 1u;
        }
        modbus_crc16_compute((const uint8_t *)G_LONG_BUF, 0x2B);
        if (G_LONG_BUF[0x2Bu] == G_CRC_LO && G_LONG_BUF[0x2Cu] == G_CRC_HI) {
            G_REQ_PENDING = 1u;
            modbus_dispatch_pdu(G_LONG_BUF[3], G_LONG_BUF[1]);
        } else {
            rx_reset_ota_state();
        }
    }
}
