/* ymodem.c — YModem file-transfer receiver.
 *
 * bleware uses YModem to receive firmware images, audio clips, and
 * PACK files over the debug UART. The OEM engine at 0x00008E50
 * (~400 B) is a YModem protocol state machine with 1 KB block
 * support and CAN abort handling. It validates each block ONLY by
 * the seq/complement byte ((buf[2]^0xFF)==buf[1]); the two trailing
 * wire bytes (the YModem CRC field) are read but never checked.
 *
 * Called by:
 *   cmd_audio_upload   — receives audio clips to ext-flash
 *   cmd_pack_upload    — receives PACK files to ext-flash
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* ---- Protocol constants --------------------------------------------- */

#define SOH   0x01   /* 128-byte block */
#define STX   0x02   /* 1024-byte block */
#define EOT   0x04   /* end of transfer */
#define ACK   0x06   /* acknowledge */
#define NAK   0x15   /* negative acknowledge */
#define CAN   0x18   /* cancel */
#define CRC_C 0x43   /* 'C' — request CRC-16 transfer */

#define BLOCK_SMALL  128u
#define BLOCK_LARGE  1024u
#define MAX_RETRIES  5

/* ---- State and callback types --------------------------------------- */

struct ymodem_state {
    uint32_t   dst_offset;
    uint32_t   max_size;
    uint32_t   bytes_rx;
    uint8_t    pad[0x18];
};

typedef int  (*ym_read_fn_t)(uint8_t *buf, int count, int timeout_ms);
typedef void (*ym_putc_fn_t)(uint8_t c);
typedef int  (*ym_open_cb_t)(int is_data, void *arg);

/* ---- Engine --------------------------------------------------------- */

enum {
    YM_OK      =  0,
    YM_TIMEOUT = -1,
    YM_CANCEL  = -2,
    YM_ERROR   = -3,
    YM_NOMEM   = -4,
};

