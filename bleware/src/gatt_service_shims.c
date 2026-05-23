/* gatt_service_shims.c — per-service TI BLE-stack callback shims for
 * the 9 non-backoffice GATT services (0x5510, 0x5520, 0x5530, 0x5540,
 * 0x5570, 0x5590, 0x55A0, 0x55C0 — plus svc 0x5560 in its own file
 * `gatt_svc_5560.c` which served as the worked example).
 *
 * All 9 shim pairs are byte-uniform — 140 B write, 94 B read — and
 * differ only in three constants:
 *   1. the hardcoded svc UUID literal,
 *   2. the per-service char-UUID-to-index function,
 *   3. the mailbox-pointer literal (RAM address of the per-service
 *      mailbox struct's `.callback_ptr` slot, which is `mailbox_base + 4`).
 *
 * Each shim does the equivalent of `gatt_svc_5560.c` — see that file
 * for the worked-out architectural notes. The summary:
 *
 *   read shim:  resolve attr → char_idx via per-service matcher; if
 *               valid AND the mailbox's callback has a non-NULL read
 *               slot (+0x08), call it; on success copy result into
 *               caller's buffer.
 *
 *   write shim: route CCCD-descriptor writes (attr type-tag 0x02 +
 *               desc UUID 0x2902) through the CCCD dispatch slot
 *               (+0x04) followed by `cccd_write_validate`; route
 *               normal-char writes through the normal write slot (+0x00).
 *
 * Mailbox struct (per-service, populated by the matching init helper
 * called from `FUN_00012BC4` — see `protocols/ssp.c` notes):
 *
 *   +0x00 u8  service_byte (small numeric id, not the 16-bit UUID)
 *   +0x04 (callback_ptr_slot) — what the shim literal addresses
 *           — runtime value is a pointer to the **shared dispatch
 *             struct** at RAM `0x20005A30`, populated at startup
 *   +0x08..+0xN per-char notification value caches (12 B each)
 *
 * Shared dispatch struct (RAM 0x20005A30, identical for all services):
 *   +0x00 normal-char write dispatcher
 *   +0x04 CCCD write dispatcher
 *   +0x08 read dispatcher
 *
 * **Mailbox sharing.** Sampled literals show:
 *   - svc 0x5510, 0x5520, 0x55A0 all reference mailbox at `0x20005184`
 *     (mailbox slot at `0x20005188` = +4). That's a 3-way share —
 *     likely because all three are "thin" services whose per-service
 *     init is the single-char `FUN_00021290` (only 1 notification
 *     cache, no per-service state worth duplicating).
 *   - Other services each have their own mailbox.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "bleware.h"

/* Same dispatch-table shape as in gatt_svc_5560.c. */
struct gatt_dispatch_vtable {
    int (*write_normal)(void *attr, uint32_t conn, uint16_t svc,
                        uint8_t idx, uint8_t *value, uint16_t len_plus_offset);
    int (*write_cccd)(void *attr, uint32_t conn, uint16_t svc,
                      uint8_t idx, uint8_t *value, uint16_t len,
                      uint16_t offset);
    int (*read_attr)(void *attr, uint32_t conn, uint16_t svc,
                     uint8_t idx, uint32_t value_storage,
                     uint8_t *out_buf, uint8_t op_byte,
                     uint16_t *out_len);
};

/* CCCD helper from gatt_svc_5560.c. */
extern int cccd_write_validate(uint32_t conn, void *attr,
                               const uint8_t *value, uint16_t len,
                               uint16_t offset, uint16_t supported_mask);

#define ATT_TYPE_CCCD 0x02
#define UUID16_CCCD   0x2902

/* Generic core — implements one shim pair's behaviour parameterised
 * by (svc_uuid, char_uuid_to_index, mailbox_callback_slot_ptr).
 *
 * The mailbox callback slot is the `mailbox_base + 4` address: a
 * pointer-to-pointer where the OEM init code stored the per-service
 * callback (= pointer to the shared dispatch vtable struct). */

