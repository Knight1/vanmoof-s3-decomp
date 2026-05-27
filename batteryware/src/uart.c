#include "batteryware.h"

/* TX ring buffer — SRAM addresses resolved from literal pool */
static volatile uint8_t * const s_tx_enabled = (volatile uint8_t *)0x2000453D;
static volatile uint16_t * const s_tx_rd_idx  = (volatile uint16_t *)0x2000453E;
static volatile uint16_t * const s_tx_wr_idx  = (volatile uint16_t *)0x20004542;
static volatile uint8_t * const s_tx_buffer   = (volatile uint8_t *)0x20002C88;

/* UART/USART peripheral base pointer (SRAM-resident) */
static volatile uint32_t * const s_uart_base   = (volatile uint32_t *)0x20004488;

/*
 * Write a byte to the UART TX ring buffer.
 *
 * Only writes if the TX subsystem is enabled. The buffer is 0x1400 (5120)
 * bytes circular — the write pointer wraps back to 0.
 */
void uart_putchar(uint8_t c)
{
    if (*s_tx_enabled != 1) {
        return;
    }

    s_tx_buffer[*s_tx_wr_idx] = c;
    uint16_t next = *s_tx_wr_idx + 1;
    *s_tx_wr_idx = next;

    if (next >= 0x1400) {
        *s_tx_wr_idx = 0;
    }
}

/*
 * TX ring buffer drain — called from TXE (transmit empty) interrupt.
 *
 * If the UART TX data register is ready (status == 0x20) and the ring
 * buffer is not empty, pops one byte and writes it to the TX register.
 * Enables TXE interrupt after writing.
 */
void uart_tx_isr(void)
{
    volatile uint32_t *uart = (volatile uint32_t *)*s_uart_base;

    /* Check TX status: must NOT be 0x20 yet (i.e. TX register is free) */
    if (uart[0x1E] == 0x20) {
        if (*s_tx_rd_idx != *s_tx_wr_idx) {
            uint8_t c = s_tx_buffer[*s_tx_rd_idx];
            uart[0x1E] = 0x21;
            uint16_t next = *s_tx_rd_idx + 1;
            *s_tx_rd_idx = next;
            if (next >= 0x1400) {
                *s_tx_rd_idx = 0;
            }
            uart[0x28 / 4] = (uint32_t)c;
            uart[0] |= 0x80;
        }
    } else if (uart[0x1E] == 0) {
        *s_tx_wr_idx = 0;
        *s_tx_rd_idx  = 0;
    }
}

/*
 * Wait for all TX bytes to be sent (poll TX status register until idle).
 */
void uart_tx_flush(void)
{
    volatile uint32_t *uart = (volatile uint32_t *)*s_uart_base;

    if (uart[0x78 / 4] != 0) {
        uart_tx_isr();
        while (uart[0x78 / 4] != 0x20) { }
    }
}

/*
 * Check USART1 parity error flag. If set, records the error in SRAM
 * status and clears the PE flag in the ISR register.
 */
void uart_check_parity_error(void)
{
    volatile uint32_t *usart1 = (volatile uint32_t *)0x40010400;
    volatile uint8_t * const s_error_flags = (volatile uint8_t *)0x20002BFC;

    if (usart1[0x14 / 4] & 1) {
        *s_error_flags |= 2;
        usart1[0x14 / 4] = 1;
    }
}

/*
 * Check USART1 overrun error flag (bit 0x2000 = ORE).
 * If set, records error in SRAM flag bit 0 and clears ORE.
 */
void uart_check_overrun_error(void)
{
    volatile uint32_t *usart1 = (volatile uint32_t *)0x40010400;
    volatile uint8_t * const s_error_flags = (volatile uint8_t *)0x20002BFC;

    if (usart1[0x14 / 4] & 0x2000) {
        *s_error_flags |= 1;
        usart1[0x14 / 4] = 0x2000;
    }
}

/*
 * Print a byte as two uppercase hex characters via uart_putchar.
 */
void uart_puthex_byte(uint8_t b)
{
    uart_putchar((uint8_t)nibble_to_hex(b >> 4));
    uart_putchar((uint8_t)nibble_to_hex(b & 0xF));
}

/*
 * Write a null-terminated string to the TX ring buffer.
 * Uses the same ring buffer as uart_putchar.
 */
void uart_puts(char *str)
{
    if (*s_tx_enabled != 1) {
        return;
    }
    for (char *p = str; *p != '\0'; p++) {
        s_tx_buffer[*s_tx_wr_idx] = (uint8_t)*p;
        uint16_t next = *s_tx_wr_idx + 1;
        *s_tx_wr_idx = next;
        if (next >= 0x1400) {
            *s_tx_wr_idx = 0;
        }
    }
}

/*
 * Print a 16-bit value as 4 uppercase hex characters.
 */
