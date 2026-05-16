/* TI driverlib `setup.c` mirror — cold-reset trim helpers that
 * `SetupTrimDevice` (vendor-stock) calls into directly out of the
 * BIM image rather than ROM-dispatching. Two functions:
 *
 *   - `bim_setup_after_cold_reset_cfg1` (`FUN_00056490`, 148 B +
 *     24 B literal pool): the OEM TI driverlib's
 *     `SetupAfterColdResetWakeupFromShutDownCfg1(fcfg1_rev)` — the
 *     first of three ROM-staged cold-reset sub-helpers, run only
 *     when `!SysCtrlResetSourceGet()` (true cold boot).
 *     Pokes FCFG2/FCFG1 readout into ADI3 + DDI0 trim registers and
 *     forwards the FCFG1 revision to a ROM-side three-step
 *     state machine at `ROM_API_TABLE[28]` (the HAPI / cold-reset
 *     sub-table that `SetupTrimDevice` shares with this caller).
 *
 *   - `bim_setup_adi_step` (`FUN_00056da4`, 44 B + 8 B literal
 *     pool): an ADI/DDI sequencer that drives the analog-config
 *     state machine through an 8-byte LUT in flash at `0x000571D8`
 *     until the live state-readback at MMIO `0x400C6000` settles on
 *     the requested target code. Called once from cfg1 with
 *     `target_code = 2`.
 *
 * Upstream: `source/ti/devices/cc13x2_cc26x2/driverlib/setup.c`
 * in TI SimpleLink CC13x2_CC26x2 SDK 3.40.x. The OEM driverlib
 * version baked into this BIM (Apr 2020) differs in detail from
 * present-day public source — exact magic constants below are
 * preserved verbatim against the OEM disassembly rather than
 * matched to the current SDK. */

#include <stdint.h>

#include "bim.h"

/* FCFG2 (0x50004000) base; cfg1 reads three consecutive 32-bit
 * words at offset 0xFAC..0xFB4 — FCFG2 isn't documented in the
 * public CC2642R1 TRM but the access shape (cold-reset trim
 * input) matches the role TI's chipinfo helpers play. */
#define FCFG2_TRIM_WORD0   (*(volatile uint32_t *)0x50004FACu)
#define FCFG2_TRIM_WORD1   (*(volatile uint32_t *)0x50004FB0u)
#define FCFG2_TRIM_WORD2   (*(volatile uint32_t *)0x50004FB4u)

/* ADI3 (Analog/Digital Interface 3, base 0x400CB000) holds the
 * recharge / DCDC / REFSYS trim registers; the cfg1 helper writes
 * one byte at offset 0x0E and one halfword at offset 0x6A (plus
 * three byte writes at the 0x4008_6209..0x4008_6256 region — the
 * other ADI access window). */
#define ADI3_TRIM_BYTE     (*(volatile uint8_t  *)0x400CB00Eu)
#define ADI3_TRIM_HALF     (*(volatile uint16_t *)0x400CB06Au)

/* DDI0 OSC (Direct Digital Interface 0, base 0x400CA000) holds the
 * RF/clock trim; cfg1 writes 0x01000100 to offset 0x404 to assert
 * an oscillator-mode bit pair, then does a 16-bit dummy readback
 * at offset 0 to push the write through the DDI write-buffer. */
#define DDI0_OSC_REG_0     (*(volatile uint16_t *)0x400CA000u)
#define DDI0_OSC_REG_404   (*(volatile uint32_t *)0x400CA404u)

/* FCFG1.MISC_TRIM_AON / OSC_CONF (offset 0x40C); read once during
 * cfg1, masked, and written back; bit fields drive ADI3.TRIM_BYTE,
 * the 0x40086209-based byte-array, and ADI3.TRIM_HALF. */
#define FCFG1_MISC_TRIM    (*(volatile uint32_t *)0x5000140Cu)

/* The 0x40086209..0x40086256 region is an MMIO byte array (likely
 * the AON_PMCTL / AON_BATMON peripheral DCDC trim shadow) — base
 * address is offset by 1 so the OEM can fold four byte writes
 * sharing the literal pool. Pinned offsets:
 *
 *   - +0x00 (= 0x40086209): byte = 2  if `r5 & (1<<25)` set
 *   - +0x13 (= 0x4008621C): byte = 0x40 if FCFG1.MISC_TRIM bit 9 set
 *   - +0x23 (= 0x4008622C): byte = 0x40 if FCFG1.MISC_TRIM bit 9 clear
 *   - +0x4D (= 0x40086256): byte = (FCFG2_TRIM_WORD0 >> 16) | 0xF0
 *
 * The bit-9 dispatch picks between two channels of the same field
 * — looks like a Vddr setpoint that lives in different shadow
 * registers depending on the chip's bootmode selector. */
#define AON_TRIM_SHADOW_BASE  ((volatile uint8_t *)0x40086209u)

