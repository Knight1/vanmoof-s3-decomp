/* gatt_svc_5560.c — per-service TI BLE-stack callback shims for
 *                   service `0x5560`.
 *
 * The 11-service registry doesn't get called directly by the TI
 * stack — instead each service registers a `gattServiceCBs_t` with
 * `GATTServApp_RegisterService`. For svc `0x5560` that struct lives
 * in flash at `0x0002A124` (immediately after the monitor command
 * table's NULL terminator), with:
 *
 *   pfnReadAttrCB  = svc_5560_read_attr_cb  @ 0x0001E310
 *   pfnWriteAttrCB = svc_5560_write_attr_cb @ 0x00019A80
 *   pfnAuthorizeCB = NULL
 *
 * Both shims do the same thing: extract the char-table index for the
 * attribute the TI stack is asking about, then forward to the
 * appropriate central dispatcher in `xs3_gatt_write.c` /
 * `xs3_gatt_read.c`. The central dispatchers receive the svc UUID
 * (hardcoded `0x5560`) and the resolved char index so they can route
 * by the same `svc + idx + 1` Modbus-command convention used
 * elsewhere.
 *
 * **Architectural note.** All 10 services (every UUID except the
 * special-cased backoffice 0x5500) have an isomorphic shim pair
 * — 140 B write, 94 B read, identical control flow. They differ in
 * three things: the hardcoded svc UUID, the per-service char-UUID-
 * to-index function, and the vtable pointer they bind to. Sampled
 * vtables: svc 0x5510/0x5520/0x55A0 share `0x20005188`; svc 0x5560
 * uses `0x20003F84`. At least two distinct vtables exist; this file
 * documents svc 0x5560's binding.
 *
 * The dispatcher pointers live in a 3-slot vtable struct (this
 * file's binding is the one at RAM `0x20003F84`, accessed via OEM
 * `*DAT_0001E370 == *DAT_00019B0C`):
 *
 *   +0x00 normal-char write dispatcher (xs3_gatt_process_write_event)
 *   +0x04 CCCD write dispatcher       (cccd_write_validate wrapper)
 *   +0x08 read dispatcher              (xs3_gatt_process_read_event)
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "bleware.h"

/* TI BLE-stack `gattAttribute_t` (the param_2 in both shims):
 *   +0x00 u8       type-tag (0x02 = CCCD descriptor, 0x10 = char value)
 *   +0x01..+0x03   pad / handle
 *   +0x04 uint8_t *uuid          (16-byte UUID pointer)
 *   +0x06 (re-aliased as) uint32_t *backing_storage_ptr
 *                  — for char-value attrs, this is a pointer-to-pointer
 *                  to the RAM shadow that the read dispatcher copies
 *                  out of when no producer overrides the value.
 *   +0x0C uint32_t *cccd_slot    (set on CCCD-descriptor attrs only)
 */

/* The central-dispatch vtable at RAM 0x20003F84. */
struct gatt_dispatch_vtable {
    int (*write_normal)(void *attr, uint32_t conn, uint16_t svc,
                        uint8_t idx, uint8_t *value, uint16_t len,
                        uint16_t offset);
    int (*write_cccd)(void *attr, uint32_t conn, uint16_t svc,
                      uint8_t idx, uint8_t *value, uint16_t len,
                      uint16_t offset);
    int (*read_attr)(void *attr, uint32_t conn, uint16_t svc,
                     uint8_t idx, uint32_t value_storage,
                     uint8_t *out_buf, uint8_t op_byte,
                     uint16_t *out_len);
};

extern struct gatt_dispatch_vtable *g_gatt_dispatch;   /* *DAT_0001E370 */

/* Per-service helpers. */
extern uint8_t svc_5560_char_uuid_to_index(uint8_t *attr);   /* 0x00012FA8 */
extern int     cccd_write_validate(uint32_t conn, void *attr,
                                   const uint8_t *value, uint16_t len,
                                   uint16_t offset,
                                   uint16_t supported_mask);  /* 0x0001F9AE */