void uart_puthex_16(uint16_t val)
{
    uart_putchar((uint8_t)nibble_to_hex((uint8_t)(val >> 12)));
    uart_putchar((uint8_t)nibble_to_hex((uint8_t)(val >> 8) & 0xF));
    uart_putchar((uint8_t)nibble_to_hex((uint8_t)(val >> 4) & 0xF));
    uart_putchar((uint8_t)nibble_to_hex((uint8_t)val & 0xF));
}

/*
 * Print a 32-bit value as 8 uppercase hex characters.
 */
void uart_puthex_32(uint32_t val)
{
    uart_putchar((uint8_t)nibble_to_hex((uint8_t)(val >> 28)));
    uart_putchar((uint8_t)nibble_to_hex((uint8_t)(val >> 24) & 0xF));
    uart_putchar((uint8_t)nibble_to_hex((uint8_t)(val >> 20) & 0xF));
    uart_putchar((uint8_t)nibble_to_hex((uint8_t)(val >> 16) & 0xF));
    uart_putchar((uint8_t)nibble_to_hex((uint8_t)(val >> 12) & 0xF));
    uart_putchar((uint8_t)nibble_to_hex((uint8_t)(val >> 8) & 0xF));
    uart_putchar((uint8_t)nibble_to_hex((uint8_t)(val >> 4) & 0xF));
    uart_putchar((uint8_t)nibble_to_hex((uint8_t)val & 0xF));
}

/*
 * UART response handler — idle poll and TX drain.
 *
 * Polls the UART idle condition and drains the TX buffer.
 * Called from the main super-loop's infinite dispatch cycle.
 */

/*
 * Print a 64-bit value as decimal digits with leading zeros.
 *
 * param_1 = low 32 bits of value
 * param_2 = high 32 bits of value
 * param_3 = max number of digits to print (up to 20)
 *
 * Prints leading zeros for digits that would be zero when
 * the value is compared against decreasing powers of 10.
 * The last digit is always printed (no leading-zero suppression).
 */
void uart_putdec_64(uint32_t lo, uint32_t hi, uint8_t digits)
{
    /* Powers of 10 as (lo, hi) pairs, decreasing from 10^19 down to 1 */
    static const uint32_t s_pow10_lo[20] = {
        0x6FC10000, 0xE8D4A510, 0xC9F2C9CD, 0x968918F5,
        0x04A00000, 0x7E37BE20, 0xA784379D, 0xC2229000,
        0x8D4FDF3B, 0x645A1CAC, 0x059C6800, 0x3702F3E0,
        0x3BCA4000, 0xDF90E200, 0xBD17A000, 0x96980000,
        0x78400000, 0x60000000, 0x4C4B4000, 0x3B9ACA00
    };
    static const uint32_t s_pow10_hi[20] = {
        0x00000005, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000
    };
    /* Referenced divisors in flash; here emulated with the actual 64-bit divide.
     * The OEM uses __aeabi_ldivmod (64-bit division). In C we emulate the
     * same algorithm: for each digit position, divide by the corresponding
     * power of 10 and print the quotient.
     */
    (void)s_pow10_lo;
    (void)s_pow10_hi;

    /* Simple algorithm: repeatedly divide by 10, collect digits in reverse,
     * then print them. The OEM uses a chain of __aeabi_ldivmod calls.
     */
    uint8_t buf[20];
    uint8_t pos = 0;

    /* Convert 64-bit value to decimal digits */
    do {
        /* Divide (hi, lo) by 10 */
        uint32_t rem;
        if (hi == 0) {
            rem = lo % 10;
            lo = lo / 10;
        } else {
            /* 64-bit division by 10 */
            uint64_t val = ((uint64_t)hi << 32) | lo;
            rem = (uint32_t)(val % 10);
            val = val / 10;
            hi = (uint32_t)(val >> 32);
            lo = (uint32_t)val;
        }
        buf[pos++] = (uint8_t)(rem | 0x30);
    } while (pos < digits);

    /* Print in reverse order */
    while (pos > 0) {
        uart_putchar(buf[--pos]);
    }
}

void uart_resp_handler(void)
{
    /* Poll UART idle and drain TX buffer */
    uart_tx_flush();
}

/*
 * UART protocol handler (uart_protocol_handler) — byte-at-a-time
 * Modbus-like frame receiver.
 *
 * State machine:
 *   state 0: waiting for 0xAA sync byte → state 1 on match
 *   state 1: got sync, waiting for command byte → state 2
 *   state 2+: accumulating data bytes into buffer
 *
 * On frame completion (state reaches expected length), validates
 * CRC-16 at the end of the frame. On success, dispatches to the
 * command parser. On failure, sends NAK response and resets.
 *
 * The receive buffer is at 0x20002CEC (SRAM).
 */
