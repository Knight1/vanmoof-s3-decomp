/* secrets.c — external-SPI-flash secrets store.
 *
 * VanMoof keeps 128 CRC-protected 32-byte records in the 4 KB sector
 * at external-flash offset `0x005A000`. Each record is laid out as:
 *
 *     record[0..27]   payload (28 bytes)
 *     record[28..31]  CRC-32/zlib of payload  (LE storage)
 *
 * Known slot assignments (cross-checked with the `VanMooof-Module`
 * Go tool's `ReadSecrets` helper):
 *
 *     slot 0    BLE Authentication Key  (16 B in payload, +12 B pad)
 *     slot 124  M-ID/M-KEY part 1       (raw 60 B spans slots 124+125,
 *     slot 125  M-ID/M-KEY part 2        does NOT use record-CRC format)
 *     slot 126  Manufacturing Key       (16 B in payload, +12 B pad)
 *
 * NB: M-ID/M-KEY is read/written *outside* this record API — the 60 B
 * are not CRC-wrapped per record. Some other path in the firmware
 * touches those two slots as raw bytes.
 *
 * OEM functions:
 *   secrets_record_read           @ 0x00020BB8  (90 B body)
 *   secrets_record_write_verify   @ 0x00020C06  (~70 B body)
 *
 * The base address `0x005A000` is encoded as a Thumb-2 modified-
 * immediate (`add.w rD, rS, #0x5A000`), not as a 32-bit literal, so it
 * doesn't show up in a naive byte-pattern search of the binary.
 */

#include <stdint.h>
#include <stddef.h>

#define SECRETS_SECTOR_BASE   0x0005A000u
#define SECRETS_RECORD_BYTES  0x20u           /* 32 B per record */
#define SECRETS_RECORD_COUNT  128u            /* 4 KB / 32 B    */
#define SECRETS_PAYLOAD_BYTES 0x1Cu           /* 28 B payload, then 4 B CRC */

extern int    extflash_read(uint32_t addr, uint32_t len, void *buf);
extern int    extflash_sector_write(uint32_t addr, uint32_t len,
                                    const void *buf);
extern uint32_t crc32_le(uint32_t seed, const uint8_t *buf, int len);
extern int    memcmp(const void *a, const void *b, unsigned int n);
extern void  *memcpy(void *dst, const void *src, unsigned int n);
extern void  *memset(void *dst, int c, unsigned int n);

/* Read record `index` from the secrets sector. If the payload's CRC-32
 * matches the stored CRC at offset 0x1C, and `out_record` is non-NULL,
 * copy the full 32-byte record into the caller's buffer. Returns 1 on
 * a CRC-valid read, 0 otherwise (including out-of-range index).
 *
 * OEM @ 0x00020BB8 — bounds-checks index against [0, 127]. */
int secrets_record_read(int index, void *out_record)
{
    if (index < 0 || index > 0x7F) {
        return 0;
    }

    uint8_t buf[SECRETS_RECORD_BYTES];
    uint32_t addr = (uint32_t)index * SECRETS_RECORD_BYTES
                  + SECRETS_SECTOR_BASE;

    extflash_read(addr, SECRETS_RECORD_BYTES, buf);

    uint32_t crc      = crc32_le(0xFFFFFFFFu, buf, SECRETS_PAYLOAD_BYTES);
    uint32_t stored;
    memcpy(&stored, buf + SECRETS_PAYLOAD_BYTES, sizeof stored);

    int valid = (crc == stored);
    if (valid && out_record != NULL) {
        memcpy(out_record, buf, SECRETS_RECORD_BYTES);
    }
    return valid;
}

/* Write the 32-byte `record` to slot `index`, then read back and
 * memcmp-verify. Retries up to 4 times on mismatch. Returns 1 on
 * verified write, 0 if all retries failed.
 *
 * OEM @ 0x00020C06 — note that, unlike the read path, the write does
 * NOT bounds-check `index`. A negative or out-of-range index will
 * touch flash bytes outside the secrets sector. Preserved as-is to
 * mirror the OEM image; do not "fix" this without confirming there
 * are no callers relying on the elided check. */
