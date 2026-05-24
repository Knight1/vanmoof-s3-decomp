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

/* ROM thunk for Semaphore_pend / Semaphore_post. */
extern int  ti_semaphore_pend(uint32_t handle, uint32_t timeout);
extern void ti_semaphore_post(uint32_t handle);

/* Connection-lookup helper — returns a pointer to the per-connection
 * CCCD storage area, or NULL. OEM FUN_00025A24. */
extern void *FUN_00025A24(uint16_t conn);

/* Forward declares. */
uint16_t cccd_read(uint16_t conn);
void     cccd_write_store(uint16_t conn, uint16_t cccd);

/* Validate a CCCD write value. Only bits 0 (notification) and 1
 * (indication) are supported. Any other set bit returns 0x80.
 * If the value has changed from the stored state, commits it via
 * cccd_write_store. OEM @ 0x0001F9AE (60 B). */
int cccd_write_validate(uint16_t conn, const uint8_t *value, uint16_t len)
{
    if (len < 2) return 0x80;

    uint16_t new_cccd = *(const uint16_t *)value;

    /* reject unsupported bits */
    if (new_cccd & ~3u) {
        return 0x80;
    }

    uint16_t stored = (uint16_t)cccd_read(conn);
    if (new_cccd != stored) {
        cccd_write_store(conn, new_cccd);
    }
    return 0;
}

/* Store a CCCD value for a connection. The OEM acquires the per-record
 * semaphore, stores the 2-byte CCCD, and posts. OEM @ 0x00024D90 (40 B). */
void cccd_write_store(uint16_t conn, uint16_t cccd)
{
    void *rec = FUN_00025A24(conn);
    if (rec == NULL) return;

    *(uint16_t *)((uint8_t *)rec + 0) = conn;
    *(uint16_t *)((uint8_t *)rec + 2) = cccd;
}

uint16_t cccd_read(uint16_t conn)
{
    void *rec = FUN_00025A24(conn);
    if (rec == NULL) return 0;
    return *(uint16_t *)((uint8_t *)rec + 2);
}
