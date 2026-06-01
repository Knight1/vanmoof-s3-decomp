#include "powerbankware.h"
#include <stdarg.h>

/*
 * UART TX ring + log formatter.
 *
 *   uart_putchar = OEM FUN_0801663c   (ring enqueue)
 *   uart_tx_isr  = OEM FUN_080167f0   (ring -> TDR, TXEIE)
 *   uart_flush   = OEM FUN_08016898   (block until the ring drains)
 *   log_print    = OEM FUN_08012fa8   (printf-like; channel arg vestigial)
 *
 * Cross-checked against batteryware uart.c (same ring model: wr/rd indices,
 * a TX-busy state byte, putchar/tx_isr/tx_flush). Differences are MCU-level:
 * the F0 USART TDR is at Instance+0x28 and TXEIE is CR1 bit 7, and the ring is
 * 0x1000 bytes at 0x20000a60 with a busy byte at handle+0x69 of the HAL UART
 * handle (0x20001a60).
 *
 * The OEM threads a "channel" through every helper (FUN_08012372 etc.) but the
 * putchar it resolves to ignores it and always targets the one ring, so the
 * channel is dropped here.
 */

#define UART_RING      ((volatile uint8_t *)0x20000a60)
#define UART_RING_WRAP 0x0fffu                 /* index wraps past 0xFFF (0x1000 entries) */

static volatile uint16_t * const s_wr_idx = (volatile uint16_t *)0x20000a5c;
static volatile uint16_t * const s_rd_idx = (volatile uint16_t *)0x20000854;
static volatile uint8_t  * const s_handle = (volatile uint8_t  *)0x20001a60;
#define UART_STATE    (*(volatile uint8_t *)(s_handle + 0x69))   /* TX: 0=off ' '=idle '!'=busy */
#define UART_STATE_RX (*(volatile uint8_t *)(s_handle + 0x6a))   /* RX: ' '=active */

/* Ring enqueue (FUN_0801663c). No enabled-guard in the OEM. */
void uart_putchar(uint8_t c)
{
    UART_RING[*s_wr_idx] = c;
    uint16_t next = (uint16_t)(*s_wr_idx + 1);
    *s_wr_idx = next;
    if (UART_RING_WRAP < next) {
        *s_wr_idx = 0;
    }
}

/* TX drain (FUN_080167f0): when the line is idle and the ring is non-empty,
 * pop one byte, mark busy, write it to the USART TDR and enable TXEIE. */
void uart_tx_isr(void)
{
    if (UART_STATE == ' ') {
        if (*s_rd_idx != *s_wr_idx) {
            uint8_t c = UART_RING[*s_rd_idx];
            UART_STATE = '!';
            uint16_t next = (uint16_t)(*s_rd_idx + 1);
            *s_rd_idx = next;
            if (UART_RING_WRAP < next) {
                *s_rd_idx = 0;
            }
            volatile uint32_t *usart = *(volatile uint32_t **)(s_handle + 0);
            *(volatile uint16_t *)((uint8_t *)usart + 0x28) = c;   /* TDR */
            *usart |= 0x80u;                                       /* CR1 TXEIE */
        }
    } else if (UART_STATE == 0) {
        *s_wr_idx = 0;
        *s_rd_idx = 0;
    }
}

/* Block until the ring has fully drained (FUN_08016898). */
void uart_flush(void)
{
    if (UART_STATE != 0) {
        uart_tx_isr();
        while (UART_STATE != ' ') {
            /* spin until the TX line returns to idle */
        }
    }
}

/* 0..15 -> '0'..'9','A'..'F' (FUN_08013640). */
char nibble_to_hex(uint8_t nibble)
{
    return (char)(nibble < 10 ? nibble + '0' : nibble + 0x37);
}

/* Null-terminated string -> ring (FUN_08012354 -> FUN_080165e4). */
void uart_puts(const char *str)
{
    for (const char *p = str; *p != '\0'; p++) {
        uart_putchar((uint8_t)*p);
    }
}

/* Big-endian hex emitters (FUN_08012396 / 080123fc / 080124b6). */
static void put_hex8(uint8_t v)
{
    uart_putchar((uint8_t)nibble_to_hex(v >> 4));
    uart_putchar((uint8_t)nibble_to_hex(v & 0xf));
}

static void put_hex16(uint16_t v)
{
    put_hex8((uint8_t)(v >> 8));
    put_hex8((uint8_t)v);
}

/*
 * Unsigned decimal (FUN_080125fc is a 64-bit, optionally zero-padded printer;
 * every log call uses the high word = 0 and width = 0, so a plain 32-bit
 * decimal with no leading zeros is behaviour-identical for the values seen).
 */
