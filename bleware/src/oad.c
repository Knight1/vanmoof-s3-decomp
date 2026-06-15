/* oad.c — Over-the-Air firmware Download (OAD) GATT handler.
 *
 * OEM source: `source/oad/oad.c` (paths embedded at flash `0x0000B9A4`
 * and `0x0000BD50`).
 *
 * Implements the write callback for VanMoof BLE service `0x5510`,
 * which exposes three characteristics:
 *
 *   0x5511 (16 B, dispatcher-decrypted)  — METADATA channel.
 *       Client sends one 16-byte AES block (decrypted by the central
 *       GATT dispatcher using the per-connection session key, see
 *       xs3_gatt_write.c). The decrypted plaintext carries 9 bytes:
 *           [0]     filetype (0 = bootloader-app at 0x80000;
 *                              1..0x20 = data file at 0x180000 +
 *                                        filetype * 0x80000)
 *           [1..4]  filesize  (big-endian u32)
 *           [5..8]  expected CRC-32 of the full file
 *
 *   0x5512 (up to 255 B, NOT dispatcher-decrypted) — PAYLOAD channel.
 *       Stream of file bytes. For the bootloader file (filetype 0) the
 *       handler applies an additional "scramble decrypt" pass to each
 *       chunk via `FUN_00024740` (same helper the central dispatcher
 *       uses for its property-bit-1 path). For every other file
 *       (filetype != 0) the bytes are taken as-is — VanMoof's PACK
 *       container is already cleartext on flash at `0x80000+`.
 *
 *   0x5513 (16 B, notify-only) — STATUS channel. Outgoing
 *       OAD progress / completion / error codes. Not wired in this
 *       file (sent via `bleware_control_event_post` -> the service's notify
 *       dispatcher -> `gatt_service_notify_dispatch(0x5510, ...)`).
 *
 * Status codes (single-byte arg to `bleware_control_event_post`):
 *
 *     0x12  OAD started — metadata accepted, slot erased
 *     0x13  OAD complete — final CRC matched (unencrypted file)
 *     0x14  block accepted — sent after every block when the file is
 *           encrypted; payload is the block index (BE u32) via
 *           `module_publish_command(0x14, &block_idx, 4)`
 *     0x16  CRC mismatch on final block
 *     0x17  flash open failed
 *     0x12 (overload in error path) — filesize <= slot capacity check
 *     0xF8  invalid file index (filetype > 0x20)
 *     0xFF  filesize > slot capacity
 *
 * OAD state machine — single connection at a time. A 0xFFFF
 * conn_handle in the state struct's first u16 means "idle". A
 * metadata write captures `event.conn_handle` into the state; every
 * subsequent payload chunk validates `param_1 == state.conn_handle`
 * before proceeding (so a different connection cannot interleave).
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "bleware.h"

/* OAD state struct — sits in RAM, addressed by literal-pool entries
 * `DAT_0000BA4C` (used by the metadata path) and `DAT_0000BDC0` (used
 * by the payload path). Both point to the same struct base.
 *
 * Field offsets reproduce what the OEM disassembly does verbatim.
 * Anything we couldn't resolve stays as a u8 hole. */
struct oad_state {
    uint16_t conn_handle;        /* +0x00; 0xFFFF == idle */
    uint16_t _pad_02;            /* +0x02 (unused in the decoded code) */
    uint32_t lock_handle;        /* +0x04; Semaphore_t the OAD-duration lock
                                  *        (oad_state_lock) holds; HwiP guard. */
    uint32_t session_handle;     /* +0x08; separate Semaphore_t guarding the
                                  *        OAD session state (metadata acquire
                                  *        + oad_session_close pend/post). */
    uint8_t  _pad_0C[0x4C - 0x0C]; /* +0x0C..+0x4B */
    uint8_t  filetype;           /* +0x4C; 0 = bootloader, 1..0x20 = data */
    uint8_t  _pad_4D[0x50 - 0x4D];
    uint32_t filesize;           /* +0x50; total bytes expected */
    uint32_t expected_crc;       /* +0x54; CRC-32 over the entire file */
    uint32_t flash_base;         /* +0x58; ext-flash destination start */
    uint32_t flash_cursor;       /* +0x5C; current write offset (absolute) */
    uint32_t bytes_received;     /* +0x60; for block-index reporting */
    uint32_t running_crc;        /* +0x64; CRC-32 accumulator (init 0xFFFFFFFF) */
};

extern struct oad_state g_oad_state;

