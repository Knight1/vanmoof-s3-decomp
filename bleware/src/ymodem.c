/* ymodem.c — YModem file-transfer receiver.
 *
 * bleware uses YModem to receive firmware images, audio clips, and
 * PACK files over the debug UART. The OEM engine at 0x00008E50
 * (~400 B) is a full YModem protocol state machine with CRC-16
 * validation, 1 KB block support, and CAN abort handling.
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

/* ---- CRC-16/Modbus (same poly as crc16_modbus in crc32.c) ----------- */

static uint16_t ymodem_crc16(const uint8_t *buf, int len)
{
    uint16_t crc = 0;
    while (len--) {
        crc ^= (uint16_t)*buf++ << 8;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

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
    int      blk_seq = 0;
    int      started = 0;
    int      retries;
    int      blk_size;
    int      header;
    int      rd;
    int      rc;

    buf = (uint8_t *)monitor_alloc(0x406);
    if (buf == NULL) {
        return YM_NOMEM;
    }

    putc_fn('C');

    for (;;) {
        retries  = 0;
        blk_size = 0;

        /* wait for a block header (SOH/STX/EOT/CAN/ESC) */
        for (;;) {
            rd = read_fn(buf, 1, 1000);
            if (rd < 1) {
                /* timeout */
                if (started && retries > 0 && (retries % 20) == 0) {
                    putc_fn(NAK);
                    putc_fn('C');
                }
                if (retries >= MAX_RETRIES) {
                    monitor_free(buf);
                    return YM_TIMEOUT;
                }
                retries++;
                if (blk_size == 0) {
                    putc_fn('C');
                }
                continue;
            }

            header = buf[0];

            if (header == SOH) {
                blk_size = BLOCK_SMALL;
                break;
            }
            if (header == STX) {
                blk_size = BLOCK_LARGE;
                break;
            }
            if (header == EOT) {
                /* sender wants to end — ACK and return */
                rd = read_fn(buf, 1, 1000);
                if (rd == 1 && buf[0] == EOT) {
                    putc_fn(ACK);
                    /* second EOT ack */
                }
                putc_fn(ACK);
                monitor_free(buf);
                return YM_OK;
            }
            if (header == CAN) {
                /* sender cancelled — read second CAN if present */
                rd = read_fn(buf, 1, 1000);
                if (rd == 1 && buf[0] == CAN) {
                    /* confirmed cancel */
                }
                putc_fn(ACK);
                monitor_free(buf);
                return YM_CANCEL;
            }
            if (header == 0x1B) {
                /* ESC — abort */
                putc_fn(CAN);
                monitor_free(buf);
                return YM_CANCEL;
            }
            /* unknown byte — ignore */
        }

        /* read block number, complement, data, CRC */
        int total = blk_size + 4; /* seq + seq_cmp + data + 2-byte CRC */
        buf[0] = (uint8_t)header;
        rd = read_fn(buf + 1, total, 5000);

        if (rd < total || buf[2] != (uint8_t)(buf[1] ^ 0xFFu)) {
            /* bad block — NAK and retry */
            putc_fn(NAK);
            blk_size = -1;
            continue;
        }

        if (buf[1] == (uint8_t)blk_seq) {
            /* duplicate of the last block — ACK and skip */
            putc_fn(ACK);
            continue;
        }

        if (buf[1] != (uint8_t)((blk_seq + 1) & 0xFF)) {
            /* out-of-sequence block — ACK and ignore */
            putc_fn(ACK);
            continue;
        }

        /* verify CRC */
        uint16_t crc_calc = ymodem_crc16(buf + 3, blk_size);
        uint16_t crc_rcvd = (uint16_t)(buf[3 + blk_size] << 8)
                          | (uint16_t)(buf[3 + blk_size + 1]);
        if (crc_calc != crc_rcvd) {
            putc_fn(NAK);
            continue;
        }

        /* good block — dispatch */
        if (blk_seq == 0) {
            /* block 0: filename + size */
            const uint8_t *fn = buf + 3;
            if (fn[0] == 0) {
                /* empty filename — end of batch */
                putc_fn(ACK);
                monitor_free(buf);
                return YM_OK;
            }
            rc = open_cb(0, (void *)(uintptr_t)fn);
            if (rc != 0) {
                putc_fn(CAN);
                monitor_free(buf);
                return YM_ERROR;
            }
        } else {
            /* data block */
            rc = open_cb(1, (void *)(uintptr_t)(buf + 3));
            if (rc != 0) {
                putc_fn(CAN);
                monitor_free(buf);
                return YM_ERROR;
            }
        }

        putc_fn(ACK);
        blk_seq++;
        started = 1;
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
