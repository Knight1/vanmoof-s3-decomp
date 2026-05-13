/* image.c — flash-resident image header validation.
 *
 * The OEM has a manifest-style record at flash 0x08001800 (in
 * shifterboot's region — likely an OTA candidate or backup slot). The
 * record starts with a 40-byte header followed by a payload. The
 * stored CRC32 is computed over the whole record, except the `crc32`
 * and `length` fields themselves which are masked to 0xFFFFFFFF (the
 * value a freshly erased flash cell holds) during CRC compute. */

#include "image.h"
#include "crc.h"
#include "flash_store.h"
#include "modbus.h"
#include "util.h"
#include <stdint.h>

#define IMAGE_HDR_SIZE  0x28u   /* 40 bytes */
#define IMAGE_HDR_WORDS 10u
#define IMAGE_MAX_SIZE  0x3000u /* 12 KB */

typedef struct {
    uint32_t magic;       /* 0x00 */
    uint32_t _u04;        /* 0x04 — TBD (build id?) */
    uint32_t crc32;       /* 0x08 — CRC of full record with crc32/length masked to 0xFFFFFFFF */
    uint32_t length;      /* 0x0C — total record bytes (header + payload) */
    uint32_t _u10[6];     /* 0x10..0x27 — build-date, reserved */
} image_header_t;

/* OEM keeps both the image pointer and the expected magic as `const`
 * data in .rodata (DAT_08003DB8 and DAT_08003DBC respectively). */
static const image_header_t *const g_image       = (const image_header_t *)0x08001800u;
static const uint32_t              g_image_magic = 0xAA55AA55u;

/* OEM @ 0x08003AC6 (100 B). */
int image_verify_crc(void)
{
    const image_header_t *img = g_image;

    if (img->magic   != g_image_magic) return 2;
    if (img->length  >= IMAGE_MAX_SIZE) return 2;

    crc_reset();

    /* CRC the header with the crc32 and length fields masked. */
    uint32_t copy[IMAGE_HDR_WORDS];
    memcpy(copy, img, IMAGE_HDR_SIZE);
    copy[2] = 0xFFFFFFFFu; /* mask out stored crc32 */
    copy[3] = 0xFFFFFFFFu; /* mask out length */
    (void)crc32_words(copy, (int)IMAGE_HDR_WORDS);

    /* Continue the CRC over the payload. */
    const uint32_t payload_words = (img->length - IMAGE_HDR_SIZE) / 4u;
    const uint32_t *payload = (const uint32_t *)((const uint8_t *)img + IMAGE_HDR_SIZE);
    const uint32_t got = crc32_words(payload, (int)payload_words);

    return (got == img->crc32) ? 0 : 1;
}

/* ---- image_apply: OEM @ 0x08003B2A (92 B) ----
 *
 * Called after the receive-slot at 0x08001800 has been written. Validates
 * it; if good, latches the "image OK" flag; if bad, erases the slot
 * (12 pages = 12 KB) and resets the receive-state RAM bytes. Also
 * always: snapshots image_verify_crc's result, extracts a 7-bit value
 * from an as-yet-unidentified RAM scratch struct (@0x200000C8 +4..+5),
 * and pings the bus status reporter.
 *
 * Several of the RAM globals here aren't yet linked to module owners;
 * they're accessed by raw address until a wider decomp pass identifies
 * their actual `extern` declarations.
 */

#include <stdbool.h>

/* Opaque RAM globals shared between image_apply and report_image_status.
 * Accessed by raw address to match OEM bytes; will be replaced by
 * proper `extern` declarations once their owning modules are
 * identified. */
#define G_C8_SCRATCH  ((volatile uint8_t    *)0x200000C8u)
#define G_D80_BYTE    (*(volatile uint8_t   *)0x20000140u)
#define G_D88_PTR     (*(volatile const void **)0x200000E0u)
#define G_D90_BYTE    (*(volatile uint8_t   *)0x200000D9u)
#define G_D94_BYTE    (*(volatile uint8_t   *)0x2000013Fu)
#define G_VERSION_BYTE (*(volatile uint8_t  *)0x20000141u)
#define G_PKT_BYTES   ((volatile uint8_t    *)0x20000142u)
#define G_IMG_STATUS  (*(volatile uint8_t   *)0x200000E9u)
#define G_IMG_OK      (*(volatile uint8_t   *)0x200000DAu)
#define G_E7_BYTE     (*(volatile uint8_t   *)0x200000E7u) /* CRC lo from modbus_crc16_compute */
#define G_E8_BYTE     (*(volatile uint8_t   *)0x200000E8u) /* CRC hi from modbus_crc16_compute */
#define MODBUS_TX_BUF ((uint8_t *)0x200001A9u)

/* OEM @ 0x08003A86 (64 B). Builds a 7-byte Modbus PDU from scattered
 * RAM state, transmits it, and (via modbus_tx_finalize's side effect)
 * reboots if image_apply just latched a freshly-validated firmware. */
void report_image_status(void)
{
    uint8_t *buf = MODBUS_TX_BUF;

    buf[0] = G_C8_SCRATCH[0];
    buf[1] = G_C8_SCRATCH[1];
    buf[2] = G_VERSION_BYTE;
    buf[3] = G_PKT_BYTES[0];
    buf[4] = G_PKT_BYTES[1];

    modbus_crc16_compute(buf, 5);

    buf[5] = G_E7_BYTE;
    buf[6] = G_E8_BYTE;

    modbus_tx_finalize(7u);
}

void image_apply(void)
{
    G_IMG_STATUS = (uint8_t)image_verify_crc();

    /* Extract bottom-7 bits of a big-endian uint16 stored at offsets
     * +4/+5 of the C8 scratch struct, doubled. */
    const uint16_t v = ((uint16_t)G_C8_SCRATCH[4] << 8) | G_C8_SCRATCH[5];
    G_VERSION_BYTE = (uint8_t)((v & 0x7Fu) << 1);

    G_PKT_BYTES[0] = 0u;
    G_PKT_BYTES[1] = G_IMG_STATUS;

    report_image_status();

    if (G_IMG_STATUS == 0u) {
        G_IMG_OK = 1u;
    } else {
        flash_erase_pages((uint32_t)g_image, 12);
        G_D90_BYTE = 0u;
        G_D94_BYTE = 0u;
        G_D80_BYTE = 0u;
        G_D88_PTR  = g_image;
    }
}
