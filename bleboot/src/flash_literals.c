/* Flash literals at fixed offsets in the bleboot image — the BIM's
 * pre-computed tables and constant blobs that the code references
 * by absolute address.
 *
 * Each item is emitted into its own named section so the linker can
 * pin them to the OEM addresses (see linker_cc2642r1.ld). Without
 * the pin, GCC -ffunction-sections -fdata-sections would place the
 * sections in arbitrary order and the hard-coded pointers in flash.c
 * and oad.c (e.g. `0x000571A8`, `0x000571F0`, `0x000571F4`,
 * `0x000571E8`) would no longer resolve.
 *
 * Layout (matches OEM bleboot_1.0.0.bin):
 *   0x000571A8  chip database table (48 B)
 *   0x000571D8  ADI step LUT (8 B)
 *   0x000571E0  "OAD NVM1" magic string #1 (8 B)
 *   0x000571E8  "OAD NVM1" magic string #2 (8 B)
 *   0x000571F0  REMS command word (4 B)
 *   0x000571F4  RDSR + WREN opcodes (4 B)
 *   0x000571F8  TI CGT handler dispatch table (12 B + 4 B pad)
 *   0x00057208  cinit body data (16 B)
 *   0x00057218  cinit record table (16 B)
 *
 * Together with `_auto_init_table` (auto_init.c) this provides the
 * TI compiler-runtime cinit pass that fires before main(). */

#include <stdint.h>

/* ----- 0x000571A8: SPI flash chip database (6 entries x 8 B) -----
 *
 * Entry layout: { uint32_t capacity_bytes; uint8_t mfr; uint8_t dev;
 *                 uint8_t reserved[2]; }. Probed via REMS (0x90)
 * which returns just mfr+dev; capacity is what the BIM caps reads
 * against. Sentinel: capacity == 0. The installed flash on the
 * S3 BLE PCB is the MX25L51245G (0xC2/0x19, 64 MB). */
struct bim_chip_entry {
    uint32_t capacity;
    uint8_t  mfr;
    uint8_t  dev;
    uint8_t  reserved[2];
};

__attribute__((section(".bim_flash.chip_table"), used))
const struct bim_chip_entry bim_chip_table[6] = {
    { 0x04000000u, 0xC2, 0x19, {0, 0} },  /* MX25L51245G  64 MB */
    { 0x00200000u, 0xC2, 0x15, {0, 0} },  /* MX25L1606E    2 MB */
    { 0x00100000u, 0xC2, 0x14, {0, 0} },  /* MX25L805C     1 MB */
    { 0x00080000u, 0xEF, 0x12, {0, 0} },  /* W25Q40       512 KB */
    { 0x00040000u, 0xEF, 0x11, {0, 0} },  /* W25Q20       256 KB */
    { 0x00000000u, 0x00, 0x00, {0, 0} },  /* sentinel */
};

/* ----- 0x000571D8: ADI sequencer stepping LUT (8 B) -----
 *
 * Used by bim_setup_adi_step to pick the next analog-config code on
 * the path to target_code. Indexed by the current code (returns next
 * intermediate) or by `LUT[3 + LUT[cur]]` (step-down) and
 * `LUT[5 + LUT[cur]]` (step-up) — the latter two indices land in
 * the table's tail region. */
__attribute__((section(".bim_flash.adi_lut"), used))
const uint8_t bim_adi_step_lut[8] = {
    0x01, 0x02, 0x00, 0x03, 0x02, 0x00, 0x01, 0x03,
};

/* ----- 0x000571E0 / 0x000571E8: "OAD NVM1" magic, twice -----
 *
 * 8-byte ASCII magic that marks an external SPI flash slot as an OAD
 * staging area. Two copies exist because two separate TUs (oad.c
 * functions oad_magic_match and oad_magic_match2) each emitted their
 * own static-inlined memcmp + private string copy. The duplication
 * is a TI CCS artifact; preserved verbatim. */
__attribute__((section(".bim_flash.oad_nvm1_a"), used))
const char bim_oad_magic_a[8] = "OAD NVM1";

__attribute__((section(".bim_flash.oad_nvm1_b"), used))
const char bim_oad_magic_b[8] = "OAD NVM1";

/* ----- 0x000571F0: SPI flash command literals (8 B) -----
 *
 * Four bytes of REMS command (`90 FF FF 00` — opcode + 24-bit dummy
 * address; low byte = 0 selects "mfr first" output ordering), then
 * the RDSR and WREN opcodes packed into the same word at +4/+5 so
 * the OEM can address them through a shared literal-pool entry. */
