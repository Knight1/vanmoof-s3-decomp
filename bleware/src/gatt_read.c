/* gatt_read.c — central GATT read dispatcher.
 *
 * OEM symbol: `xs3_gatt_process_read_event` @ 0x000061c0
 * OEM source path embedded in the binary at 0x00006468:
 *     `source/xs3_gatt_read.c`
 *
 * This is the read-side analogue of `xs3_gatt_process_write_event`
 * (src/gatt_write.c). It is called by the TI BLE-stack ReadAttrCB for
 * every characteristic in the 11-service registry. Most chars are
 * Modbus-bridged: a read here triggers a synchronous "fetch" on the
 * inter-module Modbus bus (`module_publish_sync_with_timeout`) so the
 * value is fresh, then the local copy is returned over BLE. A handful
 * of chars have bespoke producer functions (log block count, ECC-key
 * derivations, connection state) wired into the per-(svc, char_idx)
 * switch below.
 *
 * Authentication and encryption are gated by the per-char flags byte
 * (entry +0xC):
 *   bit 0 — outgoing payload encrypted with the **manufacturing key**
 *           (used by static identity/MAC readouts on svc 0x5540)
 *   bit 1 — manufacturing key must be available; if not, return 2
 *   bit 2 — session key required (cleared by 0x5502 handshake) AND
 *           outgoing payload encrypted with the session key
 *   bit 7 — permanently denied (always returns 2)
 *
 * The runtime "permission mask" (`runtime_permission_mask()` @ 0x00026050)
 * carries a 32-bit set of unlocked capabilities; the entry's required
 * mask is at +0x10 and must be a subset of the runtime mask, otherwise
 * the read returns 2.
 *
 * Return codes:
 *   0    — success, length written to *out_len
 *   1    — empty (computed length was zero)
 *   2    — permission denied
 *   4    — length out of advertised char range
 *   0x11 — scratch alloc failed
 *   0xe  — Modbus sync-fetch failed
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "bleware.h"

/* GATT registry entry layout (28-byte item), shared with gatt_write.c.
 * Fields relevant to reads:
 *   +0x00 u16 char_uuid           (e.g. 0x5505)
 *   +0x02 u16 size_min            (min advertised length)
 *   +0x04 u16 size_max            (max advertised length)
 *   +0x0C u8  flags               (see above)
 *   +0x10 u32 required_perm_mask
 *   +0x1A u8  refresh_from_modbus (if non-zero, pre-fetch on ATT_READ_REQ)
 */

extern void *gatt_config_lookup_item(uint16_t svc_uuid, uint16_t char_idx);
extern int   ble_connection_get_session_key(uint32_t conn_handle);
extern uint32_t runtime_permission_mask(void);

/* `att_mtu_clamp` declared in log_gatt.c. Same FUN_000229B0. */
/* att_mtu_clamp — declared in bleware.h */

/* Synchronous Modbus fetch: publish FETCH frame for `cmd_id`, then
 * pend on the per-module reply semaphore for `timeout_ms` ms. Returns
 * 0 on reply, -1 on timeout. OEM `module_publish_sync_with_timeout` @
 * 0x0001eef4. */
extern int  module_publish_sync_with_timeout(uint32_t module_idx,
                                             uint16_t cmd_id,
                                             uint32_t timeout_ms);

/* True if no Modbus transaction is currently in-flight (the bus is
 * idle so we may initiate a sync fetch). OEM @ 0x00026594. */
extern int  module_bus_is_idle(void);

/* AES-128 block encrypt (TI ROM via jump table). Encrypts one 16-byte
 * block in `src` into `dst` under the 16-byte key. OEM @ 0x00020898. */
extern void aes128_block_encrypt(const uint8_t *key,
                                 uint32_t       keylen,
                                 const uint8_t *src,
                                 uint8_t       *dst);

/* Heap scratch alloc/free (TI-RTOS pool, fits 0xFF B). */
extern uint8_t *gatt_scratch_alloc(uint32_t size);
extern void     gatt_scratch_free(uint8_t *p);

/* Producers wired into specific (svc, char_idx) read targets. */
extern uint32_t log_block_count_get(void);                     /* 0x00020338 */
extern uint8_t  log_total_size_byte(void);                     /* 0x000273dc */
/* `timekeeper_read_be` declared in bleware.h (now strong in
 * src/timekeeper.c). */
extern int      ble_conn_state_byte(uint32_t conn, uint8_t *out_byte);  /* 0x000228b0 */
extern int      backoffice_status_u16(uint32_t conn, uint16_t *out);    /* 0x00022970 */
extern void     ecc_sign_with_factory_key(void *dst, uint32_t dst_len,
                                          const void *curve_params,
                                          int param4, int param5,
                                          int param6);        /* 0x0002617e */
