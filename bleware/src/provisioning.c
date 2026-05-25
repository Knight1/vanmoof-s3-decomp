/* provisioning.c — backoffice GATT message handler.
 *
 * OEM source: `source/xs3_gatt_backoffice.c` (filename embedded at
 * flash `0x00004140`; symbol name `gatt_handle_backoffice_message_data`
 * at flash `0x0002b4b4`). This module implements VanMoof's GATT
 * "backoffice" service — the channel their service tools / factory
 * tooling use to push keys, configuration, and module commands into
 * the device.
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
 *   gatt_handle_backoffice_message_data()    @ OEM 0x00003E78
 *       The backoffice characteristic's write callback. Handles a
 *       single GATT-write fragment, decrypts its 16-byte AES blocks
 *       using the M-Key, accumulates them in a per-connection RAM
 *       buffer, and — on the final fragment — validates an embedded
 *       CRC-16/Modbus and dispatches one of nine sub-commands. The
 *       caller then sends a status reply on a separate GATT channel.
 *
 * The wire framing for each GATT write is:
 *
 *   buf[0]              = 0x00 (continuation) or 0x01 (final fragment)
 *   buf[1]              = byte offset within the reassembly buffer
 *                         where the decrypted payload of THIS fragment
 *                         is written (caller-managed cursor)
 *   buf[2..2+N-1]       = N × 16 bytes of AES-CBC-or-ECB ciphertext;
 *                         decrypted in-place via the CryptoCC26X2 ROM
 *                         driverlib (queued through the TI-RTOS worker
 *                         on `block_dispatch_queue_post`).
 *
 * After all fragments arrive, the assembled plaintext at
 * `g_backoffice_reassembly + 0x10` carries this layout:
 *
 *   +0x01            high byte of subcmd-context word (`local_2c[31:24]`)
 *   +0x02            ditto (`local_2c[23:16]`)
 *   +0x03            ditto (`local_2c[15: 8]`)
 *   +0x04..0x05      *unread* in the linear path
 *   +0x11..+0x13     three more bytes mixed into the 24-bit "context"
 *                    word `local_2c` echoed back in the reply
 *   +0x14..+0x15     uVar19 — 16-bit sub-command (BE)
 *   +0x16            cursor — index of the trailing CRC pair
 *   +0x17 onward     sub-command-specific arguments
 *   +(cursor+7..+8)  CRC-16/Modbus over plaintext bytes [0..cursor+6]
 *
 * Sub-commands (uVar19 high.low = byte at +0x14, +0x15):
 *
 *   1   write-keyed-record       arg = full 24-byte record at +0x17
 *                                (key at +16, tag at +24); upserts via
 *                                `secrets_upsert_keyed_record`.
 *   2   set-manufacturing-key    arg = 24-byte key bytes at +0x17;
 *                                tag forced to "MKEY", CRC recomputed,
 *                                written into slot 126.
 *   3   delete-by-key            arg = uint32 (BE) key at +0x17..+0x1A;
 *                                scrubs matching slot to 0xFF.
 *   4   ack-noop                 always succeeds (likely heartbeat).
 *   5   module-forward-async     forwards the 1-byte payload at +0x17
 *                                to module 0x5562 over the inter-module
 *                                bus (no reply expected).
 *   6   factory-reset-secrets    erases the entire 4 KB secrets sector
 *                                at ext-flash 0x005A000.
 *   7   bulk-write-keyed-records arg = N × 24-byte records starting at
 *                                +0x07 of the *fragment buffer* (not
 *                                the reassembly buffer); N is derived
 *                                from `(fragment_len - 9) / 0x18`. Each
 *                                input record is re-laid into a 32-byte
 *                                target slot (payload + key + tag) and
 *                                handed to `secrets_upsert_keyed_batch`.
 *   8   module-forward-sync      forwards (u16 cmd, payload, len) to
 *                                the configured module and waits up to
 *                                ~2.5 s for a reply; the module-supplied
 *                                status byte is translated into the
 *                                backoffice status enum.
 *   9   reserved (returns status 5)
 *   A   reserved (returns status 5)
 *
 * Status reply (10 bytes assembled at `g_backoffice_reassembly + 0`,
 * then padded out to 0xF0 and notified on GATT channel 4):
 *
 *   reply[0..3]      24-bit context (`local_2c` BE; see note below)
 *   reply[4..7]      M-ID (slot 124 payload+16 word, BE)
 *   reply[8]         status enum (see below)
 *
 * Status enum:  0=ok, 3=unknown sub-cmd, 4=case8 err -3, 5=case 6/9/A
 *               or other failure, 6=case 3/4 fail, 7=case 1/2 fail or
 *               case 7 "no room", 8=case 7 alloc-fail or case 8 err -1,
 *               9=case 8 err -2, 10=case 8 err -4.
 *
 * Quirk preserved verbatim: the BE serialisation of `local_2c` and the
 * M-ID into the reply uses `>> 0x12` (= >> 18) for the second byte
 * rather than `>> 16`. This produces a 2-bit-shifted byte that bleeds
 * bits from the next byte position. It is reproduced exactly because
 * the receiver (VanMoof service tool) is built to match.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "bleware.h"

/* Tags written into the M-Key record's payload at fixed offsets.
 * Both appear as 4-byte literals at flash 0x000195E8 / 0x000195F0,
 * and "MKEY" again at 0x000222E8. */
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
extern uint32_t  g_mkey_owning_struct_field30; /* flags */
extern uint32_t  g_mkey_owning_struct_field34; /* set to 0xFFFFFFFF */
extern uint32_t  g_mkey_owning_struct_field3C; /* CRC mirror */

