/* ymodem.c — YModem file-transfer receiver.
 *
 * bleware uses YModem to receive firmware images, audio clips, and
 * PACK files over the debug UART. The OEM engine at 0x00008E50
 * (~400 B) is a full YModem protocol state machine with CRC-16
 * validation, 1 KB block support, and CAN abort handling.
 *
 * This file provides the outer `ymodem_receive` wrapper (OEM
 * 0x000101B0, 80 B) that initialises the transfer state, calls
 * the engine, and logs the result. The engine itself
 * (ymodem_engine_receive) is a ROM-like thunk — in the OEM it's a
 * direct call; here it's a forward declaration that will resolve
 * when the YModem engine body lands.
 *
 * Called by:
 *   cmd_audio_upload   — receives audio clips to ext-flash
 *   cmd_pack_upload    — receives PACK files to ext-flash
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* YModem transfer state struct — OEM at RAM 0x2000A630 (DAT_000102C8).
 * Written by ymodem_receive before launching the engine. */
struct ymodem_state {
    uint32_t   dst_offset;    /* +0x00 — ext-flash destination */
    uint32_t   max_size;      /* +0x04 — max bytes to accept */
    uint32_t   bytes_rx;      /* +0x08 — received byte count (written by engine) */
    uint8_t    pad0c[0x18];   /* +0x0C — engine scratch */
};

extern struct ymodem_state g_ymodem_state;   /* DAT_000102C8 = 0x2000A630 */

/* YModem engine — full protocol state machine. Defined elsewhere;
 * for now, declared extern (will link when the engine body lands). */
extern int ymodem_engine_receive(void *callbacks, void *open_cb,
                                 uint8_t *scratch);

/* Callback table entries — OEM literal pool at 0x000102CC..0x000102DC */
extern void *g_ymodem_open_cb;     /* PTR_LAB_00019b10 = open/write callback */
extern void *g_ymodem_read_fn;     /* PTR_FUN_00027014 = UART read byte */
extern void *g_ymodem_flush_fn;    /* PTR_FUN_0002678e = UART flush */
extern void *g_ymodem_putc_fn;     /* PTR_LAB_00027752 = UART write byte */
extern void *g_ymodem_send_fn;     /* PTR_LAB_0002774e = UART send block */

/* Status strings for YModem return codes — OEM flash 0x000102E4 table */
static const char * const s_ymodem_status[] = {
    NULL,          /*  0 = success (no string) */
    "timeout",     /* -1 */
    "cancel",      /* -2 */
    "error",       /* -3 */
    "nomem",       /* -4 */
};

/* Receive a file via YModem into ext-flash at `dst_offset`, capped at
 * `max_size` bytes. Returns 0 on success, -1 on failure.
 * OEM @ 0x000101B0 (80 B). */
int ymodem_receive(uint32_t dst_offset, uint32_t max_size)
{
    int rc;

    g_ymodem_state.dst_offset = dst_offset;
    g_ymodem_state.max_size   = max_size;

    /* OEM calls FUN_00027542(500) — likely a 500 ms delay before starting */
    /* OEM calls FUN_000255D4() — likely a UART flush / drain */

    monitor_log("source/filetransfer.c", 0x9E, NULL, 8,
                "YModem start");

    rc = ymodem_engine_receive(&g_ymodem_state, g_ymodem_open_cb,
                               (uint8_t *)&g_ymodem_state.pad0c);

    if (rc == 0) {
        /* OEM calls FUN_00027542(500) — 500 ms post-receive delay */
        monitor_log("source/filetransfer.c", 0xAB, NULL, 8,
                    "YModem successfully received %d bytes",
                    g_ymodem_state.bytes_rx);
        return 0;
    }

    /* Map negative return codes through the status table */
    int idx = -rc;
    const char *status_str = "unknown";
    if (idx > 0 && idx <= 4) {
        status_str = s_ymodem_status[idx];
    }

    monitor_log("source/filetransfer.c", 0xA2, NULL, 2,
                "YModem returned with status: %s", status_str);

    /* OEM calls FUN_00027542(500) — delay */
    /* OEM calls FUN_000255D4() — UART flush */
    return -1;
}

/* ---- Data globals (weak — overridden when full engine lands) ------- */

__attribute__((weak))
struct ymodem_state g_ymodem_state;

__attribute__((weak)) void *g_ymodem_open_cb;
__attribute__((weak)) void *g_ymodem_read_fn;
__attribute__((weak)) void *g_ymodem_flush_fn;
__attribute__((weak)) void *g_ymodem_putc_fn;
__attribute__((weak)) void *g_ymodem_send_fn;

/* ymodem_engine_receive — weak stub until the full engine is decoded.
 * The real engine at OEM 0x00008E50 is ~400 B of protocol state
 * machine. This stub just returns -3 (error). */
__attribute__((weak))
int ymodem_engine_receive(void *callbacks, void *open_cb, uint8_t *scratch)
{
    (void)callbacks; (void)open_cb; (void)scratch;
    return -3;
}
