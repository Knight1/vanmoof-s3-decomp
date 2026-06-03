/* ota.c — serial firmware-download protocol ("Who?" loader).
 *
 * Reconstructed from ota_process_byte (0x08000220), ota_send_response
 * (0x0800069C) and the state reset performed by boot_main.
 *
 * The host drives the AP bank over USART2 one byte at a time. Framing:
 *
 *   Keepalive : "W" "h" "o" "?" "\r"      -> loader replies with the banner
 *   Command   : <cmd> <~cmd>              cmd = 0x31 '1' (erase/prepare)
 *                                               0x21 '!' (verify/finalise)
 *   Argument  : b0 b1 b2 b3 x             32-bit big-endian value + XOR check
 *                                         (x == b0^b1^b2^b3); value must lie in
 *                                         0x08008000 .. 0x08023FFF (the AP bank)
 *   Data      : L  d0..d(L-1)  x          block length, payload, XOR check
 *
 * Replies are a single byte: 0x79 'y' = ACK, 0x1F = NAK. A 0x31 command latches
 * the write address, erases the target page if it is page-aligned, and enters
 * the data phase; arriving blocks are assembled into a 2 KB page buffer and
 * programmed. A 0x21 command whose value matches the expected end address
 * finalises the transfer (sets the "upgrade finished" mode bit the server loop
 * watches, which then mirrors AP into Shadow).
 */
#include "powerbankboot.h"

#define OTA_LO_BOUND   (AP_BASE - 1u)   /* 0x08007FFF: value must be >  this    */
#define OTA_HI_BOUND   0x08023FFFu       /* value must be <= this (AP bank end)  */

extern uint32_t g_ota_end_addr;          /* expected finalise address (0x21 cmd) */

static struct {
    uint8_t  state;                      /* OTA_ST_IDLE / _ARG / _DATA           */
    uint8_t  cmd;                        /* 0x31 erase, 0x21 verify              */
    uint16_t idx;                        /* byte index within the current phase  */
    uint8_t  arg[5];                     /* 4-byte value + XOR                   */
    uint32_t addr;                       /* latched target address               */
    uint16_t blk_len;                    /* current data-block length            */
    uint16_t blk_recv;                   /* bytes received in the block (+1)      */
    uint8_t  blk_xor;                    /* running XOR over the block           */
    uint8_t  data[256];                  /* assembled data block                 */
    uint8_t  page[FLASH_PAGE_SIZE];      /* 2 KB page assembly buffer            */
} s_ota;

void ota_reset(void)
{
    s_ota.state = OTA_ST_IDLE;
    s_ota.cmd   = 0;
    s_ota.idx   = 0;
    s_ota.addr  = 0;
}

void ota_send_response(uint8_t code)
{
    uart_tx_byte(code);                  /* 0x79 ACK or 0x1F NAK                 */
    /* drop any partially-accumulated frame state */
    s_ota.idx      = 0;
    s_ota.blk_recv = 0;
    s_ota.blk_len  = 0;
    s_ota.blk_xor  = 0;
    s_ota.arg[0]   = 0;
}

/* ---- argument phase: collect "b0 b1 b2 b3 x" and act on the command ---- */
static void ota_arg_byte(uint8_t b)
{
    s_ota.arg[s_ota.idx++] = b;
    if (s_ota.idx <= 4)
        return;
    s_ota.idx = 0;

    uint8_t x = (uint8_t)(s_ota.arg[0] ^ s_ota.arg[1] ^ s_ota.arg[2] ^ s_ota.arg[3]);
    if (s_ota.arg[4] != x) {             /* XOR check failed                     */
        ota_send_response(OTA_NAK);
        return;
    }

    uint32_t value = (uint32_t)s_ota.arg[0] << 24 | (uint32_t)s_ota.arg[1] << 16
                   | (uint32_t)s_ota.arg[2] << 8  | (uint32_t)s_ota.arg[3];

    if (value <= OTA_LO_BOUND || value > OTA_HI_BOUND) {  /* outside AP bank     */
        ota_send_response(OTA_NAK);
        return;
    }

    if (s_ota.cmd == OTA_CMD_ERASE) {            /* '1' — prepare write address  */
        ota_send_response(OTA_ACK);
        s_ota.addr  = value;
        s_ota.state = OTA_ST_DATA;
        s_ota.blk_recv = 0;
        g_loop_mode |= 0x01u;                    /* busy: hold off the watchdog   */
        if ((value & (FLASH_PAGE_SIZE - 1)) == 0)/* page-aligned start -> erase   */
            flash_erase_page(value);
    } else if (s_ota.cmd == OTA_CMD_VERIFY) {    /* '!' — finalise the transfer   */
        if (value == g_ota_end_addr) {
            ota_send_response(OTA_ACK);
            uart_tx_flush();
            g_loop_mode &= (uint8_t)~0x01u;      /* clear busy                    */
            g_loop_mode |=  0x02u;               /* signal "upgrade finished"     */
        } else {
            ota_send_response(OTA_NAK);
        }
    }
}

