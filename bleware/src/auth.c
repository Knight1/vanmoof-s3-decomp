/* auth.c — session-key derivation and the manufacturing-ECB helper.
 *
 * Functions decoded:
 *
 *   `auth_derive_session_key`        @ 0x00018B1C
 *   `mfg_key_ecb_decrypt_chunks`     @ 0x00024740
 *
 * `auth_derive_session_key` resolves the 32-byte session-key record
 * for a given client key id. If a record with the matching key id
 * exists in the on-flash secrets store, it's returned verbatim (so
 * the caller can compare against the in-memory session key it just
 * derived). If no record matches AND the device is in an "untrusted"
 * state — no manufacturing key, no other valid key records in the
 * keys region — the helper synthesises the default OWNER_PERMS
 * record on the fly. Otherwise it fails (returns NULL).
 *
 * Default OWNER_PERMS record (32 bytes):
 *
 *   +0x00  16 B  "_____OWNER_PERMS"        (key payload)
 *   +0x10   4 B  0x00000000                 (flags)
 *   +0x14   4 B  0xFFFFFFFF                 (permission mask — all)
 *   +0x18   4 B  "UKEY" (0x59454B55 LE)     (record-type tag)
 *   +0x1C   4 B  CRC-32 of bytes 0..0x1B   (zlib polynomial)
 *
 * `mfg_key_ecb_decrypt_chunks` is the bulk ECB-decrypt helper used by
 * the GATT-write central dispatcher: splits the payload into 16-byte
 * blocks and pushes each through `block_dispatch_queue_post` with the
 * manufacturing key. No padding handling — the caller has already
 * rounded the length down to a multiple of 16.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bleware.h"

extern uint32_t crc32_le(uint32_t seed, const void *buf, uint32_t len);

extern void     block_dispatch_queue_post(const void *key, uint32_t kind,
                                          const void *src, void *dst, ...);

/* Count of valid (CRC-matching) records in the keys region [0..0x7B]
 * of the secrets store. Used by `auth_derive_session_key` to gate the
 * default-OWNER fallback. OEM @ 0x00025680. */
extern int secrets_count_valid_in_keys_range(void);

/* RAM scratch buffer (`g_mkey_working_buffer` in hal_stubs.S) — the
 * 32-byte session-key record is materialised here before being
 * returned to the caller. */
extern uint8_t g_mkey_working_buffer[32];   /* @ 0x2000A3DC */

/* The OEM keeps the OWNER_PERMS template string at flash 0x00018B93,
 * where byte 0 is 'F' (an unrelated leading character left over from
 * format-string deduplication) and bytes [1..16] are the 16-byte key
 * payload "_____OWNER_PERMS". */
static const char k_owner_perms_template[18] = "F_____OWNER_PERMS";

/* "UKEY" magic stored at +0x18 of the synthesised record (read as a
 * 32-bit LE word in the OEM literal pool at 0x00018BA8). */
#define UKEY_MAGIC  0x59454B55u

void *auth_derive_session_key(uint32_t client_key_id)
{
    uint8_t  record[32];
    int      rc;

    memset(record, 0, sizeof record);
    memset(g_mkey_working_buffer, 0, sizeof record);

    rc = secrets_find_by_key(client_key_id, record);
    if (rc < 0) {
        /* No matching record. Permit the synthesised default only on
         * a fully un-provisioned device (no mfg key, no key records). */
        int mfg_present = secrets_record_read(0x7E, NULL);
        if (mfg_present == 1 || secrets_count_valid_in_keys_range() != 0) {
            return NULL;
        }

        memcpy(record, &k_owner_perms_template[1], 16);
        record[0x10] = 0x00;
        record[0x11] = 0x00;
        record[0x12] = 0x00;
        record[0x13] = 0x00;
        record[0x14] = 0xFF;
        record[0x15] = 0xFF;
        record[0x16] = 0xFF;
        record[0x17] = 0xFF;
        record[0x18] = (uint8_t)(UKEY_MAGIC      );
        record[0x19] = (uint8_t)(UKEY_MAGIC >>  8);
        record[0x1A] = (uint8_t)(UKEY_MAGIC >> 16);
        record[0x1B] = (uint8_t)(UKEY_MAGIC >> 24);

        uint32_t crc = crc32_le(0xFFFFFFFFu, record, 28);
        record[0x1C] = (uint8_t)(crc      );
        record[0x1D] = (uint8_t)(crc >>  8);
        record[0x1E] = (uint8_t)(crc >> 16);
        record[0x1F] = (uint8_t)(crc >> 24);
    }

    memcpy(g_mkey_working_buffer, record, sizeof record);
    return g_mkey_working_buffer;
}

void mfg_key_ecb_decrypt_chunks(uint8_t *dst, const uint8_t *src,
                                 uint32_t total_len)
{
    uint8_t *key = manufacturing_key_get_or_init_default();
    if (key == NULL) {
        return;
    }
    for (uint32_t blocks = total_len >> 4; blocks != 0; blocks--) {
        block_dispatch_queue_post(key, 0x10, src, dst);
        dst += 16;
        src += 16;
    }
}

/* Post a crypto operation (AES block encrypt/decrypt) to the TI
 * CryptoCC26X2 ROM driver queue. Packages (key, key_len, src, dst)
 * and dispatches via the ROM jump table. OEM @ 0x00020848 (44 B). */
void block_dispatch_queue_post(const void *key, uint32_t kind,
                                const void *src, void *dst, ...)
{
    extern void FUN_000275E8(void);   /* ROM crypto dispatch */
    extern void FUN_000125C4(void);   /* ROM jump-table resolver */
    (void)key;
    (void)kind;
    (void)src;
    (void)dst;
    /* In the OEM: allocates a queue element, stores (key, len, src, dst),
     * posts to the CryptoCC26X2 ROM queue, waits for completion. */
}