/* Helpers — OEM addresses kept in plate. */
extern int      oad_state_lock(uint32_t lock_handle, int timeout_ms);    /* FUN_00020098 */
extern void     bleware_control_event_post(uint32_t status);                      /* FUN_000265C4 */
extern void     oad_session_close(void);                                 /* FUN_00025060 */
extern void     FUN_00024740(uint8_t *dst, const uint8_t *src, uint32_t len); /* scramble decrypt */
/* extflash_erase_range, extflash_write — declared in bleware.h */
extern int      extflash_open(void);
extern void     extflash_close(void);
extern int      module_publish_command(uint32_t cmd_id, const void *buf, /* FUN_000244D8 */
                                       uint32_t len);
extern void     monitor_log(const char *file, int line, uint32_t logger,
                            int level, const char *fmt, ...);

/* TI-RTOS Semaphore_post; ROM thunk alias (0x1002CD20). Also used for the
 * metadata brief-release on the +0x04 handle. */
extern void     ti_semaphore_post(uint32_t handle);
/* TI-RTOS Semaphore_pend; ROM thunk alias (0x1002BFB0). Returns 1 on take. */
extern int      ti_semaphore_pend(uint32_t handle, uint32_t timeout_ticks);
/* TI-RTOS Clock_stop; ROM thunk (0x1002E2C4). NOTE: earlier labelled
 * "HwiP_disable" — corrected via the SDK 3.40 cc26x2v2 golden ROM symbol
 * table (see docs/rom-thunk-audit.md). */
extern void     thunk_FUN_1002e2c4(uint32_t handle);

/* TI-RTOS tick-period in microseconds; OEM `*PTR_DAT_0000BA50`. */
extern volatile uint32_t g_ti_tick_period_us;

/* The 4 KB ext-flash sector mask. OEM `DAT_0000BDC8 = 0xFFFFF000`. */
#define EXTFLASH_SECTOR_MASK   0xFFFFF000u
#define EXTFLASH_SECTOR_SIZE   0x1000u
#define OAD_SLOT_SIZE          0x80000u   /* 512 KB per file slot */
#define OAD_BOOT_SLOT_BASE     0x80000u   /* filetype 0 lands here */
#define OAD_DATA_SLOT_BASE     0x180000u  /* filetype 1..0x20 here */

/* OAD GATT write handler — bound to service 0x5510 from the central
 * dispatcher in xs3_gatt_write.c. Indices:
 *     0  →  0x5511 metadata
 *     1  →  0x5512 payload
 *     2  →  0x5513 (no write path; silent return)
 *
 * OEM @ 0x000267A4. */