/* Backoffice GATT reassembly buffer.  OEM `DAT_000041F8 = 0x2000A248`.
 * Layout:
 *
 *   [0x00..0x0F]   reply scratch (status header sent back to host)
 *   [0x10..0xFF]   decrypted-message reassembly area
 *
 * The reply is notified out as a 0xF0-byte payload from offset 0 via
 * `gatt_notify_channel(4, &g_backoffice_reassembly[0])`, so the trailing
 * bytes of the request remain visible to the client as a debug echo. */
extern uint8_t  *g_backoffice_reassembly;     /* OEM `DAT_000041F8` */

/* CryptoCC26X2 ROM dispatch — see `secrets_provisioning_blob` notes in
 * progress.md. Posts (key, len, src, dst) to a worker; AES happens in
 * the worker via a function pointer in the ROM-table at
 * `_DAT_100001FC + 0x20`. */
extern int  block_dispatch_queue_post(const void *key_record,
                                      uint32_t   block_len,
                                      const void *block,
                                      const void *block_alias);

/* Helpers used inside the file. */
extern uint32_t crc16_modbus(const uint8_t *buf, int len, uint32_t seed); /* OEM 0x0002651C */
/* crc32_le — now declared in bleware.h */
extern int      secrets_record_read(int index, void *out_record);
extern int      secrets_record_write_verify(int index, const void *record);
extern int      secrets_upsert_keyed_record(const void *record_24);
extern int      secrets_upsert_keyed_batch(const void *records, unsigned int count);
extern uint32_t secrets_ensure_mid_record(void);

/* Sub-command helpers (OEM addresses preserved as plate). */
extern int      mkey_record_write_slot126(const void *rec_24);            /* FUN_000222AC */
extern int      secrets_delete_by_key(uint32_t key);                      /* FUN_00024D14 */
extern int      backoffice_ack_noop(void);                                /* FUN_0002774A */
extern int      module_forward_async(uint32_t cmd_id, uint8_t arg);       /* FUN_00024508 */
extern int      secrets_sector_erase(void);                               /* FUN_00026C30 */
extern int      module_forward_sync(uint16_t cmd, const uint8_t *payload, /* FUN_000177E8 */
                                    unsigned int len);

/* BLE connection bookkeeping. */
extern int      ble_connection_is_active(uint32_t conn_idx);              /* FUN_00023D30 */
extern void     ble_connection_touch(uint32_t conn_idx);                  /* FUN_00023608 */
extern void     backoffice_on_success_hook(void);                         /* FUN_00022BE8 */

