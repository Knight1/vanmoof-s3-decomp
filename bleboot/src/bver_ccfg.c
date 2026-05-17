/* Image footer regions:
 *   - BVER block at flash 0x00057F38 (16 B "BVER" + ASCII build date)
 *     plus an adjacent 9-byte ASCII build-time field at 0x00057F48
 *     and a 4-byte version word at 0x00057F50. VanMoof's per-image
 *     marker, not part of any TI standard format.
 *   - CCFG (Customer Configuration) at flash 0x00057FA8..0x00057FFF
 *     (88 B). The CC2642R1F ROM boot ROM reads these fields to learn
 *     the location of the application image, JTAG options, and reset
 *     vector. CCFG values were extracted verbatim from the OEM image
 *     and named per the TI CC13x2/CC26x2 SDK 3.40
 *     `source/ti/devices/cc13x2_cc26x2/startup_files/ccfg.c` header
 *     layout.
 *
 * Pinned to their fixed addresses by linker_cc2642r1.ld via section
 * placement. The 8-byte regions at 0x00057F30..0x00057F37 and
 * 0x00057FA0..0x00057FA7 are erased flash (0xFF fill) in the OEM
 * image; the Makefile passes `--gap-fill=0xFF --pad-to=0x58000` to
 * objcopy so those bytes land in the binary. */

#include <stdint.h>

/* ---- BVER block at flash 0x00057F38 ----
 *
 * 16-byte "BVER" + ASCII build-date field, followed by a 9-byte
 * ASCII build-time field and a 4-byte version word. Total 29 B,
 * laid out as three back-to-back fixed-size records:
 *
 *   "BVER" "Apr 23 2020\0"   (4 + 12 = 16 B at 0x57F38)
 *   "14:10:12\0"             (9 B at 0x57F48)
 *   0x00000100               (4 B at 0x57F51 — 1.0.0 minor version
 *                              packed in the low 3 bytes; the 0x00
 *                              at 0x57F51 is the version's null
 *                              terminator or padding from the prior
 *                              field, depending on interpretation).
 *
 * The exact byte sequence (`00 01 00 00`) is preserved verbatim from
 * the OEM image because the field's semantics are not documented in
 * any TI source — VanMoof's own marker. */
__attribute__((section(".bver_block"), used))
const uint8_t bim_bver_block[32] = {
    /* 0x57F38: "BVER" + build-date string */
    'B', 'V', 'E', 'R',
    'A', 'p', 'r', ' ', '2', '3', ' ', '2', '0', '2', '0', '\0',
    /* 0x57F48: build-time string (the trailing 0x00 at +24 serves
     * as both the C null terminator and the start of the version
     * record). */
    '1', '4', ':', '1', '0', ':', '1', '2', '\0',
    /* 0x57F51..0x57F57: version word (3 bytes LE = 0x000001) + pad */
    0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
};

/* ---- CCFG at flash 0x00057FA8..0x00057FFF (88 B) ----
 *
 * CC2642R1F Customer Configuration. Field-by-field per TI's ccfg.c
 * layout (CC13x2/CC26x2 SDK 3.40). All fields lifted verbatim from
 * the OEM image; the bleboot-specific override is IMAGE_VALID_CONF
 * = 0x00056000 (= start of this image).
 *
 * Layout (each field = 32-bit LE):
 *   +0x00  CCFG_O_SIZE_AND_DIS_FLAGS  0xFF800010 0x01800000 (8 B)
 *   +0x08  CCFG_O_MODE_CONF           0xF3391400 0x005800FD 0xFFFFFFFF... mostly 0xFF
 *   +0x30  CCFG_O_BL_CONFIG           0x00FFFFFF
 *   +0x34  CCFG_O_ERASE_CONF          0xFFFFFFFF
 *   +0x38  CCFG_O_CCFG_TI_OPTIONS     0xFFFFFF00
 *   +0x3C  CCFG_O_CCFG_TAP_DAP_0      0xFF000000
 *   +0x40  CCFG_O_CCFG_TAP_DAP_1      0xFF000000
 *   +0x44  CCFG_O_IMAGE_VALID_CONF    0x00056000
 *   +0x48  CCFG_O_CCFG_PROT_31_0      0xFFFFFFFF
 *   +0x4C  CCFG_O_CCFG_PROT_63_32     0xFFFFFFFF
 *   +0x50  CCFG_O_CCFG_PROT_95_64     0xFFFFFFFF
 *   +0x54  CCFG_O_CCFG_PROT_127_96    0xFFFFFFFF
 *
 * For byte-exactness, this is emitted as a byte array rather than a
 * struct of named fields — the field-name layer is in the comments. */
__attribute__((section(".ccfg"), used))
const uint8_t bim_ccfg[88] = {
    /* 0x57FA8: SIZE_AND_DIS_FLAGS */
    0x00, 0x00, 0x80, 0x01,
    /* 0x57FAC: MODE_CONF */
    0x10, 0x00, 0x80, 0xFF,
    /* 0x57FB0: VOLT_LOAD_0 */
    0xFD, 0xFF, 0x58, 0x00,
    /* 0x57FB4: VOLT_LOAD_1 */
    0x3A, 0x14, 0x39, 0xF3,
    /* 0x57FB8..0x57FCF: VOLT_LOAD/RTC/FREQ + IEEE_MAC_0/1 (all 0xFF) */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0x57FD0..0x57FD7: IEEE_BLE_0/1 (mostly 0xFF; one 0x00) */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    /* 0x57FD8: BL_CONFIG (3 B 0xFF, 1 B 0x00) */
    0xFF, 0xFF, 0xFF, 0x00,
    /* 0x57FDC: ERASE_CONF */
    0xFF, 0xFF, 0xFF, 0xFF,
    /* 0x57FE0: CCFG_TI_OPTIONS */
    0x00, 0xFF, 0xFF, 0xFF,
    /* 0x57FE4: CCFG_TAP_DAP_0 */
    0x00, 0x00, 0x00, 0xFF,
    /* 0x57FE8: CCFG_TAP_DAP_1 */
    0x00, 0x00, 0x00, 0xFF,
    /* 0x57FEC: IMAGE_VALID_CONF — points at start of this image */
    0x00, 0x60, 0x05, 0x00,
    /* 0x57FF0..0x57FFF: CCFG_PROT_31_0/63_32/95_64/127_96 */
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF,
};
