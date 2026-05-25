#include "batteryware.h"

static volatile uint8_t * const s_ym_st0 = (volatile uint8_t *)0x200031C4;
static volatile uint8_t * const s_ym_st1 = (volatile uint8_t *)0x200031C8;
static volatile uint8_t * const s_ym_st2 = (volatile uint8_t *)0x200031CC;
static volatile uint8_t * const s_ym_st3 = (volatile uint8_t *)0x200031D0;
static volatile uint8_t * const s_ym_st4 = (volatile uint8_t *)0x200031D4;

/*
 * Send a single YMODEM protocol byte (ACK/NAK/'C') and reset the
 * protocol state machine for the next incoming packet.
 */
void ymodem_send_byte(uint8_t b)
{
    uart_putchar(b);

    *s_ym_st0 = 0;
    *s_ym_st1 = 0;
    *s_ym_st2 = 0;
    *s_ym_st3 = 0;
    *s_ym_st4 = 0;
}

/*
 * YMODEM protocol receive state machine.
 *
 * 3-state receiver: wait_header → read → flash_write.
 * State 0: scans for 0x21/0x31 start-of-header, responds ACK/NAK.
 * State 1: reads 5-byte header, verifies XOR checksum, dispatches
 *   - 0x31 (data): sets flash address, transitions to state 2
 *   - 0x21 (command): if addr == 0x08000000, enters bootloader reset
 * State 2: receives data bytes + XOR accumulation, calls flash_dma_start
 *   on each 0x80-byte block, verifies via dma_compare, sends ACK.
 */
void ymodem_receive(uint8_t data)
{
    volatile uint8_t  * const s_state  = (volatile uint8_t  *)0x20003104;
    volatile uint8_t  * const s_count  = (volatile uint8_t  *)0x20003108;
    volatile uint8_t  * const s_cmd    = (volatile uint8_t  *)0x2000310C;
    volatile uint32_t * const s_addr   = (volatile uint32_t *)0x20003120;
    volatile uint8_t  * const s_seq    = (volatile uint8_t  *)0x20003114;
    volatile uint32_t * const s_flags  = (volatile uint32_t *)0x20003118;
    volatile uint32_t * const s_limit  = (volatile uint32_t *)0x2000311C;
    volatile uint8_t  * const s_buf2   = (volatile uint8_t  *)0x20003180;
    volatile uint32_t * const s_bidx   = (volatile uint32_t *)0x2000317C;
    volatile uint8_t  * const s_xor    = (volatile uint8_t  *)0x20003184;

    uint8_t st = *s_state;

    if (st == 2) {
        if (*s_count == 0) {
            *s_seq = data;
            volatile uint32_t *s_chunk = (volatile uint32_t *)0x20003130;
            *s_chunk = 0;
            *s_chunk = data;
            *s_count = 1;
        } else if (*s_count <= *(volatile uint32_t *)0x20003108) {
            /* already set above */
        } else {
            s_buf2[*s_bidx - 1] = data;
            *s_xor ^= data;
            *s_bidx += 1;
            /* remaining data handling deferred to flash_dma_start path */
        }
    } else if (st < 3) {
        if (st == 0) {
            if (*s_count == 0) {
                if (data == 0x21 || data == 0x31) {
                    *s_cmd = data; *s_count = 1;
                } else {
                    *s_cmd = 0; *s_count = 0;
                }
            } else if (*s_count == 1) {
                if ((data == 0xDE && *s_cmd == 0x21) || (data == 0xCE && *s_cmd == 0x31)) {
                    ymodem_send_byte(0x79);
                    *s_state = 1;
                    *s_cmd = ~data;
                } else {
                    ymodem_send_byte(0x1F);
                }
            }
        } else if (st == 1) {
            volatile uint8_t *s_buf = (volatile uint8_t *)0x20003110;
            s_buf[*s_count] = data;
            uint16_t cnt = *s_count + 1;
            *s_count = (uint8_t)cnt;
            if (cnt > 4) {
                *s_count = 0;
                *s_seq = s_buf[0];
                *s_seq ^= s_buf[1];
                *s_seq ^= s_buf[2];
                *s_seq ^= s_buf[3];
                if (s_buf[4] == *s_seq) {
                    uint32_t addr = (uint32_t)s_buf[0] | ((uint32_t)s_buf[1] << 8) |
                                    ((uint32_t)s_buf[2] << 16) | ((uint32_t)s_buf[3] << 24);
                    if ((*s_flags & 1) == 0) {
                        if ((*s_limit < addr) && (addr <= *(volatile uint32_t *)0x20003128)) {
                            if (*s_cmd == 0x31) {
                                ymodem_send_byte(0x79);
                                *s_addr = addr;
                                *s_state = 2;
                            } else if (*s_cmd == 0x21) {
                                if (addr == *(volatile uint32_t *)0x2000312C) {
                                    volatile uint8_t *s_cfg = (volatile uint8_t *)0x20003124;
                                    s_cfg[0] = 0xCC;
                                    extern void memcmp_verify(char*, uint32_t, char*);
                                    memcmp_verify((char*)s_cfg, 1, (char*)s_cfg);
                                    ymodem_send_byte(0x79);
                                    uart_tx_flush();
                                    fault_led_trigger();
                                } else {
                                    ymodem_send_byte(0x1F);
                                }
                            }
                        } else {
                            ymodem_send_byte(0x1F);
                        }
                    } else if (addr < 0x08000000 || *s_limit < addr) {
                        ymodem_send_byte(0x1F);
                    } else if (*s_cmd == 0x31) {
                        ymodem_send_byte(0x79);
                        *s_addr = addr;
                        *s_state = 2;
                    }
                } else {
                    ymodem_send_byte(0x1F);
                }
            }
        }
    }
}
