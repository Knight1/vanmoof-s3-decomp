/* gatt_write.c — central GATT write dispatcher.
 *
 * OEM symbol: `xs3_gatt_process_write_event` @ 0x00004DB0
 * OEM source path embedded at flash 0x00005114:
 *     `source/xs3_gatt_write.c`
 *
 * Write-side analogue of `xs3_gatt_process_read_event` (src/gatt_read.c).
 * The TI BLE-stack `WriteAttrCB` for every service hands off here
 * through a one-slot indirection (`*0x20003F84 -> 0x20005A30 -> +0`);
 * every per-service WriteAttrCB shim ends up here for normal writes and
 * at `+0x4` of the same vtable for CCCD writes.
 *
 * The function reads a packed "write event" struct (assembled by the
 * service-shim that called us) and dispatches based on the service UUID
 * to the right per-service write handler. Along the way it handles:
 *
 *   - the **session-key handshake** on (svc 0x5500, char 0x5502, opcode
 *     0x14, 20-byte payload) — derives a fresh session key from the
 *     client nonce and pins it on the BLE connection
 *   - length validation against the registry entry's size_min/size_max
 *   - **per-char crypto flags** (entry +0xD bits):
 *       bit 0  session-key required (must be set on the connection)
 *       bit 1  ECB-decrypt incoming payload with the manufacturing key
 *              and copy plain bytes into the local backing store
 *       bit 2  session-key-stream decrypt (16-byte blocks via
 *              `block_dispatch_queue_post`)
 *       bit 3  indicate-confirm: first u16 of payload must match the
 *              pending indicate sequence number from `indicate_seq_peek`
 *   - per-char **permission gate** (entry +0x14): `runtime_permission_mask()`
 *     must be a superset of the entry's mask
 *   - per-char **padding-byte check** (entry +0x18 signed sentinel /
 *     entry +0x19 count): trailing padding bytes must be zero
 *
 * On successful gating the function switches on the service UUID and
 * either calls the matching per-service handler (`oad_gatt_write_handler`,
 * `log_gatt_write_handler`, `gatt_handle_backoffice_message_data`) or
 * relays the payload onto the inter-module Modbus bus via
 * `module_publish_command` / `module_forward_async`.
 *
 * Return values (low byte preserved by the per-service shim):
 *   0           — accepted (handled here or forwarded)
 *   0xffffffff  — rejected (length / auth / pad / handler failure)
 *
 * Side-effect-only errors funnel into `ble_post_disconnect(conn, code)`
 * (probably "send ATT error response"); the function then returns 0
 * because the ATT layer has already signalled the failure.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "bleware.h"

/* ---- GATT registry lookup (shared with gatt_read.c) -------------- */
extern void *gatt_config_lookup_item(uint16_t svc_uuid, uint8_t char_idx);  /* differs slightly: write path passes a byte char_idx */

/* ---- Per-connection session key / counter ----------------------- */
/* `ble_connection_get_session_key`, `ble_connection_set_session_key`
 * declared in bleware.h (see src/ble_connection.c). */
extern int      ble_authenticated_connection_count(void);

/* `auth_derive_session_key` declared in bleware.h (src/auth.c).
 * Derives or synthesises a 32-byte session-key record for a given
 * client key id. Returns a pointer to the global working buffer or
 * NULL on failure. */

extern uint32_t runtime_permission_mask(void);

/* ---- Crypto / framing helpers ----------------------------------- */
extern void     block_dispatch_queue_post(const void *key, uint32_t kind,
                                          const void *src, void *dst);
/* `mfg_key_ecb_decrypt_chunks` declared in bleware.h (src/auth.c). */

/* ---- Indicate-confirm tracking ---------------------------------- *
 * `indicate_seq_peek` / `indicate_seq_advance` declared in bleware.h
 * (see src/ble_connection.c). Returns 0 on a valid conn handle, -1
 * if the per-connection state has gone stale. */

/* ---- Disconnect-on-error path ----------------------------------- *
 * Schedules a force-disconnect of the offending connection via the
 * bluetoothtask user-msg queue. Declared in bleware.h; see
 * src/bluetoothtask_post.c. */