static inline uint8_t gatt_shim_read_core(
    uint16_t                       svc_uuid,
    uint8_t                      (*char_uuid_to_index)(uint8_t *attr),
    struct gatt_dispatch_vtable * volatile *mailbox_callback_slot,
    uint32_t                       conn,
    void                          *attr,
    uint8_t                       *out_buf,
    uint16_t                      *out_len,
    uint16_t                       offset,
    uint8_t                        op_byte)
{
    uint8_t idx = char_uuid_to_index((uint8_t *)attr);
    uint8_t rc  = 1;

    struct gatt_dispatch_vtable *vt = *mailbox_callback_slot;
    if (vt->read_attr != NULL && idx != 0xFF) {
        uint32_t shadow_ptr = *(uint32_t *)((uint8_t *)attr + 6) + offset;
        rc = vt->read_attr(attr, conn, svc_uuid, idx, shadow_ptr,
                           out_buf, op_byte, out_len);
        if (rc == 0) {
            memcpy(out_buf, (const void *)shadow_ptr, *out_len);
        }
    }
    return rc;
}

static inline uint8_t gatt_shim_write_core(
    uint16_t                       svc_uuid,
    uint8_t                      (*char_uuid_to_index)(uint8_t *attr),
    struct gatt_dispatch_vtable * volatile *mailbox_callback_slot,
    uint32_t                       conn,
    void                          *attr,
    uint8_t                       *value,
    uint16_t                       len,
    uint16_t                       offset)
{
    uint8_t                     *attr_bytes = (uint8_t *)attr;
    struct gatt_dispatch_vtable *vt         = *mailbox_callback_slot;

    if (attr_bytes[0] == ATT_TYPE_CCCD) {
        uint16_t descriptor_uuid = *(uint16_t *)(attr_bytes + 4);
        if (descriptor_uuid == UUID16_CCCD) {
            uint8_t idx = char_uuid_to_index(attr_bytes);
            uint8_t rc  = vt->write_cccd(attr, conn, svc_uuid, idx,
                                         value, len, offset);
            if (rc == 0) {
                return (uint8_t)cccd_write_validate(conn, attr, value, len,
                                                    offset, 3);
            }
            return rc;
        }
    }

    uint8_t idx = char_uuid_to_index(attr_bytes);
    if (idx == 0xFF) {
        return 0x0A;
    }
    if (vt != NULL && vt->write_normal != NULL) {
        vt->write_normal(attr, conn, svc_uuid, idx, value,
                         (uint16_t)((uint32_t)len + offset));
    }
    return 0;
}

/* Per-service externs — char-UUID-to-index matchers (one per service)
 * and mailbox-callback-slot pointers (per-service global RAM addresses
 * holding a pointer to the shared dispatch struct). The hal_stubs.S
 * provides weak BSS for each `g_svc_XXXX_mailbox_callback`; init code
 * populates them at boot via the per-service registration helpers. */

extern uint8_t svc_5510_char_uuid_to_index(uint8_t *attr);   /* 0x0001D744 */
extern uint8_t svc_5520_char_uuid_to_index(uint8_t *attr);   /* 0x0001AB6C */
extern uint8_t svc_5530_char_uuid_to_index(uint8_t *attr);   /* 0x00010EB8 */
extern uint8_t svc_5570_char_uuid_to_index(uint8_t *attr);   /* 0x0001D814 */
extern uint8_t svc_5590_char_uuid_to_index(uint8_t *attr);   /* 0x00020A28 */
extern uint8_t svc_55a0_char_uuid_to_index(uint8_t *attr);   /* 0x0001ABEC */
extern uint8_t svc_55c0_char_uuid_to_index(uint8_t *attr);   /* 0x0001D6DC */

/* svc 0x5540 reuses the SSP-wide UUID-to-index helper (already named
 * `vanmoof_ssp_uuid_to_index` by an earlier Ghidra pass) instead of
 * having its own service-specific matcher. */
extern uint8_t vanmoof_ssp_uuid_to_index(uint8_t *attr);

/* Mailbox callback-slot pointers. Each is the RAM address of the
 * mailbox struct's `+0x04` slot (= where the shared-dispatch-struct
 * pointer is stored). At runtime, dereferencing one gives the shared
 * `gatt_dispatch_vtable` pointer for all services. */
