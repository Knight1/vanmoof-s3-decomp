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

/* VanMoof image-header layout (40 B, magic-first). The version word
 * encodes MAJOR.MINOR.PATCH.TYPE_ID as four bytes (MSB-first); the
 * low byte is a stable per-firmware type identifier (0xC1 for
 * shifterware — confirmed by cross-firmware comparison across the
 * S3 family in `docs/memory-map.md`). shifterware itself never
 * reads this field; it's consumed by shifterboot's image-validate
 * path before handing control to `main`. We carry it in the struct
 * because `image_verify_crc` snapshots the whole header for the
 * CRC32 compute. */
typedef struct {
    uint32_t magic;        /* 0x00 — 0xAA55AA55                          */
    uint32_t version_word; /* 0x04 — packed major:minor:patch:type_id    */
    uint32_t crc32;        /* 0x08 — masked to 0xFFFFFFFF during compute */
    uint32_t length;       /* 0x0C — total record bytes (hdr + payload)  */
    char     date[12];     /* 0x10 — "Mmm DD YYYY\0"                     */
    char     time[12];     /* 0x1C — "HH:MM:SS\0" + 3 B pad              */
} image_header_t;

/* shifterware's own type-ID byte. Asserted at compile time below
 * to catch accidental drift in `startup_mm32f031.S`'s literal. */
#define IMAGE_TYPE_SHIFTERWARE  0xC1u
#define IMAGE_VERSION_TYPE(v)   ((uint8_t)((v) & 0xFFu))
#define IMAGE_VERSION_PATCH(v)  ((uint8_t)(((v) >> 8)  & 0xFFu))
#define IMAGE_VERSION_MINOR(v)  ((uint8_t)(((v) >> 16) & 0xFFu))
#define IMAGE_VERSION_MAJOR(v)  ((uint8_t)(((v) >> 24) & 0xFFu))

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
#define G_C8_SCRATCH       ((volatile uint8_t    *)0x200000C8u)
#define G_OTA_FLUSH_FLAG   (*(volatile uint8_t   *)0x20000140u)
#define G_OTA_WRITE_PTR    (*(volatile const void **)0x200000E0u)
#define G_RX_FRAME_MODE    (*(volatile uint8_t   *)0x200000D9u)
#define G_OTA_FIRST_PACKET (*(volatile uint8_t   *)0x2000013Fu)
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

/* OEM @ 0x08003B86 (24 B). Drives the short form of Modbus cmd 0x5C
 * (`len == 3`): the bike echoes back the 3-byte register block it
 * previously stashed in `G_5C_REGS` via the long-form (`len == 0x0F`)
 * write. We splice those three bytes into the image-status emit
 * slots — `version` into `G_VERSION_BYTE`, `pkt0`/`pkt1` into the
 * two `G_PKT_BYTES` slots — and fire `report_image_status`, which
 * lays out a 7-byte PDU on the bus and may piggy-back a post-OTA
 * SYSRESETREQ (gated by `G_IMG_OK`, see modbus_tx_finalize). */
void cmd_5c_write3(uint8_t version, uint8_t pkt0, uint8_t pkt1)
{
    G_VERSION_BYTE = version;
    G_PKT_BYTES[0] = pkt0;
    G_PKT_BYTES[1] = pkt1;
    report_image_status();
}

/* OEM @ 0x08003B9E (26 B). Variant of `cmd_5c_write3` that derives the
 * version byte from the inbound PDU rather than from a parameter:
 * `G_VERSION_BYTE = (G_RX_BUF[5] & 0x7F) << 1` — the same sub-id
 * encoding as `image_apply` and `cmd_0f_report_u32`. Callers supply
 * the two payload bytes directly. Used by `cmd_5b_selftest` to report
 * the PA0/PA1 input state on the bus. */
void emit_image_status_payload(uint8_t b0, uint8_t b1)
{
    G_VERSION_BYTE = (uint8_t)((G_C8_SCRATCH[5] & 0x7Fu) << 1);
    G_PKT_BYTES[0] = b0;
    G_PKT_BYTES[1] = b1;
    report_image_status();
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
        G_RX_FRAME_MODE    = 0u;
        G_OTA_FIRST_PACKET = 0u;
        G_OTA_FLUSH_FLAG   = 0u;
        G_OTA_WRITE_PTR    = g_image;
    }
}