/* ---- Per-service relays -----------------------------------------
 * `backoffice_auth_session_init`, `ssp_relay_u32`, `ssp_relay_u16`,
 * `timekeeper_submit_epoch`, `timekeeper_read_be` all declared in
 * bleware.h. */

extern void     module_publish_command(uint16_t cmd_id, const void *buf,
                                       uint16_t len);
extern int      module_forward_async(uint32_t cmd_id, uint8_t arg);

/* ---- Monitor log (location-aware) ------------------------------- */
extern void     monitor_log(const char *file, int line, const char *fn,
                            int level, const char *fmt, ...);

/* ---- GATT registry entry — write-side fields -------------------- *
 * (See gatt_read.c for the read-side fields; the entry is shared.)
 *
 *   +0x00 u16  char_uuid
 *   +0x02 u16  size_min
 *   +0x04 u16  size_max
 *   +0x08 u32  local_storage_ptr   (RAM shadow for entry's payload)
 *   +0x0D u8   crypto_flags        (bit 0 session-key required; bit 1
 *                                   manufacturing-key ECB; bit 2 stream
 *                                   decrypt; bit 3 indicate-confirm; high
 *                                   bits reserved — bit 7 = "denied")
 *   +0x14 u8   required_perm_mask
 *   +0x18 i8   pad_sentinel        (>=0 → enforce pad-byte check)
 *   +0x19 u8   pad_byte_count
 */

/* ---- "Write event" struct delivered by the per-service shim ----- *
 * (24 bytes header, then up-to-MTU bytes of value data.) The OEM
 * passes a single `int param_1` and indexes by offset. */
struct gatt_write_event {
    uint16_t  reserved0;       /* +0x00 */
    uint16_t  conn_handle;     /* +0x02 */
    uint16_t  svc_uuid;        /* +0x04 */
    uint8_t   char_idx;        /* +0x06 */
    uint8_t   reserved7;       /* +0x07 */
    uint16_t  value_len;       /* +0x08 */
    uint8_t   value[2];        /* +0x0A .. variadic */
    /* On the indicate-confirm path the first u16 of value[] is the
     * sequence number; the rest of the payload starts at +0x0C and
     * `value_len` is decremented by 2. */
};

#define ATT_OP_WRITE_REQ 0x14   /* used by auth handshake gate */

