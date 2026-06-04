/* ota.c — serial firmware-download protocol ("WHO?" loader).
 *
 * Reconstructed from ota_process_byte (0x0800049C) and ota_send_response
 * (0x0800090C).
 *
 * The host drives the AP bank over USART1 one byte at a time. Framing:
 *
 *   Keepalive : "W" "H" "O" "?" "\r"      -> loader replies with the banner
 *   Command   : <cmd> <~cmd>              cmd = 0x11 / 0x31 (set write address)
 *                                               0x21      (verify / finalise)
 *                                         (<cmd> ^ <~cmd> == 0xFF)
 *   Argument  : b0 b1 b2 b3 x             32-bit big-endian address + XOR check
 *                                         (x == b0^b1^b2^b3); must be >= AP_BASE
 *   Data      : L  d0..dL  x              block length, L+1 payload bytes, XOR
 *                                         (x == L ^ d0 ^ .. ^ dL)
 *
 * Replies are a single byte: 0x79 'y' = ACK, 0x1F = NAK; every reply resets the
 * frame machine to idle (ota_send_response clears state/idx/cmd/addr).
 *
 * Atomic-commit trick: a data block whose target is AP_BASE (the page that
 * holds the image header + magic) is NOT programmed inline — it is stashed in
 * s_first_page and the page is only erased. The deferred header is committed by
 * the 0x21 finalise command, so an interrupted download never leaves a valid
 * magic at AP_BASE and the loader will refuse to boot the partial image.
 */
#include "bmsboot.h"

#define BLOCK_SIZE  0x80   /* 128-byte page payload per data block */

extern volatile uint8_t  g_boot_countdown;   /* 0x2000088C */
extern volatile uint8_t  g_loop_flags;       /* 0x20000850 */

/* OEM SRAM protocol state. The OEM overlaps several of these addresses across
 * phases (idx doubles as the data-block receive counter; buf doubles as the
 * 5-byte address frame and the 128-byte data block); reproduced as shared
 * fields here. */
static struct {
    uint8_t  state;        /* 0x200007F6  OTA_ST_IDLE / _ARG / _DATA           */
    uint8_t  cmd;          /* 0x200007F4  0x11 / 0x21 / 0x31                    */
    uint16_t idx;          /* 0x200005EA  handshake sub-state / arg+data index  */
    uint8_t  acc;          /* 0x200005E8  frame XOR; block length then run-XOR  */
    uint16_t total;        /* 0x200006EC  block length + 1                      */
    uint32_t addr;         /* 0x200007F0  latched write address                 */
    uint8_t  buf[BLOCK_SIZE]; /* 0x200006F0  address frame / data block         */
} s_ota;

static uint8_t s_first_page[BLOCK_SIZE];  /* 0x200005EC  held-back header page  */

/* IWDG_KR reload (the OEM kicks the watchdog through the HAL handle pointer). */
static inline void iwdg_kick(void) { REG32(IWDG_BASE) = IWDG_KR_RELOAD; }

/* ota_reset() — clear the frame machine (the same five fields ota_send_response
 * clears, without sending a byte); called by main() at startup. */
void ota_reset(void)
{
    s_ota.acc   = 0;
    s_ota.addr  = 0;
    s_ota.cmd   = 0;
    s_ota.state = OTA_ST_IDLE;
    s_ota.idx   = 0;
}

/* ota_addr() — latched write address; 0 while no transfer is in progress. */
uint32_t ota_addr(void)
{
    return s_ota.addr;
}

void ota_send_response(uint8_t code)
{
    uart_tx_byte(code);              /* 0x79 ACK or 0x1F NAK                 */
    /* return the frame machine to idle */
    s_ota.acc   = 0;                 /* 0x200005E8 */
    s_ota.addr  = 0;                 /* 0x200007F0 */
    s_ota.cmd   = 0;                 /* 0x200007F4 */
    s_ota.state = OTA_ST_IDLE;       /* 0x200007F6 */
    s_ota.idx   = 0;                 /* 0x200005EA */
}

