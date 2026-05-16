/* TI driverlib `chipinfo` mirror — three small helpers that the
 * BIM compiles directly into its image rather than calling out of
 * ROM. They check that we're actually running on a supported
 * CC13x2/CC26x2 silicon revision before the rest of
 * `SetupTrimDevice` proceeds; on a mismatch
 * `bim_chip_assert_supported` spin-traps forever.
 *
 * Upstream: `source/ti/devices/cc13x2_cc26x2/driverlib/chipinfo.c`
 * in the TI SimpleLink CC13x2_CC26x2 SDK 3.40.x. The OEM image
 * built against an *older* driverlib than current public source
 * (the case-2/case-3 HW-rev constants below differ from the
 * present-day SDK numbers; preserved verbatim against the OEM). */

#include <stdint.h>

#include "bim.h"

/* FCFG1 (factory configuration) is mapped at `0x5000_1000`; the BIM
 * touches two fields:
 *
 *   - `ICEPICK_DEVICE_ID` (offset `0x318`): a 32-bit ICEPICK / JTAG
 *     readout mirror whose bits [27..12] hold the silicon PARTNO
 *     and bits [31..28] hold the PG (process-generation) revision.
 *   - `MINOR_HW_REV` (offset `0x0A0`): low byte is the silicon's
 *     in-PG minor revision (e.g. 1 → "PG2.1.1"). Values above 0x7F
 *     are read as "no minor rev assigned" and treated as 0.
 *
 * PARTNO `0xBB41` is the CC13x2/CC26x2 family identifier (returned
 * as `FAMILY_CC13x2_CC26x2 = 4` to match TI's `ChipFamily_t` enum).
 * Any other PARTNO returns `FAMILY_Unknown = -1`. */
#define FCFG1_ICEPICK_DEVICE_ID    (*(volatile uint32_t *)0x50001318u)
#define FCFG1_MINOR_HW_REV         (*(volatile uint32_t *)0x500010A0u)

#define CHIP_PARTNO_CC13x2_CC26x2  0xBB41u
#define FAMILY_CC13x2_CC26x2       4
#define FAMILY_Unknown             (-1)

#define HWREV_1_0                  10
#define HWREV_1_1                  11
#define HWREV_2_1                  21
#define HWREV_Unknown              (-1)
#define HWREV_2_0                  20  /* gate value in `bim_chip_assert_supported` */

/* `ChipInfo_GetChipFamily` (`FUN_00057020` in the OEM, 24 B + 4 B
 * literal). Reads FCFG1.ICEPICK_DEVICE_ID and extracts PARTNO via
 * the OEM's two-shift idiom (`(id << 4) >> 16` == `(id >> 12) &
 * 0xFFFF`, matching the device-ID PARTNO field at bits [27..12]).
 * Returns `FAMILY_CC13x2_CC26x2 (=4)` on a CC13x2/CC26x2 part,
 * `FAMILY_Unknown (=-1)` otherwise. */
int32_t bim_chip_family(void)
{
    uint32_t id = FCFG1_ICEPICK_DEVICE_ID;
    if (((id << 4) >> 16) == CHIP_PARTNO_CC13x2_CC26x2) {
        return FAMILY_CC13x2_CC26x2;
    }
    return FAMILY_Unknown;
}

/* `ChipInfo_GetHwRevision` (`FUN_00056bac` in the OEM, 64 B + 4 B
 * literal). Returns the OEM driverlib's `HwRevision_t` enum:
 *
 *     pg_rev (= device_id[31:28])  →  result
 *           0 or 1                 →  HWREV_1_0  (10)
 *           2                      →  HWREV_1_1  (11) + minor
 *           3                      →  HWREV_2_1  (21) + minor
 *           else                   →  HWREV_Unknown (-1)
 *
 * Where `minor` is the bottom byte of FCFG1.MINOR_HW_REV, capped to
 * 0 if it reads >0x7F (silicon convention: high-bit-set means
 * "no minor rev").
 *
 * **Quirk**: the OEM driverlib version baked into this BIM (Apr 2020)
 * uses `HWREV_1_1 + minor` for the PG2 case, not `HWREV_2_0 + minor`
 * as present-day TI SDK does. Either an older numbering convention
 * or a TI driverlib bug; preserved as observed. The result drives
 * `bim_chip_assert_supported`'s `>= 20` gate, which means PG2
 * silicon (returning 11..) reaches the spin-trap unless the
 * function never actually executes — confirming the working bikes'
 * shipping silicon must be PG3 (case 3 → 21+, passes the gate). */
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

/* `ThisLibraryIsFor_CC13x2_CC26x2_HwRev20AndLater_HaltIfViolated`
 * (`FUN_00057110` in the OEM, 22 B). Driverlib's compile-time-vs-
 * runtime safety net: if either the chip family is wrong or the
 * silicon revision is older than PG2.0 (HWREV_2_0 = 20), spin
 * forever rather than executing setup code that assumes newer
 * silicon and may brick the chip. Called from `SetupTrimDevice`
 * (vendor-stock) as one of the very first instructions of cold
 * boot. */
__attribute__((noinline)) void bim_chip_assert_supported(void)
{
    if (bim_chip_family() != FAMILY_CC13x2_CC26x2 ||
        bim_chip_hw_revision() < HWREV_2_0) {
        for (;;) { }
    }
}