int xs3_gatt_process_write_event(struct gatt_write_event *evt)
{
    const uint8_t *entry;
    uint8_t       *value_data;   /* mutable: stream/ECB decrypt rewrites in-place */
    uint8_t       *local_store;
    void          *session_key;
    uint8_t        crypto_flags;
    uint8_t        perm_mask;
    uint16_t       value_len;
    uint16_t       svc_uuid;
    uint8_t        char_idx;
    uint16_t       conn;
    uint16_t       cmd_id;
    uint16_t       relay_val16 = 0;
    uint16_t       expected_seq;
    int            connections_seen;
    int            already_authed;
    uint32_t       perm_runtime;
    uint8_t        att_err;

    svc_uuid    = evt->svc_uuid;
    char_idx    = evt->char_idx;
    conn        = evt->conn_handle;
    value_len   = evt->value_len;
    value_data  = &evt->value[0];

    entry = (const uint8_t *)gatt_config_lookup_item(svc_uuid, char_idx);
    if (entry == NULL) {
        return 0;
    }

    perm_mask    = entry[0x14];
    crypto_flags = entry[0x0D];

    /* Session-key handshake: client writes 20 bytes to svc 0x5500's
     * 0x5502 characteristic. Bytes 16..19 of the payload are the
     * 32-bit client nonce that seeds session-key derivation. */
    if (*(const uint16_t *)entry == 0x5502 && value_len == 0x14) {
        uint8_t b0 = evt->value[0x10];
        uint8_t b1 = evt->value[0x11];
        uint8_t b2 = evt->value[0x12];
        uint8_t b3 = evt->value[0x13];
        uint32_t nonce = ((uint32_t)b3) | ((uint32_t)b2 << 8) |
                        ((uint32_t)b1 << 16) | ((uint32_t)b0 << 24);

        session_key = auth_derive_session_key(nonce);
        if (session_key == NULL) {
            return -1;
        }

        already_authed = 0;
        if (ble_connection_get_session_key(conn) != 0) {
            void *existing = (void *)(uintptr_t)ble_connection_get_session_key(conn);
            already_authed = (memcmp(existing, session_key, 16) == 0);
        }

        connections_seen = ble_authenticated_connection_count();
        crypto_flags = 0x0C;       /* synthesised: the handshake itself
                                    * is treated as both stream-decrypt
                                    * and indicate-confirm gated */
        if (connections_seen > 0 && !already_authed) {
            return -1;
        }
        ble_connection_set_session_key(conn, session_key);
    } else {
        session_key = (void *)(uintptr_t)ble_connection_get_session_key(conn);
    }

    /* Length must be within the entry's [size_min, size_max] envelope. */
    if (value_len < *(const uint16_t *)(entry + 2) ||
        value_len > *(const uint16_t *)(entry + 4)) {
        return -1;
    }

    /* Bit 7 of crypto_flags marks the characteristic as permanently
     * denied (writes are silently logged and dropped). */
    if (crypto_flags & 0x80) {
        monitor_log("source/xs3_gatt_write.c", 200,
                    "xs3_gatt_process_write_event", 1,
                    "Prop not available");
        return 0;
    }

    /* Stream-decrypt path (16-byte blocks chained through the BLE
     * dispatch queue, finally memcpy'd into the local backing store). */
    if (crypto_flags & 0x04) {
        if (ble_connection_get_session_key(conn) == 0) {
            return -1;
        }
        uint8_t *local_storage = *(uint8_t * const *)(entry + 8);
        for (uint16_t i = 0; i < (value_len >> 4); i++) {
            block_dispatch_queue_post(session_key, 0x10,
                                      value_data + i * 8,
                                      local_storage + i * 0x10);
        }
        memcpy(value_data, local_storage, value_len & 0xfff0);
    }

    /* ECB-decrypt with the manufacturing key, then copy plain bytes
     * back over the value buffer. */
    if (crypto_flags & 0x02) {
        uint8_t *local_storage = *(uint8_t * const *)(entry + 8);
        mfg_key_ecb_decrypt_chunks(local_storage, value_data, value_len);
        memcpy(value_data, local_storage, value_len & 0xfff0);
    }

    /* Indicate-confirm: first u16 of payload is the seq num. Must
     * match the expected seq for this connection. After matching,
     * skip the seq u16 from the rest of the payload. */
    if (crypto_flags & 0x08) {
        uint16_t got_seq;
        memcpy(&got_seq, value_data, 2);
        indicate_seq_peek(conn, &expected_seq);
        if (expected_seq != got_seq) {
            att_err = 1;
            goto send_att_error;
        }
        indicate_seq_advance(conn);
        value_data = &evt->value[2];
        evt->value_len = (uint16_t)(value_len - 2);
        value_len      = evt->value_len;
    }

    /* Session-key gate. */
    if (crypto_flags & 0x01) {
        if (ble_connection_get_session_key(conn) == 0) {
            att_err = 5;
            goto send_att_error;
        }
    }

    /* Default: copy the (possibly decrypted) payload into the entry's
     * local backing store, if it has one. */
    local_store = *(uint8_t * const *)(entry + 8);
    if (local_store != NULL) {
        memcpy(local_store, value_data, value_len);
    }

    /* Permission gate: runtime mask must be a superset of entry mask. */
    perm_runtime = runtime_permission_mask();
    if (perm_mask != (perm_runtime & perm_mask)) {
        att_err = 2;
        goto send_att_error;
    }

    /* Trailing-zero padding check (entry +0x18 signed sentinel acts as
     * a flag; entry +0x19 holds the count). */
    {
        int8_t sentinel = (int8_t)entry[0x18];
        uint8_t pad_n   = entry[0x19];
        if (sentinel >= 0 && pad_n != 0) {
            const uint8_t *p = value_data + (sentinel - 1);
            while (pad_n != 0) {
                p++;
                if (*p != 0) {
                    return -1;
                }
                pad_n--;
            }
        }
    }

    /* Per-service dispatch. */
    switch (svc_uuid) {
    case 0x5500: {  /* backoffice */
        switch (char_idx) {
        case 0x01:
            backoffice_auth_session_init(conn, session_key);
            return 0;
        case 0x02: {
            uint32_t v24 = ((uint32_t)value_data[0] << 16) |
                           ((uint32_t)value_data[1] << 8)  |
                           ((uint32_t)value_data[2]);
            ssp_relay_u32(0x5503, v24);
            return 0;
        }
        case 0x04:
            gatt_handle_backoffice_message_data(conn, value_data, value_len);
            return 0;
        default:
            return 0;
        }
    }

    case 0x5510:    /* OAD */
        return oad_gatt_write_handler(conn, char_idx, value_data, value_len);

    case 0x5520:
        /* Accept only char_idx 0, 2, or 3. */
        if (char_idx != 0 && (uint8_t)(char_idx - 2) > 1) {
            return -1;
        }
        relay_val16 = value_len;
        cmd_id      = (uint16_t)(char_idx + 0x5520 + 1);
        module_publish_command(cmd_id, value_data, relay_val16);
        return 0;

    case 0x5530:
        switch (char_idx) {
        case 2:
        case 4:
        case 5:
        case 7:
            relay_val16 = value_len;
            cmd_id      = (uint16_t)(char_idx + 0x5530 + 1);
            module_publish_command(cmd_id, value_data, relay_val16);
            return 0;
        case 3:
            cmd_id      = 0x5534;
            relay_val16 = 2;
            module_publish_command(cmd_id, value_data, relay_val16);
            return 0;
        case 6:
            cmd_id      = 0x5537;
            relay_val16 = 6;
            module_publish_command(cmd_id, value_data, relay_val16);
            return 0;
        default:
            return -1;
        }

    case 0x5560:    /* timekeeper / RTC service */
        if (char_idx == 1 || (uint8_t)(char_idx - 3) <= 2) {
            relay_val16 = value_len;
            cmd_id      = (uint16_t)(char_idx + 0x5560 + 1);
            module_publish_command(cmd_id, value_data, relay_val16);
            return 0;
        }
        if (char_idx == 6) {
            uint32_t epoch = ((uint32_t)value_data[0] << 24) |
                             ((uint32_t)value_data[1] << 16) |
                             ((uint32_t)value_data[2] << 8)  |
                             ((uint32_t)value_data[3]);
            timekeeper_submit_epoch(epoch);
            uint32_t now = (uint32_t)timekeeper_read_be();
            ssp_relay_u32((uint16_t)(svc_uuid + char_idx + 1), now);
            return 0;
        }
        return -1;

    case 0x5570:
        switch (char_idx) {
        case 0:
        case 3:
            return module_forward_async(0x5570 + char_idx + 1, value_data[0]);
        case 1:
            cmd_id      = 0x5572;
            relay_val16 = 0x0C;
            module_publish_command(cmd_id, value_data, relay_val16);
            return 0;
        default:
            return 0;
        }

    case 0x5580:
        switch (char_idx) {
        case 0x00:
            return module_forward_async(0x5580 + 0 + 1, value_data[0]);
        case 0x01:
            cmd_id      = 0x5582;
            relay_val16 = 3;
            module_publish_command(cmd_id, value_data, relay_val16);
            return 0;
        case 0x03: {
            uint16_t v = (uint16_t)(((uint16_t)value_data[2] << 8) |
                                    value_data[3]);
            ssp_relay_u16(0x5584, v);
            return 0;
        }
        default:
            return 0;
        }

    case 0x5590:
        if (char_idx > 1) {
            return 0;
        }
        return module_forward_async(0x5590 + char_idx + 1, value_data[0]);

    case 0x55A0:
        if (char_idx > 3) {
            return 0;
        }
        relay_val16 = value_len;
        cmd_id      = (uint16_t)(char_idx + 0x55A0 + 1);
        module_publish_command(cmd_id, value_data, relay_val16);
        return 0;

    case 0x55C0:    /* circular log GATT */
        log_gatt_write_handler(conn, char_idx, value_data, value_len);
        return 0;

    default:
        return 0;
    }

send_att_error:
    ble_post_disconnect(conn, att_err);
    return 0;
}
