/* gatt_cccd.c — CCCD (Client Characteristic Configuration Descriptor)
 * read/write helpers for svc 0x5560 and other services.
 *
 * The CCCD is a 2-byte value per-connection that controls whether
 * notifications (bit 0) and indications (bit 1) are enabled. The
 * TI BLE stack routes CCCD writes through svc_*_write_attr_cb with
 * attr type-tag 0x02.
 *
 * OEM addresses:
 *   cccd_write_validate @ 0x0001F9AE  (~60 B)
 *   cccd_write_store    @ 0x00024D90  (~40 B)
 *   cccd_read           @ 0x00026D94  (~20 B)
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* Per-record lookup helper. Scans the CCCD record array (param_2) for an
 * entry whose 16-bit key at +0 matches `conn`; returns a pointer to that
 * record, or NULL if no match. Passing conn == 0xFFFF finds the first
 * free slot. OEM FUN_00025A24(conn, array_ptr). */
extern void *FUN_00025A24(uint16_t conn, void *array_ptr);

/* Forward declares. */
uint8_t cccd_read(uint16_t conn, void *array_ptr);
uint8_t cccd_write_store(uint16_t conn, void *array_ptr, uint8_t cccd);

/* Validate a CCCD write value. Takes the full TI write-callback argument
 * set: (conn, attr, value, len, offset, supported_mask). The supported
 * notify/indicate mask is supplied by the caller (it is per-characteristic),
 * NOT hardcoded. The record array used by the read/store helpers lives at
 * **(uint32_t **)((uint8_t *)attr + 0xc).
 *
 * OEM @ 0x0001F9AE (~80 B):
 *   - offset != 0           -> return 0x0b
 *   - len    != 2           -> return 0x0d (ATT_ERR_INVALID_ATTR_LEN)
 *   - value has bits outside supported_mask -> return 0x80
 *   - else, if changed, commit via cccd_write_store and return its status;
 *     if unchanged, return 0.
 */
int cccd_write_validate(uint32_t conn, void *attr, const uint8_t *value,
                        uint16_t len, uint16_t offset, uint16_t supported_mask)
{
    if (offset != 0) {
        return 0x0b;
    }
    if (len != 2) {
        return 0x0d;
    }

    uint16_t new_cccd = (uint16_t)((uint16_t)value[0] +
                                   (uint16_t)((uint16_t)value[1] << 8));

    /* reject bits outside the per-characteristic supported mask */
    if ((new_cccd & (uint16_t)~supported_mask) != 0) {
        return 0x80;
    }

    void *array_ptr = *(void **)(*(uint8_t **)((uint8_t *)attr + 0xc));
    uint16_t stored = cccd_read((uint16_t)conn, array_ptr);
    if (new_cccd != stored) {
        return cccd_write_store((uint16_t)conn, array_ptr, (uint8_t)new_cccd);
    }
    return 0;
}

/* Store a CCCD value into the per-connection record array. Looks up the
 * record for `conn`; if absent, allocates a free slot (lookup with 0xFFFF)
 * and writes `conn` as the 16-bit key at +0. The CCCD itself is a single
 * byte stored at +2 on both paths. Returns 0 on success, 0x11 if no free
 * slot was available. OEM @ 0x00024D90 (~40 B). */
uint8_t cccd_write_store(uint16_t conn, void *array_ptr, uint8_t cccd)
{
    uint8_t *rec = FUN_00025A24(conn, array_ptr);
    if (rec == NULL) {
        rec = FUN_00025A24(0xFFFF, array_ptr);
        if (rec == NULL) {
            return 0x11;
        }
        *(uint16_t *)(rec + 0) = conn;
    }
    rec[2] = cccd;
    return 0;
}

/* Read the stored CCCD byte for `conn`, or 0 if no record exists.
 * The CCCD is a single byte at record offset +2. OEM @ 0x00026D94 (~20 B). */
uint8_t cccd_read(uint16_t conn, void *array_ptr)
{
    uint8_t *rec = FUN_00025A24(conn, array_ptr);
    if (rec == NULL) {
        return 0;
    }
    return rec[2];
}
