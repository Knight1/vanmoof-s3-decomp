/* oad_header.c — TI OAD-NVM1 image header for bleware 1.4.01.
 *
 * Pinned to flash 0x00000000..0x0000008F by the linker via the
 * `.oad_header` section. bleboot's BIM parses this header on every
 * cold-reset / image-promotion cycle to verify the image, pick the
 * `prgEntry` to jump to, and route the result of a successful image
 * promotion back into internal flash.
 *
 * The exact byte values below come from the OEM bleware_1.4.01.bin
 * raw bytes 0..0x8F (see docs/hardware.md for the field-by-field
 * decode). The `crc32` field is the precomputed CRC32 over the entire
 * image (excluding `crcStat`) — when we eventually produce a build
 * byte-equivalent to the OEM, this value matches; for now we leave it
 * at the OEM constant so bleboot still accepts the image as valid.
 *
 * Refs: TI OAD framework in SimpleLink CC13x2_CC26x2 SDK 3.40,
 * `oad/oad_image_header.h`.
 */

#include <stdint.h>

/* Packed for ABI-stable byte layout. */
struct __attribute__((packed)) oad_image_header_t {
    uint8_t  imgID[8];        /* "OAD NVM1" — NVM-bank-1 external-flash image marker */
    uint32_t crc32;           /* precomputed CRC over the rest of the image */
    uint8_t  bimVer;          /* BIM expects ≥ this version */
    uint8_t  metaVer;         /* OAD metadata format version */
    uint16_t techType;        /* tech-type word (0xFFFE = "any") */
    uint8_t  imgCpStat;       /* copy status — 0xFE = "promoted" */
    uint8_t  crcStat;         /* CRC validity — 0xFE once BIM has verified */
    uint8_t  imgType;         /* 0x07 = "application" */
    uint8_t  imgNo;           /* image slot number */
    uint32_t imgValidation;   /* unused in OAD-NVM1 (= 0xFFFFFFFF) */
    uint32_t len;             /* total image length including header */
    uint32_t prgEntry;        /* post-header offset to the program image */
    uint8_t  softVer[4];      /* software version: 00 MM mm pp */
    uint32_t imgEndAddr;      /* len - 1 */
    uint16_t hdrLen;          /* core-header length (= 0x002C) */
    uint16_t rfu;             /* reserved (= 0xFFFF) */
    /* Extended header — security/signature material, 100 B */
    uint8_t  ext[0x90 - 0x2C];
};

__attribute__((section(".oad_header"), used))
const struct oad_image_header_t g_oad_image_header = {
    .imgID         = { 'O', 'A', 'D', ' ', 'N', 'V', 'M', '1' },
    .crc32         = 0xB79C4373u,    /* OEM precomputed CRC */
    .bimVer        = 0x03u,
    .metaVer       = 0x01u,
    .techType      = 0xFFFEu,
    .imgCpStat     = 0xFFu,
    .crcStat       = 0xFFu,
    .imgType       = 0x07u,
    .imgNo         = 0x00u,
    .imgValidation = 0xFFFFFFFFu,
    .len           = 0x0002C67Cu,
    .prgEntry      = 0x00000090u,
    .softVer       = { 0x00, 0x01, 0x04, 0x01 },
    .imgEndAddr    = 0x0002C67Bu,
    .hdrLen        = 0x002Cu,
    .rfu           = 0xFFFFu,
    /* Extended header — 100 B of OEM bytes (security/signature material).
     * Default-initialised to 0xFF (= flash-erased) — non-byte-equivalent
     * but bleboot's CRC check verifies against the precomputed `crc32`
     * above, so a real byte-equivalent build will need to copy these
     * bytes from the OEM exactly. TBD as part of byte-equivalence work. */
    .ext = { 0xFFu },  /* designated-initialiser default-fills the rest */
};
