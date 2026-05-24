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
/* extflash_read, extflash_erase_range, extflash_write — declared in bleware.h */
extern uint32_t log_parse_ascii_uint(const char *s, char **endptr, int base); /* FUN_000107BC */

/* TI-RTOS Semaphore_pend; ROM thunk via `*PTR_DAT_0001EA30 = tick µs`. */
extern int  ti_semaphore_pend(uint32_t handle, uint32_t timeout_ticks);

/* `FUN_000229B0(conn_handle, &len_inout)` — clamps `len_inout` to the
 * ATT MTU negotiated for `conn_handle`. (Decoded shape: writes the
 * effective MTU bytes back via the pointer.) */
/* att_mtu_clamp — declared in bleware.h */

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

/* Both `log_read_entry` and `log_seek_to_timestamp` are now real C
 * symbols defined below in this file (no longer extern stubs). */
int  log_read_entry      (void *out_16B, int log_idx);
int  log_seek_to_timestamp(uint32_t target_unix_seconds);

/* Inner SPI / RTOS helpers — extflash_* declared in bleware.h. */
extern int  extflash_open(int mode);
extern void extflash_close(void);
extern uint32_t monitor_strtol(const char *s, char **endptr, int base); /* FUN_000107BC */
extern int  ti_semaphore_pend(uint32_t handle, uint32_t timeout_ticks);
extern void ti_semaphore_post(uint32_t handle);
extern uint32_t g_ti_tick_period_us;     /* `*PTR_DAT_0000BA50` */

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

/* Convert a millisecond delay to TI-RTOS ticks. The OEM stores the
 * tick period in microseconds at `*PTR_DAT_0000BA50`, so:
 *     ticks = (ms * 1000) / tick_period_us
 * which the OEM lifts inverted as `(1000 / tick_period_us) * ms`. */
static inline uint32_t log_ms_to_ticks(uint32_t ms)
{
    return (1000u / g_ti_tick_period_us) * ms;
}

/* `log_read_entry` — fetch one 16-byte log entry by index from the
 * circular region. The index is in 16-byte units relative to the
 * current `tail_offset` (state +0xC). Wrap mask 0x1FFFF.
 *
 * Returns 1 on success, 0 if the state-struct semaphore was
 * contended (10 ms timeout). On semaphore acquired but extflash_open
 * failure, returns 1 anyway (the OEM behaviour — left to the caller
 * to notice the zero'd output buffer).
 *
 * OEM @ 0x0001E9D8.
 */
int log_read_entry(void *out_16B, int log_idx)
{
    if (ti_semaphore_pend(g_log_state.lock_handle,
                          log_ms_to_ticks(10) /* 10 ms */) != 1) {
        return 0;
    }

    uint32_t tail = g_log_state.cursor_persist_pair[1];   /* state +0xC */

    if (extflash_open(1) != 0) {
        uint32_t offset_in_region = (tail + (uint32_t)log_idx * LOG_ENTRY_SIZE) & LOG_REGION_MASK;
        extflash_read(LOG_REGION_BASE + offset_in_region, LOG_ENTRY_SIZE, out_16B);
        extflash_close();
    }

    ti_semaphore_post(g_log_state.lock_handle);
    return 1;
}

/* Per-channel log notify dispatcher — channel 0/1 map to 0x55C1/0x55C2
 * (16-byte fixed), channel 2 maps to 0x55C3 (variable up to 0xF0 bytes,
 * aligned down to 16). Copies payload to channel buffer and sends
 * GATT_Notification if conn_handle is active.
 * OEM @ 0x0001B9F4 (98 B). */
