/* setup_cold_reset.c — TI driverlib cold-reset trim helpers.
 *
 * Two functions, both lifted in-binary by the OEM (instead of being
 * dispatched through the CC2642 boot ROM) and called out of
 * SetupTrimDevice's main path:
 *
 *   bim_setup_after_cold_reset_cfg1 (bleware @ flash 0x000173E8,
 *                                    148 B + 24 B literal pool)
 *     — first of three cold-reset sub-helpers; runs only on a true
 *     cold boot. Pokes FCFG2/FCFG1 fuse readouts into ADI3 + DDI0
 *     trim registers and forwards the FCFG1 revision to the ROM
 *     cold-reset state machine reachable via ROM_API_TABLE[28]
 *     (= ROM_HAPI_TABLE_PTR at 0x100001F0).
 *
 *   bim_setup_adi_step (bleware @ flash 0x00023AF4, 42 B + 8 B
 *                       literal pool + 8 B LUT at flash 0x0002BA64)
 *     — analog-config sequencer. Steps the live ADI mode at MMIO
 *     0x400C6000 toward a requested target by looking up the next
 *     intermediate code through an 8-byte transition LUT. Called
 *     once from cfg1 with target = 2.
 *
 * Upstream is TI's SimpleLink CC13x2_CC26x2 SDK 3.40 driverlib
 * (source/ti/devices/cc13x2_cc26x2/driverlib/setup.c). The OEM
 * BIM/bleware bake copies the driverlib bodies verbatim — same
 * code is also present in bleboot at the same shape. Magic
 * constants below are preserved against the bleware disassembly
 * rather than rederived from current SDK source.
 */

#include "bleware.h"

#include <stdint.h>

/* FCFG2 (0x50004000) cold-reset trim words at offset 0xFAC..0xFB4.
 * FCFG2 isn't in the public CC2642R1 TRM but its access shape and
 * adjacency to FCFG1 fit the role TI's chipinfo helpers play. */
#define FCFG2_TRIM_WORD0   (*(volatile uint32_t *)0x50004FACu)
#define FCFG2_TRIM_WORD1   (*(volatile uint32_t *)0x50004FB0u)
#define FCFG2_TRIM_WORD2   (*(volatile uint32_t *)0x50004FB4u)

/* ADI3 (base 0x400CB000) — recharge / DCDC / REFSYS trims. cfg1
 * writes a byte at +0x0E and a halfword at +0x6A (= +0x0E + 0x5C). */
#define ADI3_TRIM_BYTE     (*(volatile uint8_t  *)0x400CB00Eu)
#define ADI3_TRIM_HALF     (*(volatile uint16_t *)0x400CB06Au)

/* DDI0 OSC (base 0x400CA000) — RF/clock trims. cfg1 writes
 * 0x01000100 to +0x404 and does a 16-bit dummy readback at +0x000
 * to flush the DDI write buffer. */
#define DDI0_OSC_REG_0     (*(volatile uint16_t *)0x400CA000u)
#define DDI0_OSC_REG_404   (*(volatile uint32_t *)0x400CA404u)

/* FCFG1.MISC_TRIM_AON (offset 0x40C in FCFG1 base 0x50001000).
 * cfg1 reads it once and slices three fields out: the ADI3 byte,
 * a bit-8 dispatch selecting one of two AON-shadow byte writes,
 * and the ADI3 halfword. */
#define FCFG1_MISC_TRIM    (*(volatile uint32_t *)0x5000140Cu)

/* 0x40086209..0x40086256 — AON_PMCTL / AON_BATMON DCDC-trim shadow
 * byte array. Base address is offset by 1 so the OEM can fold four
 * byte writes sharing one literal pool word. Field offsets:
 *
 *   +0x00 (0x40086209): byte = 2     if r5 bit 24 set & bit 25 clear
 *   +0x13 (0x4008621C): byte = 0x40  if FCFG1.MISC_TRIM bit 8 set
 *   +0x23 (0x4008622C): byte = 0x40  if FCFG1.MISC_TRIM bit 8 clear
 *   +0x4D (0x40086256): byte = (FCFG2_TRIM_WORD0 >> 16) | 0xF0
 */
#define AON_TRIM_SHADOW_BASE  ((volatile uint8_t *)0x40086209u)

/* ROM_API_TABLE[28] — the HAPI / cold-reset sub-table pointer.
 * cfg1 calls slots 0/1/2 in order, passing FCFG2_TRIM_WORD2 (the
 * user-ID word) as a common arg. */
#define BIM_HAPI_TABLE_PTR    ((const uintptr_t *const *)0x100001F0u)

/* Bit-banded write of bit 5 of FLASH+0x24 (= 0x40030024). This is
 * NOT the same latch as FSM_ACK (bit 1) — it's a "trim-applied"
 * marker the BIM/driverlib sets after every successful cfg1 pass. */
#define BIM_FLASH_TRIM_DONE_BIT (*(volatile uint32_t *)0x42600494u)

void bim_setup_after_cold_reset_cfg1(uint32_t fcfg1_rev)
{
    uint32_t fcfg2_w1 = FCFG2_TRIM_WORD1;
    if ((fcfg2_w1 & 0x2u) == 0u) {
        uint32_t fcfg2_w0 = FCFG2_TRIM_WORD0;
        AON_TRIM_SHADOW_BASE[0x4Du] = (uint8_t)((fcfg2_w0 >> 16) | 0xF0u);
    }

    DDI0_OSC_REG_404 = 0x01000100u;
    (void)DDI0_OSC_REG_0;

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

    /* OEM @ 0x17448: `lsrs r1,r0,#0x9` shifts bit[8] into the carry flag
     * (LSR #N -> carry = source bit[N-1]); the cc/cs branch keys off
     * FCFG1.MISC_TRIM bit 8, NOT bit 9. Ghidra renders this as
     * `if ((uVar4 >> 8 & 1) == 0)`. */
    if ((misc & (1u << 8)) == 0u) {
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

/* ADI mode-transition LUT — 8 bytes at flash 0x0002BA64 in the OEM
 * bleware. Same content as bleboot's at 0x000571D8. The first four
 * bytes map current mode → step code; the last four map step code
 * → next mode (split into ≤ and > branches at indices 3 and 5). */
static const uint8_t k_adi_step_lut[8] = {
    0x01, 0x02, 0x00, 0x03,  0x02, 0x00, 0x01, 0x03,
};

void bim_setup_adi_step(uint32_t target_code)
{
    volatile uint32_t *adi = (volatile uint32_t *)0x400C6000u;

    for (;;) {
        uint32_t current;
        do {
            current = adi[0];
        } while (current != adi[1]);

        if (current == target_code) {
            return;
        }

        uint8_t cur_step = k_adi_step_lut[current];
        uint8_t tgt_step = k_adi_step_lut[target_code];
        uint8_t next;
        if (tgt_step <= cur_step) {
            next = k_adi_step_lut[3u + cur_step];
        } else {
            next = k_adi_step_lut[5u + cur_step];
        }
        adi[0] = next;
    }
}
