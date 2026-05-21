/* setup_trim.c — bleware SetupTrimDevice mirror.
 *
 * Confirmed byte-equivalent to bleboot's `SetupTrimDevice` @ flash
 * 0x0005667C (108 B). bleware's body lives at flash 0x0001878C
 * (106 B). Steps verified instruction-for-instruction against the
 * OEM disassembly — only compiler-codegen differences (CGT/armcc on
 * bleware vs GCC on bleboot, shared pool word for two related
 * MMIO addresses) separate the two byte layouts.
 *
 * Body, in order:
 *   1. Read FCFG1_FCFG1_REVISION at 0x5000131C; clamp 0xFFFFFFFF to 0.
 *   2. Assert silicon family/revision via bim_chip_assert_supported.
 *   3. Clear FLASH_FSM_ACK (bit 1 of FLASH+0x24) via bit-band 0x42600484.
 *   4. Call HAPI sub-table slot [18] (ROM helper) via the pointer
 *      stored at ROM_API_TABLE[28] (0x100001F0).
 *   5. If the bit-banded gate at 0x43280180 is set: read 0x43200580
 *      (side-effect) and call bim_setup_after_cold_reset_cfg1(rev).
 *   6. Clear AON_PMCTL register at 0x4008218C.
 *   7. Mask FLASH_CTRL register at 0x40032048: clear bits [27:17],
 *      OR in 0x01390000.
 *   8. Conditionally pulse 0x20000 in AON_PMCTL register at 0x40090028
 *      when bits [13:12] == 1.
 *   9. Spin until VIMS_STAT bit 3 (mode-change in progress) clears,
 *      via bit-band 0x4268000C.
 */

#include "bleware.h"
#include "cc2642r1.h"

#include <stdint.h>

void SetupTrimDevice(void)
{
    uint32_t fcfg1_rev = FCFG1_FCFG1_REVISION;
    if (fcfg1_rev == 0xFFFFFFFFu) {
        fcfg1_rev = 0u;
    }

    bim_chip_assert_supported();

    FLASH_BB_FSM_ACK = 0u;

    void (**const hapi)(void) = (void (**)(void))ROM_HAPI_TABLE_PTR;
    hapi[18]();

    if (BB_AON_GATE_43280180 != 0u) {
        (void)BB_AON_READ_43200580;
        bim_setup_after_cold_reset_cfg1(fcfg1_rev);
    }

    AON_REG_4008218C = 0u;

    uint32_t flash_ctl = FLASH_REG_40032048;
    flash_ctl &= ~(((1u << 11) - 1u) << 17);  /* clear bits [27:17] */
    FLASH_REG_40032048 = 0x01390000u | flash_ctl;

    uint32_t aon = AON_REG_40090028;
    if (((aon >> 12) & 0x3u) == 1u) {
        uint32_t masked = AON_REG_40090028 & 0xFCFCFFEFu;
        AON_REG_40090028 = masked | 0x20000u;
        AON_REG_40090028 = masked;
    }

    while (VIMS_BB_MODE_CHANGING != 0u) {
        /* spin until VIMS mode-change completes */
    }
}

/* ---- vendor-stock helpers SetupTrimDevice depends on ---------------
 *
 * These are stubs for now. The OEM bodies are at:
 *   bim_chip_assert_supported     @ 0x000266C8
 *   bim_chip_family               @ 0x00025D24
 *   bim_chip_hw_revision          @ 0x00021BCC
 *   bim_setup_after_cold_reset_cfg1 @ 0x000173E8
 *
 * Decoding each is a separate decomp task. For the skeleton build,
 * we provide minimal stubs that satisfy the link and (where
 * possible) the runtime behaviour. */

__attribute__((weak))
uint32_t bim_chip_family(void)
{
    /* CC13x2/CC26x2 = family 4. Hardcode for now — the OEM reads
     * this out of an FCFG1 chip-info word. */
    return 4u;
}

__attribute__((weak))
uint32_t bim_chip_hw_revision(void)
{
    /* HwRev 0x14 or later is required. Hardcode to a safe value
     * for the skeleton. */
    return 0x14u;
}

__attribute__((weak))
void bim_chip_assert_supported(void)
{
    if (bim_chip_family() != 4u) {
        for (;;) { /* halt */ }
    }
    if (bim_chip_hw_revision() < 0x14u) {
        for (;;) { /* halt */ }
    }
}

__attribute__((weak))
void bim_setup_after_cold_reset_cfg1(uint32_t fcfg1_rev)
{
    /* TI driverlib SetupAfterColdResetWakeupFromShutDownCfg1 — the
     * full body is ~148 B of pool-word-heavy MMIO setup. Stubbed
     * for the skeleton; decoding tracked separately. */
    (void)fcfg1_rev;
}
