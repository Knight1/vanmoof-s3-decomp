/* log_gatt.c — log-dispatch GATT handler.
 *
 * GATT service `0x55C0` exposes the device's circular log buffer to a
 * connected BLE client. Three characteristics:
 *
 *   0x55C1 (16 B, control)   — sub-command channel:
 *       payload[0] == 2 → seek log read-cursor past a target timestamp
 *                         (4-byte BE timestamp at payload[1..4]).
 *       payload[0] == anything else → forwarded as
 *                         `module_forward_async(0x55C1, payload[0])`
 *                         to the inter-module Modbus bus (same shape
 *                         as the generic 1-byte-forward pattern in
 *                         xs3_gatt_process_write_event).
 *       Response notified on channel 0 (16 B).
 *
 *   0x55C2 (16 B, control)   — no write path (silent return).
 *
 *   0x55C3 (up to 240 B, log readout) — paginated log dump:
 *       payload[0..3]  starting log index (BE u32, offset within the
 *                      128 KB circular buffer measured in 16-byte units)
 *       payload[4]     entry count (default 1 if zero)
 *       The handler reads up to `count` 16-byte entries from ext-flash
 *       and notifies them as one blob on channel 2.
 *
 * Log storage: a 128 KB circular region on the external SPI flash at
 * absolute address `0x03FDD000..0x03FFCFFF`. Read offsets wrap with
 * `& 0x1FFFF`. The 4 KB sector at `0x03FDC000` (immediately preceding
 * the log region) holds the persisted read-cursor.
 *
 * The log itself appears to contain ASCII text lines terminated by
 * `'\n'` — `log_seek_to_timestamp` scans 256-byte windows from the
 * persisted cursor looking for newlines and parses the leading ASCII
 * integer of each line (via `atol`-like FUN_000107BC) as a Unix
 * timestamp. The seek advances the cursor until the next line's
 * timestamp exceeds the requested target. The 16-byte fixed reads on
 * 0x55C3 are pagination units; the client reassembles to recover the
 * full ASCII lines.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "bleware.h"

/* Log-state struct in RAM at `DAT_0001EA2C = 0x200050D8`, shared with
 * `log_seek_to_timestamp`'s DAT_00014D38 (same target). 4 words +
 * scratch:
 *
 *   +0x00 u32 cursor_persist_offset   (wraps modulo 0x1000)
 *   +0x04 u32 lock_handle             (TI-RTOS Semaphore_t)
 *   +0x08 u32 cursor_persist_pair[0]  (last-known seek cursor pair —
 *                                       8 bytes written back to ext-flash
 *                                       sector at 0x03FDC000)
 *   +0x0C u32 read_offset             (current read offset within the
 *                                       128 KB circular log region) */
struct log_state {
    uint32_t cursor_persist_offset;
    uint32_t lock_handle;
    uint32_t cursor_persist_pair[2];
    /* The OEM also accesses the 8-byte block at +0x08 as a contiguous
     * region — that's the "head/tail" pair flushed to the persistence
     * sector by `log_seek_to_timestamp`. */
};

extern struct log_state g_log_state;

/* Layout constants — OEM literal-pool entries. */
#define LOG_REGION_BASE       0x03FDD000u   /* DAT_0001EA34 */
#define LOG_REGION_SIZE       0x00020000u   /* 128 KB, mask `0x1FFFF` */
#define LOG_REGION_MASK       (LOG_REGION_SIZE - 1u)
#define LOG_CURSOR_SECTOR     0x03FDC000u   /* DAT_00014D30 — 4 KB persist */
#define LOG_ENTRY_SIZE        16u

/* The three 0x55C0 notify channels — each populated by
 * log_gatt_notify_channel (OEM `0x0001B9F4`). channel 2 carries the
 * variable-length log-readout payload. */
extern int log_gatt_notify_channel(int channel, const void *buf, uint32_t len);

/* Helpers. */
extern int      extflash_open(int mode);
extern void     extflash_close(void);
extern int      extflash_read(uint32_t addr, uint32_t len, void *out);
extern void     extflash_erase_range(uint32_t addr, uint32_t len);
extern void     extflash_write(uint32_t addr, uint32_t len, const uint8_t *src);
extern uint32_t log_parse_ascii_uint(const char *s, char **endptr, int base); /* FUN_000107BC */

/* TI-RTOS Semaphore_pend; ROM thunk via `*PTR_DAT_0001EA30 = tick µs`. */
extern int  ti_semaphore_pend(uint32_t handle, uint32_t timeout_ticks);

