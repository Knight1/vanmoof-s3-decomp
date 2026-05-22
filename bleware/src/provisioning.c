/* provisioning.c — backoffice provisioning-blob handler.
 *
 * OEM source: `source/xs3_gatt_backoffice.c` (filename embedded at
 * flash `0x00004140`). This module implements VanMoof's GATT
 * "backoffice" service — the channel their service tools / factory
 * tooling use to push keys and configuration into the device.
 *
 * Two functions are decoded here:
 *
 *   manufacturing_key_get_or_init_default()  @ OEM 0x00019570
 *       Fetches slot 126 ("M-Key") from the external-SPI-flash secrets
 *       store. If the slot's CRC is invalid (factory-unprovisioned
 *       device), builds a *deterministic per-device fallback*
 *       in-RAM key:  the chip's BLE MAC, formatted as 12 ASCII hex
 *       chars (high byte first), followed by the tag "MOOF" at +12
 *       and "MKEY" at +24, with a CRC over the 28-byte payload.
 *       The fallback is NOT written to flash — it just stays in the
 *       cached buffer for the current boot.
 *
 *   secrets_provisioning_blob_apply()        @ OEM 0x00003E78
 *       Top-level handler for the provisioning characteristic. The
 *       service-tool writes an encrypted blob in 16-byte AES-block
 *       multiples; each block is submitted, along with the M-Key, to
 *       a TI-RTOS worker task that invokes CryptoCC26X2 ROM AES
 *       (function-pointer call through `_DAT_100001FC + 0x20`). When
 *       the decryption finishes, the resulting "UKEY" records are
 *       bulk-upserted into the secrets store via
 *       `secrets_upsert_keyed_batch`, slot 124's M-ID record is
 *       ensured via `secrets_ensure_mid_record`, and a 6-byte BLE
 *       device address is composed from the M-ID's payload+16 word
 *       plus a stack-supplied counter/cursor value.
 *
 * This file currently decodes only the linear flow (validate-loop-
 * upsert-ensure). The OEM function contains a `tbh`-dispatched
 * sub-command table with several error paths and an alternate
 * single-record write path; those are TBD and will be folded in as
 * separate functions once the GATT-service registration is decoded
 * and we know which sub-commands the service-tool actually issues.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "bleware.h"

/* Tags written into the M-Key record's payload at fixed offsets.
 * Both appear as 4-byte literals at flash 0x000195E8 / 0x000195F0. */
#define MKEY_TAG_MOOF  0x464F4F4DUL   /* LE "MOOF" */
#define MKEY_TAG_MKEY  0x59454B4DUL   /* LE "MKEY" */

/* CC2642R1F FCFG1 — BLE MAC address (6 bytes, byte-reversed in flash).
 * The OEM reads each byte separately, high index first, so we mirror. */
#define FCFG1_MAC_BLE_BASE 0x500012E8u

/* RAM working buffer for the cached M-Key record. The OEM stores the
 * pointer at flash literal-pool entry `DAT_000195FC = 0x2000A3DC`.
 * Layout (relative to the returned buffer pointer):
 *
 *     working[+0x00..+0x1F]  the live record buffer (mirrors flash slot 126)
 *
 * The OEM struct around it also holds extra bookkeeping fields at
 * working[-0x20..-0x01] (working buffer is at +0x20 from the struct
 * base). We model just the record-buffer part. */
extern uint8_t  *g_mkey_working_buffer;       /* &record[0..31] in RAM */
extern uint32_t  g_mkey_owning_struct_field30; /* the "+0x30" slot — flags */
extern uint32_t  g_mkey_owning_struct_field34; /* "+0x34" — set to 0xFFFFFFFF */
extern uint32_t  g_mkey_owning_struct_field3C; /* "+0x3C" — CRC mirror */

/* TI-RTOS message-queue post for the (key, block) tuple sent to the
 * CryptoCC26X2 worker task. Real implementation lives in
 * `block_dispatch_queue_post` (OEM 0x00020848). */
extern int  block_dispatch_queue_post(const void *key_record,
                                      uint32_t   block_len,
                                      const void *block,
                                      const void *block_alias);