extern struct gatt_dispatch_vtable * volatile g_svc_5510_mailbox_callback;  /* @ 0x20005188 (shared w/ 0x5520, 0x55A0) */
extern struct gatt_dispatch_vtable * volatile g_svc_5520_mailbox_callback;  /* @ 0x20005188 (shared) */
extern struct gatt_dispatch_vtable * volatile g_svc_5530_mailbox_callback;  /* @ 0x20003D30 */
extern struct gatt_dispatch_vtable * volatile g_svc_5540_mailbox_callback;  /* @ 0x20003998 */
extern struct gatt_dispatch_vtable * volatile g_svc_5570_mailbox_callback;  /* @ 0x20004F78 */
extern struct gatt_dispatch_vtable * volatile g_svc_5590_mailbox_callback;  /* @ 0x20005214 */
extern struct gatt_dispatch_vtable * volatile g_svc_55a0_mailbox_callback;  /* @ 0x20005188 (shared) */
extern struct gatt_dispatch_vtable * volatile g_svc_55c0_mailbox_callback;  /* @ 0x20004EC4 */

/* `DEFINE_GATT_SHIM_PAIR` — emit both the read and write shim for
 * one service. The OEM keeps these as separate strong symbols because
 * the TI BLE-stack stores function pointers per service in a
 * `gattServiceCBs_t`. Macros let us share the body without losing
 * the OEM's two-function-per-service layout. */

#define DEFINE_GATT_SHIM_PAIR(svc_hex_lc, svc_uuid_literal, matcher_fn, mailbox_callback) \
    uint8_t svc_##svc_hex_lc##_read_attr_cb (uint32_t  conn,                              \
                                             void     *attr,                              \
                                             uint8_t  *out_buf,                           \
                                             uint16_t *out_len,                           \
                                             uint16_t  offset,                            \
                                             uint16_t  max_len,                           \
                                             uint8_t   op_byte)                           \
    {                                                                                     \
        (void)max_len;                                                                    \
        return gatt_shim_read_core(svc_uuid_literal, matcher_fn,                          \
                                   &mailbox_callback,                                     \
                                   conn, attr, out_buf, out_len,                          \
                                   offset, op_byte);                                      \
    }                                                                                     \
    uint8_t svc_##svc_hex_lc##_write_attr_cb(uint32_t  conn,                              \
                                             void     *attr,                              \
                                             uint8_t  *value,                             \
                                             uint16_t  len,                               \
                                             uint16_t  offset)                            \
    {                                                                                     \
        return gatt_shim_write_core(svc_uuid_literal, matcher_fn,                         \
                                    &mailbox_callback,                                    \
                                    conn, attr, value, len, offset);                      \
    }

DEFINE_GATT_SHIM_PAIR(5510, 0x5510, svc_5510_char_uuid_to_index, g_svc_5510_mailbox_callback)
DEFINE_GATT_SHIM_PAIR(5520, 0x5520, svc_5520_char_uuid_to_index, g_svc_5520_mailbox_callback)
DEFINE_GATT_SHIM_PAIR(5530, 0x5530, svc_5530_char_uuid_to_index, g_svc_5530_mailbox_callback)
DEFINE_GATT_SHIM_PAIR(5540, 0x5540, vanmoof_ssp_uuid_to_index,   g_svc_5540_mailbox_callback)
DEFINE_GATT_SHIM_PAIR(5570, 0x5570, svc_5570_char_uuid_to_index, g_svc_5570_mailbox_callback)
DEFINE_GATT_SHIM_PAIR(5590, 0x5590, svc_5590_char_uuid_to_index, g_svc_5590_mailbox_callback)
DEFINE_GATT_SHIM_PAIR(55a0, 0x55A0, svc_55a0_char_uuid_to_index, g_svc_55a0_mailbox_callback)
DEFINE_GATT_SHIM_PAIR(55c0, 0x55C0, svc_55c0_char_uuid_to_index, g_svc_55c0_mailbox_callback)

/* Note on svc 0x5560 — it has its own .c file (`src/gatt_svc_5560.c`)
 * because it served as the worked-example case for understanding the
 * shim mechanism, and contains the longer architectural comment block.
 * If we ever want to consolidate, that file's pair could become a
 * `DEFINE_GATT_SHIM_PAIR(5560, …)` line here. For now it's kept
 * separate for documentation density. */