int log_gatt_notify_channel(int channel, const void *buf, uint32_t len)
{
    extern uint8_t  g_log_notify_chan_tag;     /* state_base - 4 */
    extern uint32_t g_log_notify_state[5];     /* +0:pad, +4:ch1_conn, +8:ch0_conn,
                                                  +12:ch2_conn, +16:param */
    extern void    *memcpy(void *dst, const void *src, unsigned int n);
    extern int      FUN_00016D1C(int conn, void *buf, uint32_t flag,
                                 void *param, uint16_t len, uint8_t tag,
                                 uint32_t extra);

    void    *chan_buf;
    uint16_t copy_len;
    int      conn_handle;

    if (channel == 0) {
        chan_buf    = (void *)0x2000AB00;   /* DAT_0001BA60 */
        conn_handle = (int)g_log_notify_state[2];  /* +8 */
        copy_len    = 16;
    } else if (channel == 1) {
        chan_buf    = (void *)0x2000AB10;   /* DAT_0001BA5C */
        conn_handle = (int)g_log_notify_state[1];  /* +4 */
        copy_len    = 16;
    } else if (channel == 2) {
        if (len < 0xF1) {
            copy_len = (uint16_t)len;
            if (len & 0xF) {
                copy_len = (uint16_t)(len & 0xFFFFFFF0u);
            }
        } else {
            copy_len = 0xF0;
        }
        *(uint16_t *)((uint8_t *)&g_log_notify_state[-1]) = copy_len;
        chan_buf    = (void *)0x20009E34;   /* DAT_0001BA58 */
        conn_handle = (int)g_log_notify_state[3];  /* +12 */
    } else {
        return 2;
    }

    memcpy(chan_buf, buf, copy_len);

    if (conn_handle != 0) {
        uint8_t tag = g_log_notify_chan_tag;
        FUN_00016D1C(conn_handle, chan_buf, 0,
                     (void *)g_log_notify_state[4],
                     copy_len, tag, *(uint32_t *)((uint8_t *)&g_log_notify_state[4] + 4));
    }
    return 0;
}

/* `log_seek_to_timestamp` — advance the tail cursor past every line
 * whose leading ASCII Unix-seconds timestamp is ≤ `target`.
 *
 * Walks one 256-byte window at a time from the current tail towards
 * the head (state +0x08 = head_offset, state +0x0C = tail_offset).
 * For each window, finds the first `'\n'`, parses the integer at
 * `window[newline_pos + 1]` via `monitor_strtol(.., 10)` (base 10),
 * and breaks the loop when the parsed timestamp first exceeds
 * `target`.
 *
 * After the loop (whether broken or natural-end), persists the new
 * (head, tail) pair to the 4 KB cursor sector at flash 0x03FDC000.
 * The persist sector itself is a circular log of (head, tail) pairs —
 * each pair is 8 bytes, position advances by 8 bytes per write, wraps
 * mod 0x1000 (sector size). When the position wraps to 0 the sector
 * is erased first.
 *
 * OEM @ 0x00014C68.
 */
int log_seek_to_timestamp(uint32_t target_unix_seconds)
{
    if (ti_semaphore_pend(g_log_state.lock_handle,
                          log_ms_to_ticks(10)) != 1) {
        return 0;
    }
    if (extflash_open(1) != 0) {
        uint32_t tail = g_log_state.cursor_persist_pair[1];
        uint8_t  window[256];

        while (g_log_state.cursor_persist_pair[0] /* head_offset */ != tail) {
            extflash_read(LOG_REGION_BASE + tail, sizeof window, window);

            int nl_pos = 0;
            while (nl_pos < (int)sizeof window && window[nl_pos] != '\n') {
                nl_pos++;
            }

            if ((int)(sizeof window) - nl_pos > 10) {
                /* `nl_pos` is the index of the '\n'. The next line's
                 * leading integer starts at window[nl_pos + 1]. */
                char    *endptr = NULL;
                uint32_t ts = monitor_strtol((char *)&window[nl_pos + 1],
                                             &endptr, 10);
                if (target_unix_seconds < ts) {
                    break;
                }
            }

            tail = (tail + (uint32_t)nl_pos + 1u) & LOG_REGION_MASK;
            g_log_state.cursor_persist_pair[1] = tail;
        }

        /* Persist the (head, tail) pair to the cursor sector. */
        uint32_t cursor_pos = g_log_state.cursor_persist_offset;
        if (cursor_pos == 0) {
            extflash_erase_range(LOG_CURSOR_SECTOR, 0x1000);
            cursor_pos = g_log_state.cursor_persist_offset;
        }
        extflash_write(LOG_CURSOR_SECTOR + cursor_pos, 8,
                       (const uint8_t *)g_log_state.cursor_persist_pair);
        g_log_state.cursor_persist_offset = (cursor_pos + 8) & 0xFFFu;

        extflash_close();
    }
    ti_semaphore_post(g_log_state.lock_handle);
    return 1;
}