__attribute__((section(".bim_flash.spi_opcodes"), used))
const uint8_t bim_spi_opcodes[8] = {
    0x90, 0xFF, 0xFF, 0x00,   /* REMS command */
    0x05,                      /* RDSR (read status) */
    0x06,                      /* WREN (write enable) */
    0xFF, 0xFF,                /* padding (OEM leaves 0xFF here) */
};

/* ----- 0x000571F8..0x00057204: TI CGT handler dispatch table -----
 *
 * Three function pointers indexed by the first byte of each cinit
 * record's body. Index 0 = byte-stream copy (`0x00056924`, still
 * undecoded), index 1 = generic copy (`0x00057148`, undecoded),
 * index 2 = zero-fill (`auto_init_zero_fill`).
 *
 * The trailing zero word at 0x00057204 is the `__TI_Handler_Table_Limit`
 * sentinel value the walker compares against. */
extern void auto_init_zero_fill(const void *body, void *dst);

typedef void (*cinit_handler_t)(const void *body, void *dst);

/* Forward declarations for the two cinit handlers Ghidra hasn't
 * promoted to functions yet. They live at fixed flash addresses in
 * the OEM image; their bodies stay opaque until we decode them. The
 * dispatch table needs the pointers, so reference them as externs
 * the linker will resolve once they exist in C. For now, weak null
 * placeholders keep the build working. */
__attribute__((weak)) void cinit_byte_stream_copy(const void *body, void *dst) { (void)body; (void)dst; }
__attribute__((weak)) void cinit_generic_copy(const void *body, void *dst) { (void)body; (void)dst; }

__attribute__((section(".bim_flash.cinit_handlers"), used))
cinit_handler_t const __TI_Handler_Table_Base[4] = {
    cinit_byte_stream_copy,        /* type 0 — body @ 0x57210 */
    cinit_generic_copy,            /* type 1 */
    auto_init_zero_fill,           /* type 2 — body @ 0x57208 */
    (cinit_handler_t)0,            /* __TI_Handler_Table_Limit sentinel */
};

/* The walker actually reads `__TI_Handler_Table_Limit` as a separate
 * external symbol; the linker provides it via PROVIDE() in the linker
 * script as `__TI_Handler_Table_Base + 12`. */

/* ----- 0x00057208..0x00057218: cinit body data (16 B) -----
 *
 * Two record bodies, referenced from the cinit table below:
 *   body @ 0x57208: type=2 (zero-fill), length-at-+4 = 0x108 (264 B)
 *                   - clears SRAM 0x20000300..0x20000408 on boot
 *                     (CRC scratch buffer + chip table cursor).
 *   body @ 0x57210: type=0 (byte-stream copy), payload encodes a
 *                   small inline-data copy. The OEM bytes are
 *                   {00 01 00 00 00 ff f0 00} — preserved verbatim
 *                   until the byte-stream-copy handler is decoded. */
__attribute__((section(".bim_flash.cinit_bodies"), used))
const uint8_t bim_cinit_bodies[16] = {
    /* 0x57208: zero-fill body */
    0x02, 0x00, 0x00, 0x00,   /* type byte + 3 B alignment */
    0x08, 0x01, 0x00, 0x00,   /* length = 0x108 */
    /* 0x57210: byte-stream-copy body */
    0x00, 0x01, 0x00, 0x00,
    0x00, 0xFF, 0xF0, 0x00,
};

/* ----- 0x00057218..0x00057228: cinit record table (2 x 8 B) -----
 *
 * Each record is (body_ptr, dst_ptr). _auto_init_table walks
 * __TI_CINIT_Base..__TI_CINIT_Limit (16 B in this build), dispatching
 * each through the handler table indexed by body[0]. */
struct cinit_record {
    const uint8_t *body;
    void          *dst;
};

extern uint8_t g_bim_sram_zero_fill_start[];      /* 0x20000300 */
extern uint8_t g_bim_sram_byte_stream_dst[];      /* 0x20000408 */

__attribute__((section(".bim_flash.cinit_table"), used))
const struct cinit_record __TI_CINIT_Base[2] = {
    { &bim_cinit_bodies[0], (void *)0x20000300u },   /* zero-fill 264 B at CRC scratch */
    { &bim_cinit_bodies[8], (void *)0x20000408u },   /* byte-stream copy at chip cursor */
};
