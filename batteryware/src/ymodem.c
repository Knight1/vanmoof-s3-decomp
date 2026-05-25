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