int secrets_record_write_verify(int index, const void *record)
{
    uint8_t verify[SECRETS_RECORD_BYTES];
    uint32_t addr = (uint32_t)index * SECRETS_RECORD_BYTES
                  + SECRETS_SECTOR_BASE;
    int retry = 4;
    int done;

    do {
        extflash_sector_write(addr, SECRETS_RECORD_BYTES, record);
        extflash_read(addr, SECRETS_RECORD_BYTES, verify);

        int cmp = memcmp(record, verify, SECRETS_RECORD_BYTES);
        done = (cmp == 0);
        if (!done) {
            retry--;
            done = (retry == 0);
        }
    } while (!done);

    /* OEM does this final memcmp again instead of caching the loop's
     * last result — preserved verbatim. */
    return memcmp(record, verify, SECRETS_RECORD_BYTES) == 0;
}

/* ---------------------------------------------------------------------
 * Keyed-record API
 * ---------------------------------------------------------------------
 *
 * Records in slots 0..123 (0x7B) are addressed by a 32-bit key stored at
 * payload offset +16 and tagged with a 4-character marker at payload
 * offset +24. Slots 124..127 are reserved for directory / metadata
 * records (slot 124 is the M-ID record — see secrets_ensure_mid_record).
 *
 * Two tag values appear in the OEM image:
 *   "M-ID"  — slot 124, set by secrets_ensure_mid_record
 *   "UKEY"  — user-provided keyed records, written by
 *             secrets_upsert_keyed_record
 *
 * Other code paths likely write additional tag values to other slots,
 * but those haven't surfaced yet in this decomp pass.
 */

#define SECRETS_KEYED_SLOT_LIMIT 0x7Cu          /* slots 0..123 are user-keyed */
#define SECRETS_KEY_OFFSET       0x10u          /* uint32 key  at payload+16 */
#define SECRETS_TAG_OFFSET       0x18u          /* uint32 tag  at payload+24 */
#define SECRETS_TAG_MID          0x44492D4DUL   /* little-endian "M-ID" */
#define SECRETS_TAG_UKEY         0x59454B55UL   /* little-endian "UKEY" */

/* Linear-scan slots [0, 123] for a record whose payload+16 word equals
 * `key`. On match, if `out_record` is non-NULL, the full 32-byte record
 * is copied into it. Returns the slot index, or -1 if no match.
 *
 * Records with bad CRCs are skipped (secrets_record_read returns 0).
 *
 * OEM @ 0x00022BAA. */
int secrets_find_by_key(uint32_t key, void *out_record)
{
    uint8_t buf[SECRETS_RECORD_BYTES];

    for (int index = 0; index < (int)SECRETS_KEYED_SLOT_LIMIT; index++) {
        if (secrets_record_read(index, buf) == 1) {
            uint32_t slot_key;
            memcpy(&slot_key, buf + SECRETS_KEY_OFFSET, sizeof slot_key);
            if (slot_key == key) {
                if (out_record != NULL) {
                    memcpy(out_record, buf, SECRETS_RECORD_BYTES);
                }
                return index;
            }
        }
    }
    return -1;
}

/* Count slots in [0, 123] whose stored CRC validates — the population
 * of intact UKEY records on the device. Used by `auth_derive_session_key`
 * as the "device is provisioned" gate: a device with zero valid keyed
 * records (and no mfg key) falls back to the synthesized OWNER_PERMS
 * default record.
 *
 * Note that this scans the same slot range as `secrets_count_free_slots`
 * but counts the *valid* slots, not the invalid ones — and that range
 * extends to slot 123 inclusive (`index < 0x7C`), matching the upper
 * limit of the keyed-record region.
 *
 * OEM @ 0x00025680. */
int secrets_count_valid_in_keys_range(void)
{
    int valid_count = 0;
    for (int index = 0; index < (int)SECRETS_KEYED_SLOT_LIMIT; index++) {
        if (secrets_record_read(index, NULL) == 1) {
            valid_count++;
        }
    }
    return valid_count;
}

/* Count slots in [0, 123] whose stored CRC does NOT validate. These are
 * the "free" slots that an upsert may claim for a new record.
 *
 * OEM @ 0x00026034. */
int secrets_count_free_slots(void)
{
    int free_count = 0;
    for (int index = 0; index < (int)SECRETS_KEYED_SLOT_LIMIT; index++) {
        if (secrets_record_read(index, NULL) == 0) {
            free_count++;
        }
    }
    return free_count;
}

/* Upsert a keyed record. `record` is a caller-provided 24-byte payload
 * (bytes 0..23 of the record's payload area); bytes [16..19] are the
 * record's key. This function tags the record as "UKEY" at payload+24,
 * computes the CRC, and writes it to flash:
 *
 *   1. Search for an existing slot with this key  → overwrite there
 *   2. Otherwise find the first slot with an invalid CRC  → write there
 *   3. If neither succeeds (table full of valid non-matching records),
 *      return 0.
 *
 * The OEM searches for a free slot using a forward scan; on the very
 * first failed-CRC slot it jumps to the write block. The decompile keeps
 * the slightly unusual `if (iVar1 < 0)` pre-decrement test from the OEM.
 *
 * Returns the result of secrets_record_write_verify on success, or 0 if
 * no slot was available.
 *
 * OEM @ 0x0001CA68. */