/* ---- data phase: "L d0..d(L-1) x" -> assemble + program a page ---- */
static void ota_data_byte(uint8_t b)
{
    if (s_ota.blk_recv == 0) {                   /* first byte: block length      */
        s_ota.blk_len  = b;
        s_ota.blk_xor  = (uint8_t)(b + 1);
        s_ota.blk_recv = 1;
        return;
    }
    if (s_ota.blk_recv < (uint16_t)(s_ota.blk_len + 1)) {
        s_ota.data[s_ota.blk_recv - 1] = b;      /* payload byte                  */
        s_ota.blk_xor ^= b;
        s_ota.blk_recv++;
        return;
    }
    /* trailing byte == running XOR -> commit the block */
    if (b != s_ota.blk_xor) {
        ota_send_response(OTA_NAK);
        return;
    }

    /* assemble into the page buffer at the in-page offset, then program the
     * 256-byte-aligned slice. On a program error, erase the whole 2 KB page and
     * re-program it from the accumulated page buffer. The IWDG is kicked around
     * every flash op so a slow erase cannot trip the watchdog. */
    mem_copy(&s_ota.page[s_ota.addr & (FLASH_PAGE_SIZE - 1)], s_ota.data,
             (int16_t)s_ota.blk_len);
    iwdg_refresh();
    int err = flash_program((uint32_t *)(s_ota.addr & 0xFFFFFF00u),
                            s_ota.blk_len, s_ota.data);
    while (err != 0) {
        iwdg_refresh();
        flash_erase_page(s_ota.addr & ~(FLASH_PAGE_SIZE - 1));
        err = flash_program((uint32_t *)(s_ota.addr & ~(FLASH_PAGE_SIZE - 1)),
                            (uint16_t)(s_ota.blk_len + (s_ota.addr & (FLASH_PAGE_SIZE - 1))),
                            s_ota.page);
    }
    ota_send_response(OTA_ACK);
    iwdg_refresh();
}

/* ---- idle phase: keepalive handshake + command header ---- */
static void ota_idle_byte(uint8_t b)
{
    switch (s_ota.idx) {
    case 0:
        if (b == 0x0A) { s_ota.cmd = 0; s_ota.idx = 0; }          /* LF resets    */
        else if (b == 'W') { s_ota.idx = 1; }                     /* "Who?"        */
        else if (b == OTA_CMD_VERIFY || b == OTA_CMD_ERASE) {     /* command       */
            s_ota.cmd = b; s_ota.idx = 1;
        } else { s_ota.cmd = 0; s_ota.idx = 0; }
        break;
    case 1:
        if (b == 'h') { s_ota.idx = 2; }                          /* "W h ..."     */
        else if ((b == 0xDE && s_ota.cmd == OTA_CMD_VERIFY) ||    /* <cmd> <~cmd>  */
                 (b == 0xCE && s_ota.cmd == OTA_CMD_ERASE)) {
            ota_send_response(OTA_ACK);
            s_ota.cmd   = (uint8_t)~b;                            /* 0xDE->0x21 …  */
            s_ota.state = OTA_ST_ARG;
        } else { ota_send_response(OTA_NAK); }
        break;
    case 2:
        if (b == 'o') s_ota.idx = 3; else s_ota.idx = 0;
        break;
    case 3:
        if (b == '?') s_ota.idx = 4; else s_ota.idx = 0;
        break;
    case 4:
        if (b == '\r')                                            /* "Who?\r"      */
            uart_tx_string(STR_BANNER);
        s_ota.idx = 0;
        break;
    default:
        s_ota.idx = 0;
        break;
    }
}

void ota_process_byte(uint8_t b)
{
    switch (s_ota.state) {
    case OTA_ST_ARG:  ota_arg_byte(b);  break;
    case OTA_ST_DATA: ota_data_byte(b); break;
    case OTA_ST_IDLE:
    default:          ota_idle_byte(b); break;
    }
}