/* Helpers used inside the file. */
extern uint32_t crc32_le(uint32_t seed, const uint8_t *buf, int len);
extern int      secrets_record_read(int index, void *out_record);
extern int      secrets_upsert_keyed_batch(const void *records, unsigned int count);
extern uint32_t secrets_ensure_mid_record(void);

/* Hex digit table; OEM lives at flash 0x0002B46C and is read by
 * `byte_to_hex_chars(dst, byte_value)`. */
static const char g_hex_digits[] = "0123456789ABCDEF";

/* Encode `b` as two ASCII hex characters at `dst[0..1]`.
 * OEM `byte_to_hex_chars` @ 0x00026504. */
static inline void byte_to_hex_chars(uint8_t *dst, unsigned int b)
{
    dst[0] = (uint8_t)g_hex_digits[(b >> 4) & 0xF];
    dst[1] = (uint8_t)g_hex_digits[b & 0xF];
}

/* Slot 126 — Manufacturing Key.
 *
 * Returns a pointer to the 32-byte working buffer that holds the
 * effective M-Key record. On a factory-unprovisioned device the slot's
 * CRC fails; in that case a deterministic fallback derived from the
 * chip's BLE MAC is materialised in RAM (not written back to flash).
 *
 * OEM @ 0x00019570. */
uint8_t *manufacturing_key_get_or_init_default(void)
{
    uint8_t *buf = g_mkey_working_buffer;   /* = owning_struct + 0x20 */

    if (secrets_record_read(0x7E, buf) == 1) {
        return buf;
    }

    /* Slot 126 unreadable — synthesise the per-device fallback key:
     *   buf[0..11]  = ASCII hex of BLE MAC, byte 5..byte 0 (i.e. the
     *                 MAC printed in standard big-endian display order)
     *   buf[12..15] = "MOOF"
     *   buf[16..23] = (left untouched — assumed zero-initialised)
     *   buf[24..27] = "MKEY"
     *   buf[28..31] = CRC-32 over buf[0..27]
     */
    const volatile uint8_t *mac = (const volatile uint8_t *)FCFG1_MAC_BLE_BASE;
    byte_to_hex_chars(buf + 0x00, mac[5]);
    byte_to_hex_chars(buf + 0x02, mac[4]);
    byte_to_hex_chars(buf + 0x04, mac[3]);
    byte_to_hex_chars(buf + 0x06, mac[2]);
    byte_to_hex_chars(buf + 0x08, mac[1]);
    byte_to_hex_chars(buf + 0x0A, mac[0]);

    uint32_t tag_moof = MKEY_TAG_MOOF;
    memcpy(buf + 0x0C, &tag_moof, sizeof tag_moof);

    g_mkey_owning_struct_field30 = 0;
    g_mkey_owning_struct_field34 = 0xFFFFFFFFu;

    uint32_t tag_mkey = MKEY_TAG_MKEY;
    memcpy(buf + 0x18, &tag_mkey, sizeof tag_mkey);

    g_mkey_owning_struct_field3C = crc32_le(0xFFFFFFFFu, buf, 0x1C);
    return buf;
}

