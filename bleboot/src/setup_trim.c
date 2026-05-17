/* TI driverlib `SetupTrimDevice` mirror — silicon trim entry called
 * from Reset_Handler before anything else.
 *
 * OEM body at flash 0x0005667C is 108 B. Steps (verbatim from the
 * OEM disassembly):
 *   1. Read FCFG1.FCFG1_REVISION at 0x5000131C; clamp 0xFFFFFFFF to 0.
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
 *
 * The MMIO addresses are documented in hardware.md. Register naming
 * stays neutral (raw addresses) for the few slots whose role in the
 * CC2642R1F TRM hasn't been pinned yet. */

#include <stdint.h>

#include "bim.h"

/* FCFG1 (Factory Configuration 1) fields. */
#define FCFG1_FCFG1_REVISION   (*(volatile uint32_t *)0x5000131Cu)

/* FLASH controller bit-banded acks (FLASH+0x24, alias base 0x42600000). */
#define FLASH_BB_FSM_ACK       (*(volatile uint32_t *)0x42600484u) /* bit 1 */

/* ROM HAPI sub-table — pointer-to-table-of-pointers at 0x100001F0.
 * Slot [18] is the cold-reset hook the trim sequence kicks before
 * touching any silicon-config registers. */
#define ROM_HAPI_TABLE_PTR     (*(void *const *volatile)0x100001F0u)

/* Bit-banded AON gates whose role isn't yet pinned to a TRM name. */
#define BB_AON_GATE_43280180   (*(volatile uint32_t *)0x43280180u)
#define BB_AON_READ_43200580   (*(volatile uint32_t *)0x43200580u)

/* AON_PMCTL registers (raw — TRM names pending). */
#define AON_REG_4008218C       (*(volatile uint32_t *)0x4008218Cu)
#define FLASH_REG_40032048     (*(volatile uint32_t *)0x40032048u)
#define AON_REG_40090028       (*(volatile uint32_t *)0x40090028u)

/* VIMS_STAT bit 3 (mode-change in progress), bit-banded. */
#define VIMS_BB_MODE_CHANGING  (*(volatile uint32_t *)0x4268000Cu)

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