/* TI BLE-stack op-codes we care about. */
#define ATT_OP_WRITE_REQ 0x02
#define ATT_TYPE_CCCD    0x02   /* attr.type-tag for CCCD descriptor */
#define UUID16_CCCD      0x2902 /* Client Characteristic Configuration */

/* Read callback. Called by the TI stack with:
 *   conn        — connection handle
 *   attr        — `gattAttribute_t *`
 *   out_buf     — caller-owned destination for the read value
 *   out_len     — in: max length the caller can accept; out: length actually written
 *   offset      — read offset within the attribute
 *   max_len     — MTU-derived ceiling
 *   op_byte     — ATT opcode (0x0A = ReadReq, 0x0B = ReadBlobReq, etc.)
 *
 * OEM @ 0x0001E310.
 */
uint8_t svc_5560_read_attr_cb(uint32_t  conn,
                              void     *attr,
                              uint8_t  *out_buf,
                              uint16_t *out_len,
                              uint16_t  offset,
                              uint16_t  max_len,
                              uint8_t   op_byte)
{
    uint8_t idx = svc_5560_char_uuid_to_index((uint8_t *)attr);
    uint8_t rc  = 1;

    if (g_gatt_dispatch->read_attr != NULL && idx != 0xFF) {
        /* Pull the attribute's backing-storage pointer (at attr+0x06)
         * and pass it along with the requested read-offset. The
         * central read dispatcher uses it as the "Modbus shadow"
         * source for chars that don't have a bespoke producer. */
        uint32_t shadow_ptr = *(uint32_t *)((uint8_t *)attr + 6) + offset;

        rc = g_gatt_dispatch->read_attr(attr, conn, 0x5560, idx,
                                        shadow_ptr, out_buf, op_byte,
                                        out_len);
        if (rc == 0) {
            /* On success the dispatcher writes the canonical value
             * into the shadow at `shadow_ptr`; copy it back into
             * `out_buf` for the TI stack. */
            memcpy(out_buf, (const void *)shadow_ptr, *out_len);
        }
    }
    return rc;
}

/* Write callback. CCCD writes (UUID 0x2902, identified by the
 * attribute's type-tag byte being 0x02) are routed to the CCCD
 * dispatcher; everything else hits the normal-char dispatcher.
 *
 * On CCCD success, also calls `cccd_write_validate(..., 3)` — the
 * `3` is the "supported notify+indicate mask" for whichever char's
 * CCCD this is. The TI stack uses this to reject CCCDs that try to
 * enable notifications on a char that only supports indications (or
 * vice versa).
 *
 * OEM @ 0x00019A80.
 */
uint8_t svc_5560_write_attr_cb(uint32_t  conn,
                               void     *attr,
                               uint8_t  *value,
                               uint16_t  len,
                               uint16_t  offset)
{
    uint8_t                       *attr_bytes = (uint8_t *)attr;
    struct gatt_dispatch_vtable   *vt         = g_gatt_dispatch;

    if (attr_bytes[0] == ATT_TYPE_CCCD) {
        uint16_t descriptor_uuid = *(uint16_t *)(attr_bytes + 4);
        if (descriptor_uuid == UUID16_CCCD) {
            uint8_t idx = svc_5560_char_uuid_to_index(attr_bytes);
            uint8_t rc  = vt->write_cccd(attr, conn, 0x5560, idx,
                                         value, len, offset);
            if (rc == 0) {
                return (uint8_t)cccd_write_validate(conn, attr, value, len,
                                                    offset, 3);
            }
            return rc;
        }
    }

    /* Normal-characteristic write path. */
    uint8_t idx = svc_5560_char_uuid_to_index(attr_bytes);
    if (idx == 0xFF) {
        return 0x0A;   /* ATT error: invalid handle */
    }
    if (vt != NULL && vt->write_normal != NULL) {
        vt->write_normal(attr, conn, 0x5560, idx, value,
                         (uint16_t)((uint32_t)len + offset));
    }
    return 0;
}
