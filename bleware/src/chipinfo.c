/* chipinfo.c — TI driverlib silicon-identification helpers.
 *
 * Three small helpers SetupTrimDevice gates on before doing any
 * real cold-reset trim work. The OEM compiles them in-binary rather
 * than dispatching through ROM, so we mirror them here.
 *
 *   bim_chip_family         (bleware @ flash 0x00025D24, 22 B body
 *                            + 4 B literal pool)
 *   bim_chip_hw_revision    (bleware @ flash 0x00021BCC, 62 B body
 *                            + 4 B literal pool)
 *   bim_chip_assert_supported (bleware @ flash 0x000266C8, 22 B)
 *
 * Upstream is TI's SimpleLink CC13x2_CC26x2 SDK 3.40 driverlib
 * (source/ti/devices/cc13x2_cc26x2/driverlib/chipinfo.c). The OEM
 * driverlib baked into this bleware (Apr 2020) numbers HwRevision_t
 * differently from current public SDK source — the case-2/case-3
 * minor-rev base values (11 and 21) are preserved verbatim against
 * the bleware disassembly. Byte-shape matches bleboot's mirror at
 * flash 0x00057020 / 0x00056BAC / 0x00057110.
 */

#include "bleware.h"

#include <stdint.h>

/* FCFG1 (factory configuration) is mapped at 0x50001000; bleware
 * reads two fields:
 *
 *   ICEPICK_DEVICE_ID  (offset 0x318): 32-bit ICEPICK/JTAG readout
 *     mirror. Bits [27:12] hold the silicon PARTNO; bits [31:28]
 *     hold the PG (process-generation) revision.
 *   MINOR_HW_REV       (offset 0x0A0): low byte is the silicon's
 *     in-PG minor revision (e.g. 1 -> "PG2.1.1"). Values above
 *     0x7F are read as "no minor rev assigned" and treated as 0.
 *
 * PARTNO 0xBB41 is the CC13x2/CC26x2 family identifier; matching
 * yields FAMILY_CC13x2_CC26x2 = 4 (TI's ChipFamily_t enum value).
 * Any other PARTNO returns FAMILY_Unknown = -1.
 */
#define FCFG1_ICEPICK_DEVICE_ID    (*(volatile uint32_t *)0x50001318u)
#define FCFG1_MINOR_HW_REV         (*(volatile uint32_t *)0x500010A0u)

#define CHIP_PARTNO_CC13x2_CC26x2  0xBB41u
#define FAMILY_CC13x2_CC26x2       4
#define FAMILY_Unknown             (-1)

#define HWREV_1_0                  10
#define HWREV_1_1                  11
#define HWREV_2_1                  21
#define HWREV_Unknown              (-1)
#define HWREV_2_0                  20  /* gate value in bim_chip_assert_supported */

/* bleware @ 0x00025D24. Reads FCFG1.ICEPICK_DEVICE_ID and extracts
 * PARTNO via the OEM driverlib's two-shift idiom: (id << 4) >> 16
 * == (id >> 12) & 0xFFFF, matching the PARTNO field at bits [27:12]. */
int32_t bim_chip_family(void)
{
    uint32_t id = FCFG1_ICEPICK_DEVICE_ID;
    if (((id << 4) >> 16) == CHIP_PARTNO_CC13x2_CC26x2) {
        return FAMILY_CC13x2_CC26x2;
    }
    return FAMILY_Unknown;
}

/* bleware @ 0x00021BCC. Decodes the OEM driverlib's HwRevision_t:
 *
 *     pg_rev (= device_id[31:28])  ->  result
 *           0 or 1                 ->  HWREV_1_0  (10)
 *           2                      ->  HWREV_1_1  (11) + minor
 *           3                      ->  HWREV_2_1  (21) + minor
 *           else                   ->  HWREV_Unknown (-1)
 *
 * `minor` is the low byte of FCFG1.MINOR_HW_REV, capped to 0 if it
 * reads > 0x7F (silicon convention: high-bit-set means "no minor
 * rev assigned").
 *
 * Quirk preserved from OEM: the case-2 base is HWREV_1_1 (11), not
 * HWREV_2_0 (20) as current public TI SDK source uses. The OEM
 * driverlib's `>= 20` gate in bim_chip_assert_supported therefore
 * traps PG1/PG2 silicon — shipping bikes must be PG3 (case 3 -> 21+,
 * passes the gate). */
int32_t bim_chip_hw_revision(void)
{
    uint32_t device_id  = FCFG1_ICEPICK_DEVICE_ID;
    uint32_t minor_word = FCFG1_MINOR_HW_REV;
    uint32_t pg_rev     = device_id >> 28;
    uint8_t  minor      = (uint8_t)(minor_word & 0xFFu);
    if (minor > 0x7Fu) {
        minor = 0u;
    }
    if (bim_chip_family() != FAMILY_CC13x2_CC26x2) {
        return HWREV_Unknown;
    }
    switch (pg_rev) {
    case 0:
    case 1:
        return HWREV_1_0;
    case 2:
        return (int32_t)(int8_t)(minor + HWREV_1_1);
    case 3:
        return (int32_t)(int8_t)(minor + HWREV_2_1);
    default:
        return HWREV_Unknown;
    }
}

/* bleware @ 0x000266C8. Driverlib's runtime safety net: spin forever
 * if the silicon is the wrong family or older than PG2.0. */
__attribute__((noinline)) void bim_chip_assert_supported(void)
{
    if (bim_chip_family() != FAMILY_CC13x2_CC26x2 ||
        bim_chip_hw_revision() < HWREV_2_0) {
        for (;;) { }
    }
}