int ymodem_engine_receive(ym_read_fn_t  read_fn,
                           ym_putc_fn_t  putc_fn,
                           ym_open_cb_t  open_cb,
                           struct ymodem_state *state,
                           uint8_t      *scratch)
{
    uint8_t *buf;
    int      blk_seq = 0;    /* OEM r5  — expected/next sequence number */
    int      started = 0;    /* OEM sp+0x18 — set once first block dispatched */
    int      retries = 0;    /* OEM r8  — only counted while started */
    int      blk_state;      /* OEM *DAT_00009060 — per-block size relay
                              *  (0 = EOT, 0x80/0x400 = data, -1 = CAN-CAN) */
    int      blk_size;       /* OEM r6  — current block payload size */
    uint8_t  hdr;
    int      rd;
    int      rc;

    (void)scratch;

    buf = (uint8_t *)monitor_alloc(0x406);
    if (buf == NULL) {
        return YM_NOMEM;
    }

    /* OEM @ 0x00008e78: disable the byte-count accumulator if state[0] != 0. */
    if (state != NULL && state->dst_offset != 0) {
        state = NULL;
    }

    putc_fn('C');

    /* OEM is a single dispatch loop (0x00008fa8). It reads one header byte,
     * classifies it, and — for SOH/STX (and a lone CAN) — reads the block
     * body and falls into the shared completion handler at 0x00008ec8.
     * EOT, CAN-CAN, ESC and timeouts are handled inline. There is no
     * per-block CRC: a block is accepted on the seq/complement byte alone. */
    for (;;) {
        blk_state = 0;       /* OEM @ 0x00008fb4: *DAT_00009060 = 0 */
        rd = read_fn(buf, 1, 1000);
        if (rd > 0) {
            hdr = buf[0];
            if (hdr == SOH || hdr == STX) {
                blk_size = (hdr == SOH) ? (int)BLOCK_SMALL : (int)BLOCK_LARGE;
                goto read_block;            /* OEM 0x00008fe8 */
            }
            if (hdr == EOT) {
                goto completion;            /* OEM EOT -> 0x00008ec8 (state 0) */
            }
            if (hdr == CAN) {
                /* OEM @ 0x00008e9e: read a second byte. */
                rd = read_fn(buf, 1, 1000);
                if (rd == 1) {
                    if (buf[0] != CAN) {
                        /* lone CAN: treat the byte stream as a 0-size data
                         * block (OEM sets r6=0, b 0x00008fe8). */
                        blk_size = 0;
                        goto read_block;
                    }
                    /* CAN-CAN: sentinel = -1, into completion -> return -4. */
                    blk_state = -1;
                    goto completion;
                }
                /* read failed -> timeout/retry path */
            } else if (hdr == 0x1B) {
                goto exhausted;             /* OEM ESC -> 0x0000903c */
            }
            /* fall through: unrecognised byte -> timeout/retry path */
        }
        goto timeout_retry;             /* OEM 0x0000900e */

    read_block:
        /* OEM @ 0x00008fe8: read seq + seq_cmp + data + 2 trailing (CRC) bytes.
         * The block is validated ONLY by the seq/complement byte; the two
         * trailing wire bytes are read but never checked. */
        buf[0] = hdr;
        rd = read_fn(buf + 1, blk_size + 4, 5000);
        if (rd > 0 && buf[2] == (uint8_t)(buf[1] ^ 0xFFu)) {
            blk_state = blk_size;           /* OEM *DAT_00009060 = r6 */
            goto completion;
        }
        /* bad read -> timeout/retry path */
        goto timeout_retry;

    completion:
        /* OEM @ 0x00008ec8 — shared completion handler. */
        retries = 0;                        /* OEM @ 0x00008ecc: r8 = 0 */
        if (blk_state == -1) {
            /* CAN-CAN abort: ACK then return -4 (OEM 0x00008f02). */
            putc_fn(ACK);
            monitor_free(buf);
            return YM_NOMEM;
        }
        if (blk_state == 0) {
            /* EOT: single ACK, reset sequence, loop (OEM 0x00008f92). */
            putc_fn(ACK);
            blk_seq = 0;
            continue;
        }

        if ((uint8_t)blk_seq != buf[1]) {
            /* sequence mismatch (OEM 0x00008f76) */
            if (buf[1] == 0 && (uint8_t)blk_seq == 0) {
                putc_fn(ACK);               /* restart from block 0 */
            }
            if (buf[1] != 0) {
                putc_fn(ACK);
            }
            continue;
        }

        if (blk_seq == 0) {
            /* block 0: filename + size (OEM 0x00008ef6) */
            const uint8_t *fn = buf + 3;
            if (fn[0] == 0) {
                /* empty filename — end of batch (OEM 0x00008efa: ACK, ret 0) */
                putc_fn(ACK);
                monitor_free(buf);
                return YM_OK;
            }
            rc = open_cb(0, (void *)(uintptr_t)fn);
            if (rc != 0) {
                goto cancelled;             /* OEM 0x00008f52: ret -2 */
            }
            putc_fn(ACK);
            putc_fn('C');                   /* OEM 0x00008f34: next 'C' */
        } else {
            /* data block (OEM 0x00008f3e) */
            rc = open_cb(1, (void *)(uintptr_t)(buf + 3));
            if (rc != 0) {
                goto cancelled;
            }
            /* OEM @ 0x00008f58: when the accumulator is live it advances the
             * (int*) state cursor by blk_state words (param_3 += *sentinel).
             * The reconstructed struct signature does not model that cursor;
             * the live/disabled gate is preserved via the state[0] check above. */
            putc_fn(ACK);
        }
        blk_seq++;
        started = 1;
        continue;

    timeout_retry:
        /* OEM @ 0x0000900e — timeout, bad read, or unrecognised byte. */
        if (started) {
            retries++;
            if ((retries % 20) == 0) {
                putc_fn(NAK);
                putc_fn('C');
            }
        }
        if (retries > MAX_RETRIES) {
            goto exhausted;
        }
        if (blk_seq == 0) {
            putc_fn('C');
        }
        continue;

    cancelled:
        /* open callback rejected the block (OEM 0x00008f40/0x00009040). */
        putc_fn(CAN);
        putc_fn(CAN);
        monitor_free(buf);
        return YM_CANCEL;

    exhausted:
        /* retry budget gone or ESC abort: ret -3, two CAN bytes, free. */
        putc_fn(CAN);
        putc_fn(CAN);
        monitor_free(buf);
        return YM_ERROR;
    }
}

/* ---- Public wrapper ------------------------------------------------- */

extern struct ymodem_state g_ymodem_state;

int ymodem_receive(uint32_t dst_offset, uint32_t max_size)
{
    extern void *g_ymodem_open_cb;
    extern void *g_ymodem_read_fn;
    extern void *g_ymodem_putc_fn;

    int rc;

    g_ymodem_state.dst_offset = dst_offset;
    g_ymodem_state.max_size   = max_size;

    monitor_log("source/filetransfer.c", 0x9E, NULL, 8,
                "YModem start");

    rc = ymodem_engine_receive((ym_read_fn_t)g_ymodem_read_fn,
                               (ym_putc_fn_t)g_ymodem_putc_fn,
                               (ym_open_cb_t)g_ymodem_open_cb,
                               &g_ymodem_state,
                               (uint8_t *)&g_ymodem_state.pad);

    if (rc == 0) {
        monitor_log("source/filetransfer.c", 0xAB, NULL, 8,
                    "YModem successfully received %d bytes",
                    g_ymodem_state.bytes_rx);
        return 0;
    }

    static const char * const status_strs[] = {
        NULL, "timeout", "cancel", "error", "nomem"
    };
    int idx = -rc;
    const char *s = (idx > 0 && idx <= 4) ? status_strs[idx] : "unknown";
    monitor_log("source/filetransfer.c", 0xA2, NULL, 2,
                "YModem returned with status: %s", s);
    return -1;
}