extern uint16_t trng_fill_16(void *dst);                      /* ROM thunk 0x1002fdda */

/* The ECC curve-parameter blob used by the 0x5540 ECC sign path lives
 * inline in the function literal pool at flash 0x000064C0 (20 B). */
extern const uint8_t g_ecc_curve_params[];                    /* LAB_000064c0 */

/* ATT opcode 0x0A = Read Request. The opaque param_7 from the
 * BLE-stack callback. */
#define ATT_OP_READ_REQ 0x0A

int xs3_gatt_process_read_event(uint32_t       module_idx,
                                uint32_t       conn_handle,
                                uint16_t       svc_uuid,
                                uint16_t       char_idx,
                                uint8_t       *out_buf,
                                uint16_t      *out_len,
                                uint8_t        att_opcode)
{
    uint16_t mtu_clamp = 0;
    att_mtu_clamp(conn_handle, &mtu_clamp);

    const uint8_t *entry = (const uint8_t *)gatt_config_lookup_item(svc_uuid, char_idx);
    uint32_t session_key = (uint32_t)ble_connection_get_session_key(conn_handle);

    uint32_t required_perm = *(const uint32_t *)(entry + 0x10);
    uint32_t value_len     = *(const uint16_t *)(entry + 2);   /* size_min default */
    uint8_t  flags         = entry[0xC];

    /* Session-key gate. */
    if ((flags & 0x04) && session_key == 0) {
        return 2;
    }
    /* Manufacturing-key gate. */
    if ((flags & 0x02) && manufacturing_key_get_or_init_default() == NULL) {
        return 2;
    }

    /* If this characteristic is a Modbus-bridged readout (entry +0x1A
     * set) AND the bus is idle, fire a synchronous FETCH so the local
     * shadow copy is up to date before we serve the BLE read. */
    if (att_opcode == ATT_OP_READ_REQ && entry[0x1A] != 0 && module_bus_is_idle() == 0) {
        uint16_t cmd_id = (uint16_t)(svc_uuid + char_idx + 1);
        if (module_publish_sync_with_timeout(conn_handle, cmd_id, 150 /* ms */) != 0) {
            /* monitor_log("Failed in ssp synchronization u%d", cmd_id); */
            return 0xE;
        }
    }

    /* Per-(svc, char_idx) read producers. Anything not listed here
     * falls through to the generic "Modbus-shadow copy is already in
     * `out_buf` from the FETCH above" path. */
    if (svc_uuid == 0x5540) {
        /* Manufacturer authentication / ECC keys. */
        if (char_idx == 10) {
            memset(out_buf, 0, 16);
            ecc_sign_with_factory_key(out_buf, 16, g_ecc_curve_params, 1, 4, 1);
            goto finalize;
        }
        if (char_idx == 16) {
            memset(out_buf + 16, 0, 16);
            ecc_sign_with_factory_key(out_buf + 16, 16, g_ecc_curve_params, 1, 4, 1);
            value_len = *(const uint16_t *)(entry + 4);   /* size_max */
            goto finalize;
        }
        if (char_idx == 17) {
            memset(out_buf, 0, 16);
            uint16_t produced = trng_fill_16(out_buf);
            uint16_t cap = *(const uint16_t *)(entry + 4);
            value_len = (produced < cap) ? produced : cap;
            goto finalize;
        }
    }

    if (svc_uuid == 0x55C0) {
        /* Log-readout metadata. */
        if (char_idx == 1) {
            /* 4 BE bytes: number of 16-byte log blocks ready for readout. */
            uint32_t blocks = log_block_count_get();
            memset(out_buf, 0, 16);
            out_buf[0] = (uint8_t)(blocks >> 24);
            out_buf[1] = (uint8_t)(blocks >> 16);
            out_buf[2] = (uint8_t)(blocks >>  8);
            out_buf[3] = (uint8_t)blocks;
            goto finalize;
        }
        if (char_idx == 2) {
            /* Effective total log byte-size: ((byte & 0xFFF) << 4). */
            value_len = ((uint32_t)log_total_size_byte() & 0xFFFu) << 4;
            goto finalize;
        }
    }

    if (svc_uuid == 0x5520 && char_idx == 1) {
        /* BLE connection-state byte → out_buf[1]. */
        ble_conn_state_byte(conn_handle, out_buf + 1);
        goto finalize;
    }

    if (svc_uuid == 0x5560 && char_idx == 6) {
        /* Timekeeper / RTC readout → 4 LE bytes from the LOW word of the
         * 8-byte timestamp. OEM @ 0x000062b6: bl 0x00027448 returns r0 =
         * the low return register, and FUN_00027448's epilogue loads
         * r0 = [sp+4] = scratch[1] (the low half of its CONCAT44 value).
         * The four output bytes are little-endian: out_buf[0]=word&0xFF,
         * out_buf[1]=word>>8, out_buf[2]=word>>16, out_buf[3]=word>>24. */
        uint64_t ts = timekeeper_read_be();
        uint32_t word = (uint32_t)ts;
        out_buf[0] = (uint8_t)word;
        out_buf[1] = (uint8_t)(word >>  8);
        out_buf[2] = (uint8_t)(word >> 16);
        out_buf[3] = (uint8_t)(word >> 24);
        goto finalize;
    }

    if (svc_uuid == 0x5500 && char_idx == 0) {
        /* Backoffice service status word (16 bits). */
        uint16_t status = 0;
        backoffice_status_u16(conn_handle, &status);
        *(uint16_t *)out_buf = status;
        value_len = 2;
        goto finalize;
    }

    /* All other chars: out_buf already contains the Modbus shadow. */

finalize:
    if (mtu_clamp < value_len) {
        value_len = mtu_clamp;
    }

    /* If the char will be AES-encrypted (mfg-key or session-key bits),
     * round the length up to a 16-byte boundary, then cap to the MTU
     * and the entry's size_max. */
    if (flags & 0x06) {
        if (value_len & 0xF) {
            value_len = (value_len & 0xFFF0u) + 0x10;
        }
        while (mtu_clamp < value_len) {
            value_len -= 0x10;
        }
        while (*(const uint16_t *)(entry + 4) < value_len) {
            value_len -= 0x10;
        }
    }

    /* OEM @ 0x0000639e checks the empty case FIRST: a zero computed
     * length returns 1 ("empty") before any range check. Only a
     * non-zero length is range-checked against [size_min, size_max]. */
    if (value_len == 0) {
        return 1;
    }
    if (value_len < *(const uint16_t *)(entry + 2) ||
        value_len > *(const uint16_t *)(entry + 4)) {
        return 4;
    }

    /* Runtime permission mask check + deny bit. */
    uint32_t perm = runtime_permission_mask();
    if ((required_perm & perm) != required_perm || (flags & 0x80)) {
        return 2;
    }

    /* AES-128 ECB encrypt each 16-byte block, in-place, with either
     * the manufacturing key (flags bit 1) or the session key (flags
     * bit 2). Scratch buffer is owned by the GATT heap pool. */
    uint32_t nblocks = value_len >> 4;
    if (flags & 0x02) {
        uint8_t *scratch = gatt_scratch_alloc(0xFF);
        if (scratch == NULL) {
            return 0x11;
        }
        for (uint32_t i = 0; i < nblocks; i++) {
            const uint8_t *key = manufacturing_key_get_or_init_default();
            aes128_block_encrypt(key, 16, out_buf + i * 16, scratch + i * 16);
        }
        memcpy(out_buf, scratch, value_len);
        gatt_scratch_free(scratch);
    }
    if (flags & 0x04) {
        uint8_t *scratch = gatt_scratch_alloc(0xFF);
        if (scratch == NULL) {
            return 0x11;
        }
        for (uint32_t i = 0; i < nblocks; i++) {
            uint8_t *key = (uint8_t *)(uintptr_t)ble_connection_get_session_key(conn_handle);
            aes128_block_encrypt(key, 16, out_buf + i * 16, scratch + i * 16);
        }
        memcpy(out_buf, scratch, value_len);
        gatt_scratch_free(scratch);
    }

    if (out_len != NULL) {
        *out_len = (uint16_t)value_len;
    }
    return 0;
}

/* GATT scratch buffer alloc — thin wrapper around monitor_alloc.
 * OEM @ ~0x00026EE4 (thunk to ROM alloc). */
uint8_t *gatt_scratch_alloc(uint32_t size)
{
    return (uint8_t *)monitor_alloc(size);
}

/* GATT scratch buffer free — thin wrapper around monitor_free. */
void gatt_scratch_free(uint8_t *p)
{
    monitor_free(p);
}

/* Backoffice status word — same as indicate_seq_peek.
 * OEM @ 0x00022970. */
int backoffice_status_u16(uint32_t conn, uint16_t *out)
{
    return indicate_seq_peek(conn, out);
}

/* TI-RTOS NVS driver open — vendor-stock, returns a handle. */
void *nvs_open(void *params)
{
    (void)params;
    return NULL;
}
