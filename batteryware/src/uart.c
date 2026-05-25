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
 * UART response handler — idle poll and TX drain.
 *
 * Polls the UART idle condition and drains the TX buffer.
 * Called from the main super-loop's infinite dispatch cycle.
 */
void uart_resp_handler(void)
{
    /* Poll UART idle and drain TX buffer */
    uart_tx_flush();
}