/* `FUN_000229B0(conn_handle, &len_inout)` — clamps `len_inout` to the
 * ATT MTU negotiated for `conn_handle`. (Decoded shape: writes the
 * effective MTU bytes back via the pointer.) */
extern void att_mtu_clamp(uint32_t conn_handle, uint32_t *len_inout);

/* Module-forward (Modbus async send) — already declared in bleware.h. */

/* Log control-channel scratch (16 B at RAM `0x2000ABC0`); written
 * into by sub-command processing and notified back on channel 0. */
extern uint8_t g_log_control_scratch[16];          /* DAT_000149E4 */

/* Log readout buffer (up to 0xF0 bytes at RAM `0x20009E34`). */
extern uint8_t g_log_readout_buffer[0xF0];         /* DAT_000149E0 */

/* Single-byte field at RAM `0x20005B6C` carrying the most recent
 * "count" from a 0x55C3 readout request. The OEM uses it as a side
 * channel between the write callback and the notify finaliser. */
extern uint8_t g_log_last_count;                   /* *DAT_000149DC */

/* Read one 16-byte log entry. Wraps around the 128 KB circular log
 * region. `log_idx` is in 16-byte units relative to the current
 * `read_offset` (state[+0xC]). OEM @ `0x0001E9D8`. */
extern int  log_read_entry(void *out_16B, int log_idx);

/* Seek the persisted read-cursor past the first line whose ASCII
 * timestamp exceeds `target_unix_seconds`. Also flushes the new
 * cursor pair to ext-flash at `0x03FDC000`. OEM @ `0x00014C68`. */
extern int  log_seek_to_timestamp(uint32_t target_unix_seconds);

/* Log-service write handler — bound to service `0x55C0` from
 * xs3_gatt_process_write_event. OEM @ `0x00014910`. */
int log_gatt_write_handler(uint32_t       conn_handle,
                           int            char_idx,
                           const uint8_t *payload,
                           uint32_t       payload_len)
{
    uint32_t mtu_len = payload_len;
    att_mtu_clamp(conn_handle, &mtu_len);

    if (char_idx == 0) {
        /* === 0x55C1 control write ================================ */
        uint8_t saved_b0 = g_log_control_scratch[0];
        memset(g_log_control_scratch, 0, 16);
        g_log_control_scratch[0] = payload[0];

        if (payload[0] == 0x02) {
            /* Seek-to-timestamp sub-command. Big-endian u32 at +1. */
            uint32_t target_ts =
                ((uint32_t)payload[1] << 24) |
                ((uint32_t)payload[2] << 16) |
                ((uint32_t)payload[3] <<  8) |
                 (uint32_t)payload[4];

            log_gatt_notify_channel(0, g_log_control_scratch, 7); /* ack early */
            log_seek_to_timestamp(target_ts);

            memset(g_log_control_scratch, 0, 16);
            g_log_control_scratch[0] = saved_b0;
        } else {
            /* Generic 1-byte forward to the Modbus bus. */
            module_forward_async(0x55C1, payload[0]);
        }

        log_gatt_notify_channel(0, g_log_control_scratch, 7);
        return 0;
    }

    if (char_idx != 2) {
        /* 0x55C2 has no write path. */
        return 0;
    }

    /* === 0x55C3 paginated log readout ============================ */
    uint32_t start_idx =
        ((uint32_t)payload[0] << 24) |
        ((uint32_t)payload[1] << 16) |
        ((uint32_t)payload[2] <<  8) |
         (uint32_t)payload[3];
    uint32_t count = (uint8_t)payload[4];
    if (count == 0) {
        count = 1;
    }
    /* Cap to "what fits in the notify buffer for this MTU".
     *   buffer_max_entries = (mtu_len & 0xFFFF) >> 4   (i.e. /16) */
    uint32_t max_entries = (mtu_len & 0xFFFFu) >> 4;
    if (max_entries < count) {
        count = max_entries;
    }

    g_log_last_count = (uint8_t)count;

    uint8_t *dst = g_log_readout_buffer;
    uint32_t actual = 0;
    for (uint32_t i = 0; i < count; i++) {
        log_read_entry(dst, (int)(start_idx + i));
        dst    += LOG_ENTRY_SIZE;
        actual  = g_log_last_count;   /* OEM re-reads the count from the
                                         shared field at every iteration */
    }

    log_gatt_notify_channel(2, g_log_readout_buffer, actual * LOG_ENTRY_SIZE);
    return 0;
}