static void put_dec(uint32_t v)
{
    char buf[10];
    int n = 0;
    do {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    while (n > 0) {
        uart_putchar((uint8_t)buf[--n]);
    }
}

/*
 * log_print (FUN_08012fa8) — printf-like formatter over the TX ring.
 *
 * The OEM dispatches each '%' specifier through a jump table whose handlers
 * fetch the next 4-byte vararg slot and continue the format loop. The
 * conversions used by the firmware:
 *   %d  u8  -> decimal      %i  u16 -> decimal      %l  u32 -> decimal
 *   %x  u8  -> 2 hex        %w  u16 -> 4 hex        %s  -> string
 * (upper-case variants alias the same handlers). Any other specifier is
 * emitted literally as "%<c>".
 */
void log_print(uint8_t channel, const char *fmt, ...)
{
    va_list ap;
    (void)channel;                       /* vestigial — putchar ignores it */
    va_start(ap, fmt);

    for (const char *p = fmt; *p != '\0'; p++) {
        if (*p != '%') {
            uart_putchar((uint8_t)*p);
            continue;
        }
        p++;
        switch (*p) {
        case 'd': case 'D': put_dec((uint8_t)va_arg(ap, int));      break;
        case 'i': case 'I': put_dec((uint16_t)va_arg(ap, int));     break;
        case 'l': case 'L': put_dec(va_arg(ap, uint32_t));          break;
        case 'x': case 'X': put_hex8((uint8_t)va_arg(ap, int));     break;
        case 'w': case 'W': put_hex16((uint16_t)va_arg(ap, int));   break;
        case 's': case 'S': uart_puts(va_arg(ap, const char *));    break;
        case '\0': va_end(ap); return;
        default:
            uart_putchar('%');
            uart_putchar((uint8_t)*p);
            break;
        }
    }
    va_end(ap);
}

/*
 * uart_rx_handler — OEM FUN_08016688. Polled from the main loop and the
 * service delay; drains the USART RX ring (512 B @ 0x20000858, wr 0x20000a58
 * / rd 0x20000a5a) and routes each byte.
 *
 * The mode word (0x200006a0) selects text-console vs binary mode, exactly as
 * in batteryware's uart_resp_handler: text mode when (bit0 clear AND bit1
 * clear) OR bit2 set; otherwise the byte goes to the binary/OTA handler. When
 * the RX channel is disabled the ring + line buffer are reset.
 *
 * In text mode each byte feeds *both* the Modbus frame processor
 * (modbus_process = FUN_080168c4) and the line buffer dispatched to the
 * command parser on CR. modem_rx_byte (FUN_0800b518) is the binary/OTA path.
 */
extern void modem_rx_byte(uint8_t channel, uint8_t c);        /* FUN_0800b518 (binary/OTA) */
/* modbus_process / cmd_dispatch are declared in the header. */

void uart_rx_handler(void)
{
    volatile uint16_t * const rx_wr   = (volatile uint16_t *)0x20000a58;
    volatile uint16_t * const rx_rd   = (volatile uint16_t *)0x20000a5a;
    uint8_t           * const rx_ring = (uint8_t *)0x20000858;
    volatile uint16_t * const mode    = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const line_len = (volatile uint16_t *)0x20000a5e;
    uint8_t           * const line_buf = (uint8_t *)0x20001ad0;

    if (UART_STATE_RX == ' ') {
        while (*rx_wr != *rx_rd) {
            uint8_t  b = rx_ring[*rx_rd];
            uint16_t next = (uint16_t)(*rx_rd + 1);
            *rx_rd = next;
            if (next > 0x1ff) {
                *rx_rd = 0;
            }

            if (((*mode & 1) == 0 && (*mode & 2) == 0) || (*mode & 4) != 0) {
                modbus_process(2, b);
                if ((*mode & 4) != 0) {
                    *line_len = 0;
                } else if (b == 0xd) {
                    if (*line_len != 0) {
                        cmd_dispatch(2, line_buf, (uint8_t)*line_len);
                    }
                    *line_len = 0;
                } else if (b < 0x20 || (int8_t)b < 0) {
                    *line_len = 0;
                } else {
                    line_buf[*line_len] = b;
                    uint16_t li = (uint16_t)(*line_len + 1);
                    *line_len = li;
                    if (li > 0x2c) {
                        *line_len = 0;
                    }
                }
            } else {
                modem_rx_byte(2, b);
            }
        }
    } else if (UART_STATE == 0) {
        *rx_wr = 0;
        *rx_rd = 0;
        *line_len = 0;
    }
}