/* `ROM_API_TABLE[28]` is the "cold-reset / HAPI" sub-table pointer
 * — three function slots cfg1 calls in order (with the FCFG2 user
 * ID `r5` as the common arg). Lives at ROM `0x100001F0`. */
#define BIM_HAPI_TABLE_PTR    ((const uintptr_t *const *)0x100001F0u)

/* `*0x42600494 = 1` is a bit-banded write of bit 5 of FLASH+0x24
 * (= 0x40030024). Bit 5 of FLASH+0x24 isn't the same latch as
 * `BIM_FLASH_ACK_BIT` (bit 1) — looks like a "trim-applied"
 * marker the BIM signals after every cold-reset cfg1 pass. */
#define BIM_FLASH_TRIM_DONE_BIT (*(volatile uint32_t *)0x42600494u)

void bim_setup_after_cold_reset_cfg1(uint32_t fcfg1_rev)
{
    uint32_t fcfg2_w1 = FCFG2_TRIM_WORD1;
    if ((fcfg2_w1 & 0x2u) == 0u) {
        uint32_t fcfg2_w0 = FCFG2_TRIM_WORD0;
        AON_TRIM_SHADOW_BASE[0x4Du] = (uint8_t)((fcfg2_w0 >> 16) | 0xF0u);
    }

    DDI0_OSC_REG_404 = 0x01000100u;
    (void)DDI0_OSC_REG_0;                              /* dummy readback */

    const uintptr_t *hapi = (const uintptr_t *)*BIM_HAPI_TABLE_PTR;
    uint32_t         r5   = FCFG2_TRIM_WORD2;

    ((void (*)(uint32_t))hapi[0])(r5);

    if ((r5 & (1u << 25)) == 0u) {
        if ((r5 & (1u << 24)) != 0u) {
            AON_TRIM_SHADOW_BASE[0x00u] = 2u;
        }
    }

    hapi = (const uintptr_t *)*BIM_HAPI_TABLE_PTR;
    ((void (*)(uint32_t, uint32_t))hapi[1])(fcfg1_rev, r5);

    uint32_t misc = FCFG1_MISC_TRIM;
    ADI3_TRIM_BYTE = (uint8_t)((misc >> 12) & 0x3Fu);

    if ((misc & (1u << 9)) == 0u) {
        AON_TRIM_SHADOW_BASE[0x23u] = 0x40u;
    } else {
        AON_TRIM_SHADOW_BASE[0x13u] = 0x40u;
    }

    ADI3_TRIM_HALF = (uint16_t)(((misc >> 6) & 0x38u) | 0x3800u);

    hapi = (const uintptr_t *)*BIM_HAPI_TABLE_PTR;
    ((void (*)(uint32_t))hapi[2])(r5);

    bim_setup_adi_step(2u);

    BIM_FLASH_TRIM_DONE_BIT = 1u;
}

/* ADI sequencer (`FUN_00056da4` in the OEM, 44 B + 8 B literal
 * pool + 8 B LUT at flash `0x000571D8`).
 *
 * Drives an analog-config state machine that lives at MMIO
 * `0x400C6000` (= status word) / `0x400C6004` (= ack word):
 *
 *   1. Wait for the ADI to acknowledge the previous write
 *      (`*0x400C6000 == *0x400C6004`).
 *   2. If the live state equals `target_code`, exit.
 *   3. Otherwise look up the next intermediate state via the LUT
 *      at `0x000571D8` (8 bytes: 01 02 00 03 02 00 01 03), pick
 *      either `LUT[3 + LUT[current]]` or `LUT[5 + LUT[current]]`
 *      depending on whether the target's LUT value is `<=` or
 *      `>` the current's, and write it as a 32-bit value to the
 *      status word.
 *
 * Sole caller: `bim_setup_after_cold_reset_cfg1` with
 * `target_code = 2`. The math is preserved verbatim against the
 * OEM rather than re-derived from first principles — the LUT
 * picks a safe stepping path that avoids forbidden direct
 * transitions between non-adjacent ADI modes. */
void bim_setup_adi_step(uint32_t target_code)
{
    static const uint8_t adi_step_lut[8] = {
        0x01, 0x02, 0x00, 0x03, 0x02, 0x00, 0x01, 0x03,
    };
    volatile uint32_t *adi = (volatile uint32_t *)0x400C6000u;

    for (;;) {
        uint32_t current;
        do {
            current = adi[0];
        } while (current != adi[1]);

        if (current == target_code) {
            return;
        }

        uint8_t cur_step = adi_step_lut[current];
        uint8_t tgt_step = adi_step_lut[target_code];
        uint8_t next;
        if (tgt_step <= cur_step) {
            next = adi_step_lut[3u + cur_step];
        } else {
            next = adi_step_lut[5u + cur_step];
        }
        adi[0] = next;
    }
}