/* GATT notify dispatch — declared in bleware.h */
extern void     monitor_log(const char *file, int line, uint32_t logger,
                            int level, const char *fmt, ...);

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
 * gatt_handle_backoffice_message_data — backoffice characteristic
 * write callback.  OEM @ 0x00003E78.
 *
 * Called once per BLE GATT-write fragment. The full message can span
 * multiple fragments; only the fragment whose buf[0] is 1 triggers the
 * actual dispatch. Until then this function just decrypts and stages
 * the bytes into the reassembly buffer.
 *
 * Return value: 0 in every normal path (success and dispatch-error
 * both reply via gatt_notify_channel and return 0); -1 only when the
 * fragment is too short or malformed.
 */
int gatt_handle_backoffice_message_data(uint32_t       conn_idx,
                                        const uint8_t *frag,
                                        unsigned int   frag_len)
{
    uint8_t *reasm = g_backoffice_reassembly;

    /* The fragment's first two bytes are header (final-flag + cursor);
     * the rest must be a non-empty multiple of 16 bytes (one or more
     * AES blocks). */
    unsigned int payload_len = (frag_len - 2u) & 0xFFu;
    if ((payload_len & 0x0Fu) != 0 || payload_len == 0) {
        monitor_log("source/xs3_gatt_backoffice.c", 0x66, 0 /* logger */, 2,
                    "Invalid message length <%d>, should be a multiply of 16",
                    payload_len);
        return -1;
    }

    /* Decrypt each 16-byte block in place via the CryptoCC26X2 worker. */
    for (unsigned int i = 0; i < (payload_len >> 4); i++) {
        const uint8_t *src = frag + (i << 4) + 2;
        uint8_t       *key = manufacturing_key_get_or_init_default();
        block_dispatch_queue_post(key, 0x10u, src, src);
    }

    /* Reassembly bounds check. The cursor (frag[1]) plus this
     * fragment's payload must fit in the 0x90-byte reassembly window. */
    unsigned int cursor = (uint8_t)frag[1];
    if (cursor + payload_len > 0x90u) {
        monitor_log("source/xs3_gatt_backoffice.c", 0x74, 0, 2,
                    "This backoffice message exceeds the maximum size");
        return -1;
    }
    memcpy(reasm + 0x10 + cursor, frag + 2, payload_len);

    /* Continuation? Nothing to dispatch yet. */
    if (frag[0] != 0x01) {
        return 0;
    }

    /* === Final fragment: validate CRC and dispatch =================== */

    /* Echo / context word — bytes the reply has to mirror back.
     * Three bytes come from the *reassembled* payload header
     * (reasm[0x11..0x13]), a fourth byte comes from the just-arrived
     * fragment's first reassembly byte (reasm[0x10]). The OEM packs
     * them as a 24-bit BE value with one byte unused. */
    uint8_t hdr_b1 = reasm[0x10];                          /* puVar15[0] */
    uint8_t hdr_b2 = reasm[0x11];
    uint8_t hdr_b3 = reasm[0x12];
    uint8_t hdr_b4 = reasm[0x13];
    uint32_t context_be =
        ((uint32_t)hdr_b1 << 24) | ((uint32_t)hdr_b2 << 16) |
        ((uint32_t)hdr_b3 <<  8) |  (uint32_t)hdr_b4;

    /* 16-bit sub-command, BE. */
    uint16_t subcmd = ((uint16_t)reasm[0x14] << 8) | reasm[0x15];

    /* CRC-16/Modbus over the assembled plaintext, length stashed at
     * reasm[0x16]. The expected CRC pair sits immediately after the
     * checksummed region at reasm[crc_len+7..+8]. */
    unsigned int crc_len = (uint8_t)reasm[0x16];
    uint16_t calc = (uint16_t)crc16_modbus(reasm + 0x10, crc_len + 7, 0xFFFFu);
    uint16_t want = ((uint16_t)reasm[0x10 + crc_len + 7] << 8) |
                     reasm[0x10 + crc_len + 8];

    char status;
    if (calc != want) {
        status = 1;  /* CRC mismatch */
        goto build_reply;
    }

    /* Connection must still be live. Silent drop (return 0, no reply)
     * if the link went away mid-transfer. */
    if (!ble_connection_is_active(conn_idx)) {
        return 0;
    }
    ble_connection_touch(conn_idx);

    /* Sub-command dispatch. The argument data starts at reasm[0x17]. */
    int rc;
    switch (subcmd) {
    case 1: {
        /* Single keyed-record upsert. The record is (16 B payload @
         * +0x17, key @ +0x27..+0x2A, tag @ +0x2B..+0x2E). The caller
         * supplies key and tag verbatim. */
        uint8_t rec[24];
        memcpy(rec,        reasm + 0x17, 0x10);
        memcpy(rec + 0x10, reasm + 0x27, 0x04);   /* key  */
        memcpy(rec + 0x14, reasm + 0x2B, 0x04);   /* tag  */
        rc = secrets_upsert_keyed_record(rec);
        status = (rc == 0) ? 7 : 0;
        break;
    }
    case 2: {
        /* Set manufacturing key (slot 126). The 16-byte key bytes are
         * supplied at +0x17; the wrapper zeroes the key+tag spots and
         * delegates to mkey_record_write_slot126, which fixes the tag
         * to "MKEY" and recomputes the slot CRC before writing. */
        uint8_t rec[24];
        memcpy(rec,        reasm + 0x17, 0x10);
        memset(rec + 0x10, 0, 0x08);
        rc = mkey_record_write_slot126(rec);
        status = (rc == 0) ? 7 : 0;
        break;
    }
    case 3: {
        /* Delete record by 32-bit BE key at +0x17..+0x1A. */
        uint32_t key = ((uint32_t)reasm[0x17] << 24) |
                       ((uint32_t)reasm[0x18] << 16) |
                       ((uint32_t)reasm[0x19] <<  8) |
                        (uint32_t)reasm[0x1A];
        rc = secrets_delete_by_key(key);
        status = (rc == 0) ? 6 : 0;
        break;
    }
    case 4:
        rc = backoffice_ack_noop();
        status = (rc == 0) ? 6 : 0;
        break;
    case 5:
        module_forward_async(0x5562, reasm[0x17]);
        status = 0;
        break;
    case 6:
        rc = secrets_sector_erase();
        status = (rc != 1) ? 5 : 0;
        break;
    case 7: {
        /* Bulk import. The fragment carries N × 24-byte source records
         * starting at frag[7]; the worker re-lays each into a 32-byte
         * secrets-slot shape (16 B payload + key + tag) and hands the
         * batch to secrets_upsert_keyed_batch.
         *
         * NOTE: Unlike the other cases, case 7's inputs come from the
         * just-arrived *fragment* buffer rather than the reassembly
         * buffer — see the OEM use of `puVar15` indexing at +7 / +0x17
         * etc., and the `(payload_len - 9) / 0x18` count formula. */
        unsigned int n = (payload_len - 9u) / 0x18u;
        void *scratch = monitor_alloc((n & 0x7FFu) << 5);
        if (scratch == NULL) {
            status = 8;
            break;
        }
        uint8_t *dst = (uint8_t *)scratch;
        for (unsigned int i = 0; i < n; i++) {
            unsigned int base_payload = (i * 0x18u + 7u)  & 0xFFFFu;
            unsigned int base_key     = (i * 0x18u + 0x17u) & 0xFFFFu;
            unsigned int base_tag     = (i * 0x18u + 0x1Bu) & 0xFFFFu;

            memcpy(dst,       reasm + 0x10 + base_payload, 0x10);
            memcpy(dst + 0x10, reasm + 0x10 + base_key,    0x04);
            memcpy(dst + 0x14, reasm + 0x10 + base_tag,    0x04);
            dst += 0x20;
        }
        rc = secrets_upsert_keyed_batch(scratch, n);
        if      (rc ==  0) status = 0;
        else if (rc == -1) status = 7;
        else               status = 5;
        monitor_free(scratch);
        break;
    }
    case 8: {
        /* Forward (u16 cmd, ptr, len) to a peer module synchronously
         * and translate its reply status. */
        uint16_t cmd = ((uint16_t)reasm[0x17] << 8) | reasm[0x18];
        rc = module_forward_sync(cmd, reasm + 0x19, payload_len - 2u);
        switch (rc) {
        case  0: status = 0;  break;
        case -1: status = 8;  break;
        case -2: status = 9;  break;
        case -3: status = 4;  break;
        case -4: status = 10; break;
        default: status = 5;  break;
        }
        break;
    }
    default:
        if ((unsigned int)(subcmd - 9) < 2) {
            status = 5;        /* sub-cmds 9 / 0xA reserved, always-fail */
        } else {
            status = 3;        /* unknown sub-cmd */
        }
        break;
    }

    if (status == 0) {
        monitor_log("source/xs3_gatt_backoffice.c", 0x14D, 0, 0,
                    "Successfully processed backoffice message with type %d",
                    subcmd);
    } else {
        monitor_log("source/xs3_gatt_backoffice.c", 0x149, 0, 2,
                    "backoffice-message type 0x%d returns <%d>",
                    subcmd, status);
    }

build_reply: {
    /* Assemble the 9-byte reply prologue inside the reassembly buffer's
     * first 16 bytes. The remainder of the 0xF0-byte notify payload
     * still holds whatever was last written there — this is intentional
     * (the OEM sends the entire 0xF0 window to give the service tool a
     * debug echo). */
    uint32_t mid = secrets_ensure_mid_record();
    memset(reasm, 0, 0x10);

    reasm[0] = (uint8_t)(context_be >> 24);
    reasm[1] = (uint8_t)(context_be >> 18);  /* sic — OEM uses >>18 */
    reasm[2] = (uint8_t)(context_be >>  8);
    reasm[3] = (uint8_t) context_be;
    reasm[4] = (uint8_t)(mid >> 24);
    reasm[5] = (uint8_t)(mid >> 18);         /* sic — see plate comment */
    reasm[6] = (uint8_t)(mid >>  8);
    reasm[7] = (uint8_t) mid;
    reasm[8] = (uint8_t) status;

    if (status == 0) {
        backoffice_on_success_hook();
    }

    gatt_notify_channel(4, reasm);
    return 0;
    }
}