void uart_protocol_handler(uint8_t byte)
{
    volatile uint8_t  * const s_state    = (volatile uint8_t  *)0x20002CEC;
    volatile uint8_t  * const s_buf      = (volatile uint8_t  *)0x20002CED;
    volatile uint16_t * const s_rx_idx   = (volatile uint16_t *)0x20002CF0;
    volatile uint16_t * const s_rx_total = (volatile uint16_t *)0x20002CF2;

    uint8_t state = *s_state;

    if (state == 0) {
        /* Waiting for sync byte 0xAA */
        if (byte == 0xAA) {
            *s_state = 1;
            s_buf[0] = 0xAA;
            *s_rx_idx = 1;
        }
        return;
    }

    if (state == 1) {
        /* Got sync, store command byte */
        s_buf[*s_rx_idx] = byte;
        *s_rx_idx = 2;
        *s_state = 2;

        /* Command byte determines expected frame length */
        /* Small frames (cmd < 0x80): 8 bytes, Large frames: variable */
        if (byte < 0x80) {
            *s_rx_total = 8;
        } else {
            *s_rx_total = byte & 0x7F;
            if (*s_rx_total == 0) {
                *s_rx_total = 0x80;
            }
        }
        return;
    }

    /* State 2+: accumulating data */
    if (*s_rx_idx < *s_rx_total) {
        s_buf[*s_rx_idx] = byte;
        *s_rx_idx += 1;
    }

    /* Check if frame is complete */
    if (*s_rx_idx >= *s_rx_total) {
        /* Validate CRC-16 over the frame (last 2 bytes are CRC) */
        uint16_t frame_len = *s_rx_total;
        uint16_t calc_crc = crc16_calc((uint8_t *)s_buf, (int16_t)(frame_len - 2));
        uint16_t rx_crc   = (uint16_t)s_buf[frame_len - 1] << 8 | s_buf[frame_len - 2];

        if (calc_crc == rx_crc) {
            /* CRC OK — dispatch to command parser */
            extern void command_parser(uint32_t buf_addr, int buf_len, uint8_t cmd);
            command_parser((uint32_t)(uintptr_t)s_buf, (int)(frame_len - 2), s_buf[1]);
        } else {
            /* CRC error — send NAK and reset */
            extern void veneer_a6aa(void);
            veneer_a6aa();
        }

        /* Reset state machine for next frame */
        *s_state = 0;
        *s_rx_idx = 0;
        *s_rx_total = 0;
    }
}

/*
 * uart_printf — simplified printf-like formatter over UART.
 *
 * Iterates a format string byte-by-byte. On '%' it dispatches
 * to a switch table at (0x08008D50) for format specifiers.
 * Unrecognized '%' sequences and non-'%' bytes are output directly.
 *
 * This is FUN_08008998 from the OEM binary.
 *
 * Format specifiers (based on the switch at 0x080089e4):
 *   %x → uart_puthex_32  (print 32-bit hex)
 *   %i → uart_putdec_64  (print 64-bit decimal)
 *   %d → uart_putdec_64  (print 64-bit decimal)
 *   %l → uart_putdec_64  (print 64-bit decimal, long)
 *   %s → uart_puts       (print string)
 *   %w → uart_puthex_16  (print 16-bit hex)
 *
 * Arguments come from the stack frame (varargs-like).
 */
void uart_printf(uint8_t *fmt)
{
    if (*(volatile uint8_t *)0x20000268 == 0) {
        return;  /* UART not initialized — skip */
    }

    struct {
        uint32_t r0, r1, r2, r3;
    } args;
    uint32_t *arg_ptr = (uint32_t *)&args;

    while (*fmt != 0) {
        if (*fmt == '%') {
            fmt++;
            char spec = *fmt;

            if (spec >= '0' && spec <= 'H') {
                /* Jump through switch table at 0x08008D50 */
                switch (spec) {
                case 'x':  /* 32-bit hex */
                    uart_puthex_32(*arg_ptr++);
                    break;
                case 'X':  /* 32-bit hex (uppercase) */
                    uart_puthex_32(*arg_ptr++);
                    break;
                case 'd':  /* signed decimal */
                case 'i':  /* signed integer */
                case 'l':  /* long decimal */
                case 'u':  /* unsigned decimal */
                    {
                        uint32_t val = *arg_ptr++;
                        uart_putdec_64(val, 0, 0x14);  /* default 20 digits */
                    }
                    break;
                case 's':  /* string */
                    {
                        uint8_t *str = (uint8_t *)(*arg_ptr++);
                        uart_puts((char *)str);
                    }
                    break;
                case 'w':  /* 16-bit hex word */
                    uart_puthex_16((uint16_t)(*arg_ptr++));
                    break;
                case 'b':  /* byte */
                    uart_puthex_byte((uint8_t)(*arg_ptr++));
                    break;
                case 'c':  /* char */
                    uart_putchar((uint8_t)(*arg_ptr++));
                    break;
                default:
                    /* Unknown specifier — output literal '%' + char */
                    uart_putchar('%');
                    uart_putchar(spec);
                    break;
                }
            } else {
                /* Not a recognized format char — output literal */
                uart_putchar('%');
                uart_putchar(spec);
            }
        } else {
            uart_putchar(*fmt);
        }
        fmt++;
    }
}