/* ---------------------------------------------------------------------
 * Provisioning-blob apply — linear-flow portion
 *
 * The full OEM function at 0x00003E78 is a multi-mode dispatcher
 * controlled by a sub-command byte in the input buffer (`tbh [pc, r0]`
 * jump table at OEM 0x00003F6C). We currently decode only one path —
 * the "bulk encrypted record import" path — because (a) it's the only
 * path that actually touches the M-Key + secrets store, and (b) it's
 * the only path with confirmed semantics. Other tbh cases (sub-commands
 * 0/4/5/8/9/a) handle status-reply, single-record write, etc., and are
 * still TBD.
 *
 * Inputs (from the GATT characteristic write):
 *   pkt[0]        sub-command (we handle the "decrypt-bulk" case only)
 *   pkt[1..6]     header fields used for the BLE-address rebuild and
 *                 case dispatch (TBD)
 *   pkt[7..]      ciphertext payload: N × 16-byte AES blocks
 *   payload_len   total bytes from offset 7 onward; must be a multiple
 *                 of 16 (validated against `0x0F`)
 *
 * On success:
 *   - Each 16-byte ciphertext block is paired with the M-Key and posted
 *     to the CryptoCC26X2 worker queue. The worker decrypts in-place
 *     (the OEM passes the same source pointer twice: src and dst alias,
 *     hence the `block_dispatch_queue_post(key, 16, src, src)` shape).
 *   - The contiguous decrypted region is then bulk-upserted into the
 *     secrets store as `count` 32-byte records.
 *   - Slot 124 (the M-ID directory) is reaffirmed.
 *   - A 6-byte BLE address is composed from the M-ID and a stack value
 *     (probably a roll counter) and written into a caller-provided
 *     output slot.
 *
 * Status-code byte at the end (`status_out`):
 *   0  success
 *   5  upsert returned -2 ("any write failed")
 *   7  upsert returned -1 ("not enough room")
 *   other (4/6/8/9/A) — TBD sub-command-specific
 */
int secrets_provisioning_apply_bulk(const uint8_t *pkt,
                                    uint8_t       *ble_addr_out,
                                    uint32_t       payload_len)
{
    if ((payload_len & 0x0Fu) != 0 || payload_len == 0) {
        /* OEM: monitor_log("source/xs3_gatt_backoffice.c", 0x66,
         *      "Invalid message length <%d>, should be a multiply of 16",
         *      payload_len);
         */
        return -1;
    }

    const unsigned int block_count = payload_len >> 4;

    /* Allocate the decryption scratch (OEM via FUN_000276AE — likely
     * `monitor_alloc`). Size: block_count * 32 bytes (each input AES
     * block expands to a 32-byte secrets record). */
    void *scratch = monitor_alloc(block_count * 32u);
    if (scratch == NULL) {
        return -1;
    }

    /* Decrypt each 16-byte AES block. The OEM iterates the block
     * index, fetches the M-Key per iteration (cheap: just an in-RAM
     * read after the first), and posts (key, 16, src+2, src+2) to the
     * CryptoCC26X2 worker queue. The +2 skip is part of the packet
     * framing: the encrypted payload doesn't start at the block
     * boundary, it starts two bytes in. */
    for (unsigned int i = 0; i < block_count; i++) {
        const uint8_t *src = pkt + (i << 4) + 2;
        uint8_t       *key = manufacturing_key_get_or_init_default();
        block_dispatch_queue_post(key, 16, src, src);
    }

    int rc = secrets_upsert_keyed_batch(scratch, block_count);

    /* Translate the upsert return code to the OEM status enum. */
    int status;
    if (rc == 0)       status = 0;          /* success */
    else if (rc == -1) status = 7;          /* "no room" */
    else               status = 5;          /* "any write failed" */

    /* OEM @ 0x00004066 calls a cleanup helper (FUN_00021B88) before
     * returning. Likely a `monitor_free(scratch)` analogue. */
    monitor_free(scratch);

    if (status != 0) {
        return status;
    }

    /* Rebuild the device's 6-byte BLE address from the M-ID record
     * and a roll counter the caller stashed at [sp+0x34]. The OEM
     * packs three bytes of the M-ID word (>>24, >>18, >>8, >>0) and
     * two bytes of the counter (>>24, >>18). Order matches the BD_ADDR
     * layout used by the BLE stack's `GAP_DeviceInit` call. */
    uint32_t mid     = secrets_ensure_mid_record();
    uint32_t counter = *(const uint32_t *)(pkt + 0x34 /* TBD: caller-supplied */);

    ble_addr_out[0] = (uint8_t)(mid     >> 24);
    ble_addr_out[1] = (uint8_t)(mid     >> 18);    /* sic — OEM uses >>18, not >>16 */
    ble_addr_out[2] = (uint8_t)(mid     >>  8);
    ble_addr_out[3] = (uint8_t)(mid           );
    ble_addr_out[4] = (uint8_t)(counter >> 24);
    ble_addr_out[5] = (uint8_t)(counter >> 18);

    return 0;
}