/* Backoffice sub-command 4: heartbeat / capability probe. Always
 * returns 1 (ack). OEM @ 0x0002774A (4 B). */
int backoffice_ack_noop(void)
{
    return 1;
}

/* Backoffice sub-command 2: write the M-Key to slot 126. Takes a
 * 24-byte record payload, stamps the "MKEY" tag at offset +24,
 * computes CRC-32 over the full 28-byte payload, and writes the
 * 32-byte record via secrets_record_write_verify. OEM @ 0x000222AC. */
int mkey_record_write_slot126(const void *rec_24)
{
    extern const uint32_t g_mkey_tag;           /* DAT_000222E8 = "MKEY" LE */
    const uint8_t        *src = (const uint8_t *)rec_24;
    uint8_t               buf[32];

    memcpy(buf, src, 24);
    memcpy(buf + 24, &g_mkey_tag, 4);
    uint32_t crc = crc32_le(0xFFFFFFFFu, buf, 28);
    memcpy(buf + 28, &crc, 4);

    return secrets_record_write_verify(0x7E, buf);
}

/* Firmware abort — logs a "Platform reset" message via monitor_log,
 * waits for pending TX to drain, sets a flag, and infinite-loops.
 * OEM @ 0x0001F7F8 (54 B). */
void firmware_abort(void)
{
    extern const char s_Platform_reset[];   /* "Platform reset" @ flash */
    extern const char s_source_xs3_app_c[]; /* "source/xs3_app.c" */
    extern uint32_t    g_abort_flag;        /* DAT_0001F848 */
    extern int         FUN_000202e4(void);  /* pending-TX check */
    extern void        FUN_00027274(void);  /* power-safe hook */
    int                done;

    monitor_log(s_source_xs3_app_c + 1, 0x2FB, 0, 1, s_Platform_reset);

    do {
        done = FUN_000202e4();
    } while (done == 0);

    FUN_00027274();
    g_abort_flag = 1;

    for (;;) {
        /* infinite loop — device is halted */
    }
}