/* ---- idle phase: keepalive handshake + command header ---- */
static void ota_idle_byte(uint8_t b)
{
    switch (s_ota.idx) {
    case 0:
        if (b == 0x0A) { s_ota.cmd = 0; s_ota.idx = 0; }          /* LF resets    */
        else if (b == 0x57 || b == 0x77) { s_ota.idx++; }         /* 'W'          */
        else if (b == OTA_CMD_A || b == OTA_CMD_VERIFY || b == OTA_CMD_ERASE) {
            s_ota.cmd = b; s_ota.idx++;                           /* command      */
        } else { s_ota.cmd = 0; s_ota.idx = 0; }
        break;
    case 1:
        if (b == 0x48 || b == 0x68) { s_ota.idx++; }              /* 'H'          */
        else if ((b == 0xEE && s_ota.cmd == OTA_CMD_A)      ||    /* <cmd> <~cmd> */
                 (b == 0xDE && s_ota.cmd == OTA_CMD_VERIFY) ||
                 (b == 0xCE && s_ota.cmd == OTA_CMD_ERASE)) {
            ota_send_response(OTA_ACK);                           /* resets state */
            s_ota.cmd   = (uint8_t)~b;                            /* 0xEE->0x11 …  */
            s_ota.state++;                                        /* -> OTA_ST_ARG */
        } else { ota_send_response(OTA_NAK); }
        break;
    case 2:
        if (b == 0x4F || b == 0x6F) s_ota.idx++; else s_ota.idx = 0;  /* 'O'      */
        break;
    case 3:
        if (b == 0x3F) s_ota.idx++; else s_ota.idx = 0;           /* '?'          */
        break;
    case 4:
        if (b == 0x0D) {                                          /* "WHO?\r"     */
            uart_tx_string(STR_BANNER_WHO);
            s_ota.acc  = 0;
            s_ota.addr = 0;
            s_ota.cmd  = 0;
        }
        s_ota.idx = 0;
        break;
    default:
        s_ota.idx = 0;
        break;
    }
}

/* ---- argument phase: collect "b0 b1 b2 b3 x" and act on the command ---- */
static void ota_arg_byte(uint8_t b)
{
    s_ota.buf[s_ota.idx++] = b;
    if (s_ota.idx <= 4)
        return;
    s_ota.idx = 0;

    s_ota.acc = (uint8_t)(s_ota.buf[0] ^ s_ota.buf[1] ^ s_ota.buf[2] ^ s_ota.buf[3]);
    if (s_ota.buf[4] != s_ota.acc) {                 /* XOR check failed         */
        ota_send_response(OTA_NAK);
        return;
    }

    uint32_t value = (uint32_t)s_ota.buf[0] << 24 | (uint32_t)s_ota.buf[1] << 16
                   | (uint32_t)s_ota.buf[2] << 8  | (uint32_t)s_ota.buf[3];

    if (value <= OTA_LO_BOUND) {                     /* below the AP bank        */
        ota_send_response(OTA_NAK);
        return;
    }

    if (s_ota.cmd == OTA_CMD_A || s_ota.cmd == OTA_CMD_ERASE) {  /* set write addr */
        ota_send_response(OTA_ACK);
        s_ota.addr = value;
        s_ota.state = OTA_ST_DATA;
        g_boot_countdown = 0;
        g_loop_flags |= 0x01u;                       /* busy: hold off boot       */
    } else if (s_ota.cmd == OTA_CMD_VERIFY) {        /* finalise: commit header   */
        if (value == APP_BASE) {
            while (flash_program_verify(value, BLOCK_SIZE, s_first_page) != 0)
                flash_erase_page(value);
            ota_send_response(OTA_ACK);
            uart_tx_flush();
            g_loop_flags &= (uint8_t)~0x01u;         /* clear busy                */
            g_loop_flags |=  0x02u;                  /* signal "upgrade finished" */
            g_boot_countdown = 0;
        } else {
            ota_send_response(OTA_NAK);
        }
    }
}

/* ---- data phase: "L d0..dL x" -> erase + program a 128-byte page ---- */
static void ota_data_byte(uint8_t b)
{
    if (s_ota.idx == 0) {                            /* first byte: block length  */
        s_ota.acc   = b;                             /* run-XOR seed = length     */
        s_ota.total = (uint16_t)(b + 1);
        s_ota.idx   = 1;
        return;
    }
    if (s_ota.total >= s_ota.idx) {                  /* still collecting payload  */
        s_ota.buf[s_ota.idx - 1] = b;
        s_ota.acc ^= b;
        s_ota.idx++;
        return;
    }

    /* trailing byte must equal the running XOR -> commit the page */
    if (b != s_ota.acc) {
        ota_send_response(OTA_NAK);
        return;
    }

    if (s_ota.addr == APP_BASE) {
        /* header page: stash it and only erase — committed later by 0x21 */
        mem_copy(s_first_page, s_ota.buf, BLOCK_SIZE);
        flash_erase_page(s_ota.addr);
    } else {
        int err;
        do {
            flash_erase_page(s_ota.addr);
            err = flash_program_verify(s_ota.addr, BLOCK_SIZE, s_ota.buf);
        } while (err != 0);
    }
    ota_send_response(OTA_ACK);
    iwdg_kick();
}

void ota_process_byte(uint8_t b)
{
    if (s_ota.state == OTA_ST_DATA) {
        ota_data_byte(b);
    } else if (s_ota.state == OTA_ST_IDLE) {
        ota_idle_byte(b);
    } else if (s_ota.state == OTA_ST_ARG) {
        ota_arg_byte(b);
    }
}