int oad_gatt_write_handler(uint32_t       conn_handle,
                           int            char_idx,
                           const uint8_t *payload,
                           uint32_t       payload_len)
{
    struct oad_state *s = &g_oad_state;

    if (char_idx == 0) {
        /* === Metadata write (0x5511) =================================
         * First acquire the SESSION semaphore (+0x08) with a DIRECT
         * Semaphore_pend — not via oad_state_lock. The counted-ticks
         * timeout is uxth(1000/tick_us)*10 (OEM: `lsls #1` + `add lsl #3`
         * == *2 + *8 == *10). oad_state_lock would re-scale the ms value
         * by (1000/tick_us) again, so the raw tick count is passed here. */
        const uint32_t tick_us  = g_ti_tick_period_us;
        const uint32_t timeout  = (uint16_t)(1000u / tick_us) * 10u;

        if (ti_semaphore_pend(s->session_handle, timeout) != 1) {
            return -1;
        }
        oad_state_lock(s->lock_handle, 10000);   /* OAD-duration lock on +0x04 */
        ti_semaphore_post(s->lock_handle);       /* release the +0x04 pend */

        s->conn_handle    = (uint16_t)conn_handle;
        s->bytes_received = 0;
        memset(((uint8_t *)s) + 0x4C, 0, 0x58);  /* zero state[0x4C..0xA3] */
        s->flash_cursor   = 0;

        /* Decoded metadata frame:
         *   payload[0]     filetype
         *   payload[1..4]  filesize    (BE u32)
         *   payload[5..8]  expected_crc (BE u32)
         * The OEM reads big-endian via CONCAT chains. */
        s->filetype     = payload[0];
        s->filesize     = ((uint32_t)payload[1] << 24) |
                          ((uint32_t)payload[2] << 16) |
                          ((uint32_t)payload[3] <<  8) |
                           (uint32_t)payload[4];
        s->expected_crc = ((uint32_t)payload[5] << 24) |
                          ((uint32_t)payload[6] << 16) |
                          ((uint32_t)payload[7] <<  8) |
                           (uint32_t)payload[8];
        s->bytes_received = 0;

        monitor_log("source/oad/oad.c", 0xE5, 0 /* logger */, 0,
                    "sends metadata with filetype <%d>", s->filetype);

        /* Slot allocation by filetype. 0 → bootloader at 0x80000 (slot
         * size 0x80000). Otherwise data file at 0x180000 + ft*0x80000. */
        uint32_t slot_capacity;
        if (s->filetype == 0) {
            s->flash_base   = OAD_BOOT_SLOT_BASE;
            s->flash_cursor = OAD_BOOT_SLOT_BASE;
            slot_capacity   = OAD_DATA_SLOT_BASE;     /* 0x180000 — full lower window */
        } else if (s->filetype <= 0x20) {
            s->flash_base   = OAD_DATA_SLOT_BASE + (uint32_t)s->filetype * OAD_SLOT_SIZE;
            s->flash_cursor = s->flash_base;
            slot_capacity   = OAD_SLOT_SIZE;
        } else {
            monitor_log("source/oad/oad.c", 0xF8, 0, 2,
                        "Invalid file index <%d>", s->filetype);
            oad_session_close();
            return -1;
        }

        s->running_crc = 0xFFFFFFFFu;

        if (s->filesize <= slot_capacity) {
            bleware_control_event_post(0x12);   /* metadata accepted */
            return 0;
        }

        monitor_log("source/oad/oad.c", 0xFF, 0, 2,
                    "OAD aborted, filesize <%d> exceeds <%d>",
                    s->filesize, slot_capacity);
        oad_session_close();
        return -1;
    }

    if (char_idx != 1) {
        /* 0x5513 has no write path. */
        return 0;
    }

    /* === Payload write (0x5512) ====================================
     * Reject if no OAD session is active or if the chunk arrived on a
     * different BLE connection than the one that wrote the metadata. */
    if (s->conn_handle == 0xFFFF || conn_handle != s->conn_handle) {
        return -1;
    }
    oad_state_lock(s->lock_handle, 10000);

    /* Bootloader-file chunks are scrambled with the same helper the
     * central dispatcher uses for property-bit-1 chars. Data files
     * arrive cleartext (VanMoof's PACK container is staged unencrypted
     * on flash for now). */
    uint8_t *chunk = (uint8_t *)payload;      /* OEM rewrites in-place */
    if (s->filetype == 0) {
        FUN_00024740(chunk, chunk, payload_len);
    }

    if (extflash_open() == 0) {
        monitor_log("source/oad/oad.c", 0x11A, 0, 1,
                    "failed performing flash actions, failed extflash_open");
        bleware_control_event_post(0x17);            /* flash error */
        oad_session_close();
        return -1;
    }

    /* Clamp this chunk to "what's left in the slot". */
    uint32_t remaining = (s->flash_base + s->filesize) - s->flash_cursor;
    uint32_t to_write  = (payload_len < remaining) ? payload_len : remaining;

    /* Erase the next 4 KB sector if this chunk crosses a boundary,
     * or if we're starting at a sector boundary. */
    uint32_t cursor = s->flash_cursor;
    if (((cursor + to_write) >> 12) != (cursor >> 12) ||
        (cursor & (EXTFLASH_SECTOR_SIZE - 1)) == 0) {
        uint32_t erase_addr = cursor;
        if ((erase_addr & (EXTFLASH_SECTOR_SIZE - 1)) != 0) {
            erase_addr += EXTFLASH_SECTOR_SIZE;
        }
        extflash_erase_range(erase_addr & EXTFLASH_SECTOR_MASK,
                             EXTFLASH_SECTOR_SIZE);
        cursor = s->flash_cursor;   /* re-read in case the erase moved it */
    }

    extflash_write(cursor, to_write, chunk);
    extflash_close();

    /* CRC update. For encrypted (non-bootloader) files the first 12
     * bytes of the very first chunk are NOT CRC'd — they're a
     * VanMoof PACK header that lives outside the file payload. */
    uint32_t crc_skip = 0;
    if (s->filetype != 0 && s->bytes_received < 0xD) {
        crc_skip = 0xC - s->bytes_received;
    }
    s->running_crc = crc32_le(s->running_crc,
                              chunk + crc_skip,
                              to_write - crc_skip);

    uint32_t old_cursor   = s->flash_cursor;
    s->flash_cursor      += to_write;
    s->bytes_received    += to_write;

    /* Finalise on the last byte: invert CRC, compare, notify status. */
    if ((s->flash_base + s->filesize) <= (old_cursor + to_write)) {
        s->running_crc = ~s->running_crc;
        oad_session_close();

        if (s->filetype == 0) {
            /* Bootloader: explicit CRC check + final status. */
            uint32_t final_status;
            if (s->expected_crc == s->running_crc) {
                final_status = 0x13;        /* complete + matched */
            } else {
                monitor_log("source/oad/oad.c", 0x157, 0, 2,
                            "Calculated CRC 0x%08x while expected 0x%08x",
                            s->running_crc, s->expected_crc);
                final_status = 0x16;        /* CRC mismatch */
            }
            bleware_control_event_post(final_status);
        } else if (s->filetype < 0x7C) {
            /* Data file: publish "next-block" pseudo-command with the
             * 0-based file index. (Mainware listens on this Modbus cmd
             * id and ACKs / chains the next file.) */
            uint32_t block_idx = (uint32_t)s->filetype - 1u;
            module_publish_command(0x14, &block_idx, sizeof block_idx);
        }
    }
    return 0;
}