/* Return the number of available 16-byte log blocks (head - tail
 * difference, with 0x20000 wrap). Rounds up to the next 16-byte
 * boundary. Returns 0 if the semaphore is held longer than 10 ms.
 * OEM at 0x00020338 (76 B). */
uint32_t log_block_count_get(void)
{
    if (ti_semaphore_pend(g_log_state.lock_handle,
                          log_ms_to_ticks(10)) != 1) {
        return 0;
    }

    uint32_t head = g_log_state.cursor_persist_pair[0];  /* state +0x08 */
    uint32_t tail = g_log_state.cursor_persist_pair[1];  /* state +0x0C */
    uint32_t blocks;

    if (head < tail) {
        blocks = (head - tail) + 0x20000u;
    } else {
        blocks = head - tail;
    }
    /* Round up to 16-byte blocks, plus 1 if any fractional block. */
    blocks >>= 4;
    if ((blocks & 0x0Fu) != 0) {
        blocks++;
    }

    ti_semaphore_post(g_log_state.lock_handle);
    return blocks;
}

/* Return the log total size byte — reads a single byte from the
 * global at DAT_000273e4 (RAM 0x20005B6C). Caller masks with 0xFFF
 * and shifts left 4 to get the effective byte count. OEM at 0x000273DC. */
uint8_t log_total_size_byte(void)
{
    extern uint8_t *g_log_capacity_ptr;  /* DAT_000273e4 = 0x20005B6C */
    return *g_log_capacity_ptr;
}

/* Erase the entire 128 KB log region (32 × 4 KB sectors) plus the
 * 4 KB cursor-persist sector at 0x03FDC000. Yields between each sector
 * to keep the RTOS alive. OEM @ 0x000230D8 (50 B). */
void log_region_erase(void)
{
    extern int  extflash_open(int mode);
    extern void extflash_close(void);
    /* extflash_erase_range — declared in bleware.h */
    extern void FUN_00027478(void);   /* yield / sleep helper */

    if (extflash_open(1) != 0) {
        for (int i = 0; i < 32; i++) {
            extflash_erase_range(LOG_REGION_BASE + (uint32_t)i * 0x1000u, 0x1000u);
            FUN_00027478();   /* yield between sectors */
        }
        extflash_erase_range(LOG_CURSOR_SECTOR, 0x1000u);
        extflash_close();
    }
}

/* Restore the log writer's (head, tail) cursors from the persist
 * sector at 0x03FDC000. Scans the sector forward from offset 0 in
 * 8-byte strides, looking for the last valid (head, tail) pair
 * (validity = head < 0x20000 AND tail < 0x20000). Restores that
 * pair into the log state struct. OEM @ 0x00017B24 (78 B). */
void log_writer_restart(void)
{
    extern int  extflash_open(int mode);
    extern void extflash_close(void);
    extern int  extflash_read(uint32_t addr, uint32_t len, void *out);

    if (extflash_open(1) != 0) {
        uint32_t offset = 0;
        uint32_t head = 0, tail = 0;
        uint8_t  pair[8];

        do {
            extflash_read(LOG_CURSOR_SECTOR + offset, sizeof pair, pair);
            uint32_t h = *(uint32_t *)(pair + 0);
            uint32_t t = *(uint32_t *)(pair + 4);
            if (h < LOG_REGION_SIZE && t < LOG_REGION_SIZE) {
                head = h;
                tail = t;
            }
            offset += sizeof pair;
        } while (offset < 0x1000u);

        g_log_state.cursor_persist_pair[0] = head;
        g_log_state.cursor_persist_pair[1] = tail;
        extflash_close();
    }
}