int secrets_upsert_keyed_record(const void *record_24)
{
    const uint8_t *src = (const uint8_t *)record_24;
    uint32_t key;
    memcpy(&key, src + SECRETS_KEY_OFFSET, sizeof key);

    int slot = secrets_find_by_key(key, NULL);
    if (slot < 0) {
        /* No match — find first free (CRC-invalid) slot. */
        for (slot = 0; slot < (int)SECRETS_KEYED_SLOT_LIMIT; slot++) {
            if (secrets_record_read(slot, NULL) == 0) {
                break;
            }
        }
        if (slot >= (int)SECRETS_KEYED_SLOT_LIMIT) {
            return 0;
        }
    }

    uint8_t buf[SECRETS_RECORD_BYTES];
    memcpy(buf, src, 0x18);                                /* 24 B payload from caller */
    /* OEM stores the 4-byte tag word verbatim from a pool entry. */
    uint32_t tag = SECRETS_TAG_UKEY;
    memcpy(buf + SECRETS_TAG_OFFSET, &tag, sizeof tag);
    uint32_t crc = crc32_le(0xFFFFFFFFu, buf, SECRETS_PAYLOAD_BYTES);
    memcpy(buf + SECRETS_PAYLOAD_BYTES, &crc, sizeof crc);

    return secrets_record_write_verify(slot, buf);
}

/* Upsert a batch of `count` keyed records. Each record in the array is
 * 32 bytes wide (the OEM steps the input pointer by 0x20 between
 * records — only the first 24 B of each are used by the upsert).
 *
 * The function first counts how many of the requested keys are
 * already-present (would overwrite an existing slot) and how many free
 * slots exist. If `(matches + free) < count`, returns -1 ("no room").
 * Otherwise upserts each record in turn, returning -2 if any single
 * upsert fails. On full success returns 0.
 *
 * OEM @ 0x0001F0BE. */
int secrets_upsert_keyed_batch(const void *records, unsigned int count)
{
    const uint8_t *p = (const uint8_t *)records;
    int matches = 0;

    for (unsigned int i = 0; i < count; i++) {
        uint32_t key;
        memcpy(&key, p + i * SECRETS_RECORD_BYTES + SECRETS_KEY_OFFSET,
               sizeof key);
        if (secrets_find_by_key(key, NULL) >= 0) {
            matches++;
        }
    }

    int free_slots = secrets_count_free_slots();
    if ((unsigned int)(matches + free_slots) < count) {
        return -1;
    }

    for (unsigned int i = 0; i < count; i++) {
        if (secrets_upsert_keyed_record(p + i * SECRETS_RECORD_BYTES) == 0) {
            return -2;
        }
    }
    return 0;
}

/* Ensure slot 124 — the "M-ID" directory record — exists with a valid
 * CRC. If it does, return the 32-bit word stored at payload offset +16
 * (its meaning is still unknown — likely an M-ID counter / cursor).
 * If the slot's CRC fails, zero-initialise a fresh record, stamp the
 * "M-ID" tag at payload offset +24, recompute the CRC, write it, and
 * return 0.
 *
 * OEM @ 0x00021328. The function unconditionally returns a uint32, but
 * the success-path return is loaded from the just-read buffer at +16
 * while the init-path return is the literal 0. */
uint32_t secrets_ensure_mid_record(void)
{
    uint8_t buf[SECRETS_RECORD_BYTES];

    if (secrets_record_read(0x7C, buf) == 1) {
        uint32_t field16;
        memcpy(&field16, buf + SECRETS_KEY_OFFSET, sizeof field16);
        return field16;
    }

    /* Slot 124 is missing or corrupt — initialise it. */
    memset(buf, 0, SECRETS_RECORD_BYTES);
    uint32_t tag = SECRETS_TAG_MID;
    memcpy(buf + SECRETS_TAG_OFFSET, &tag, sizeof tag);
    uint32_t crc = crc32_le(0xFFFFFFFFu, buf, SECRETS_PAYLOAD_BYTES);
    memcpy(buf + SECRETS_PAYLOAD_BYTES, &crc, sizeof crc);
    secrets_record_write_verify(0x7C, buf);
    return 0;
}