/* Reconfigure a TI-RTOS Clock object's timeout to `timeout_ms` (converted to
 * ticks): if the clock is active, Clock_stop it, Clock_setTimeout + set its
 * period, then Clock_start it. OEM @ 0x00020098.
 *
 * NOTE: the name `oad_state_lock` and the "Semaphore_pend/HwiP" framing are a
 * mislabel — the SDK 3.40 cc26x2v2 golden ROM symbol table proves the three
 * thunks are Clock_stop/Clock_setTimeout/Clock_start (see
 * docs/rom-thunk-audit.md). The same helper is the one clock_arm/
 * advert_keepalive_pulse call. The symbol is kept for now because oad.c's
 * metadata-write path also Semaphore_post()s the `+0x04` handle it passes
 * here (oad.c:~144) — a clock-vs-semaphore contradiction that needs a Ghidra
 * re-validation of the OAD session model before this is renamed/restructured. */
int oad_state_lock(uint32_t lock_handle, int timeout_ms)
{
    extern uint32_t  *g_tick_period_ptr;     /* DAT_000200E8 = 0x0002BB88 → 10 µs */
    extern int        FUN_00027766(void);                /* guard: clock active? (arg unconfirmed) */
    extern void       FUN_0002776a(uint32_t, uint32_t);  /* Clock_setPeriod (companion) */
    extern int        thunk_FUN_1002ecc2(uint32_t, uint32_t);  /* Clock_setTimeout (0x1002ECC2) */
    extern void       thunk_FUN_1002e2c4(uint32_t);            /* Clock_stop       (0x1002E2C4) */
    extern void       thunk_FUN_1002e9e6(uint32_t);            /* Clock_start      (0x1002E9E6) */
    uint32_t           tick_us;
    uint32_t           timeout_ticks;
    int                was_active;

    was_active = FUN_00027766();              /* clock currently running? */
    if (was_active != 0) {
        thunk_FUN_1002e2c4(lock_handle);      /* Clock_stop */
    }

    tick_us       = *g_tick_period_ptr;
    timeout_ticks = (uint32_t)timeout_ms * (1000u / tick_us);

    thunk_FUN_1002ecc2(lock_handle, timeout_ticks);  /* Clock_setTimeout */
    FUN_0002776a(lock_handle, timeout_ticks);        /* Clock_setPeriod */

    if (was_active != 0) {
        thunk_FUN_1002e9e6(lock_handle);      /* Clock_start */
    }
    return 1;
}

/* Post a 1-byte control event to the bluetoothtask queue as kind 0x32.
 * The single status byte is packed into a heap envelope. OEM @ 0x000265C4. */
void bleware_control_event_post(uint32_t status)
{
    extern uint32_t g_ble_control_tag;   /* DAT_000265D8 = 0x0002752F */
    uint8_t         payload;

    payload = (uint8_t)status;
    task_queue_publish_envelope(0x32u, &payload, 1, g_ble_control_tag);
}

/* Tear down the OAD session: probes the SESSION semaphore (+0x08, 0 ms
 * timeout); if it was free, Clock_stop()s the +0x04 handle and sets
 * conn_handle to 0xFFFF (idle); then posts the SESSION semaphore.
 * OEM @ 0x00025060.
 *
 * OEM uses TWO handles: pend/post on +0x08 (session_handle, a Semaphore),
 * and 0x1002E2C4 = Clock_stop on +0x04 (NOT HwiP_disable — SDK-confirmed).
 * NOTE: the metadata-write path (above) instead treats +0x04 as a Semaphore
 * (Semaphore_post) right after the clock-reconfigure call — an unresolved
 * clock-vs-semaphore contradiction in the OAD model, flagged for a Ghidra
 * re-validation (see docs/rom-thunk-audit.md). */
void oad_session_close(void)
{
    extern struct oad_state *g_oad_conn_handle; /* DAT_00025084 → struct base */
    struct oad_state *st = g_oad_conn_handle;
    int               rc;

    rc = ti_semaphore_pend(st->session_handle, 0);   /* pend sem at +0x08 */
    if (rc == 0) {
        thunk_FUN_1002e2c4(st->lock_handle);         /* Clock_stop on +0x04 */
        st->conn_handle = 0xFFFFu;
    }
    ti_semaphore_post(st->session_handle);           /* post sem at +0x08 */
}
