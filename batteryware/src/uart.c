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
 * EXTI0_1_IRQHandler (IRQ5) — real OEM vector target at 0x0800724C (was
 * mis-named `uart_check_parity_error`; 0x40010400 is the **EXTI** base, not
 * USART1, and offset 0x14 is EXTI_PR). Handles EXTI line 0 = PB0 button:
 * if its pending bit is set, records the event (flag bit 1 @ 0x20002BFC) and
 * clears EXTI_PR line 0. Strong def overrides the weak startup.S alias.
 */
void EXTI0_1_IRQHandler(void)
{
    volatile uint32_t * const exti = (volatile uint32_t *)0x40010400;
    volatile uint8_t  * const s_btn_flags = (volatile uint8_t *)0x20002BFC;

    if (exti[0x14 / 4] & 1) {            /* EXTI_PR line 0 (PB0) */
        *s_btn_flags |= 2;
        exti[0x14 / 4] = 1;              /* clear pending */
    }
}

/*
 * EXTI4_15_IRQHandler (IRQ7) — real OEM vector target at 0x08007278 (was
 * mis-named `uart_check_overrun_error`). Handles EXTI line 13 = PC13 power
 * button (mask 0x2000): records the event (flag bit 0 @ 0x20002BFC) and clears
 * EXTI_PR line 13. Strong def overrides the weak startup.S alias.
 */
void EXTI4_15_IRQHandler(void)
{
    volatile uint32_t * const exti = (volatile uint32_t *)0x40010400;
    volatile uint8_t  * const s_btn_flags = (volatile uint8_t *)0x20002BFC;

    if (exti[0x14 / 4] & 0x2000) {       /* EXTI_PR line 13 (PC13 power button) */
        *s_btn_flags |= 1;
        exti[0x14 / 4] = 0x2000;         /* clear pending */
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

/*
 * UART RX drain (FUN_0800adbc) — polled from the main loop. Consumes bytes
 * from the USART1 RX ring buffer (base 0x20004088, 0x400 entries, head
 * 0x20004540 / tail 0x20004544). When the control word 0x20002C00 selects
 * command mode (bits 0/1 clear, or bit 2 set), each byte is fed to the
 * binary command processor (uart_protocol_handler, unless bit 17 is set)
 * and also accumulated into an ASCII line buffer (0x20004510, max 0x2c)
 * dispatched to command_parser on CR. Otherwise the byte goes to the
 * YMODEM receiver.
 */
void uart_resp_handler(void)
{
    volatile uint16_t * const rx_head  = (volatile uint16_t *)0x20004540;
    volatile uint16_t * const rx_tail  = (volatile uint16_t *)0x20004544;
    uint8_t           * const rx_ring  = (uint8_t *)0x20004088;
    volatile uint32_t * const mode     = (volatile uint32_t *)0x20002C00;
    volatile uint16_t * const line_len = (volatile uint16_t *)0x20002C84;
    uint8_t           * const line_buf = (uint8_t *)0x20004510;

    while (*rx_head != *rx_tail) {
        uint8_t  data = rx_ring[*rx_tail];
        uint16_t i = *rx_tail;
        *rx_tail = (uint16_t)(i + 1);
        if ((uint16_t)(i + 1) > 0x3ff) *rx_tail = 0;

        if ((((*mode & 1) == 0) && (((*mode & 3) >> 1) == 0)) ||
            (((*mode & 7) >> 2) != 0)) {
            if (((*mode & 0x3ffff) >> 0x11) == 0) {
                uart_protocol_handler(data);
            }
            if (data == 0xd) {
                if (*line_len != 0) {
                    command_parser(1, (uint32_t)(uintptr_t)line_buf, (uint8_t)*line_len);
                }
                *line_len = 0;
            } else if (data < 0x20 || (int8_t)data < 0) {
                *line_len = 0;
            } else {
                uint16_t j = *line_len;
                line_buf[j] = data;
                *line_len = (uint16_t)(j + 1);
                if ((uint16_t)(j + 1) > 0x2c) *line_len = 0;
            }
        } else {
            ymodem_receive(data);
        }
    }
}

/* Absolute SRAM/EEPROM reads used by the telemetry cascade. */
#define RD8(a)   (*(volatile uint8_t  *)(uintptr_t)(a))
#define RD16(a)  (*(volatile uint16_t *)(uintptr_t)(a))
#define RD32(a)  (*(volatile uint32_t *)(uintptr_t)(a))

/* Append a 16-bit field big-endian to the response buffer. */
static void send_be(uint16_t v)
{
    modem_send_2bytes((uint8_t)(v >> 8), (uint8_t)v);
}

/* Signed value mod 10 (sign-preserving), as used by the digit-extract
 * telemetry fields. */
static int16_t digit_mod10(int32_t v)
{
    if (v < 0) {
        return (int16_t)(-(int32_t)(((uint32_t)(-v)) % 10));
    }
    return (int16_t)(((uint32_t)v) % 10);
}

/*
 * Report-table arm value (cmd-3 report dispatch, table @0x08018038).
 * Index = report_state - 3. Arm 0 composes an aggregate fault word from
 * g_fault_flags (0x20002C44); arms 1..3 yield 0; arms 4..22 yield a
 * single status bitmask. (Computed-goto in the OEM; the arms set the
 * field-3 value then fall through into the shared field-4+ cascade.)
 */
static uint16_t report_arm_value(uint8_t idx)
{
    switch (idx) {
    case 0: {
        uint16_t ff = RD16(0x20002C44);
        uint16_t rv = 0;
        if (ff & 0x0001) rv |= 0x2000;
        if (ff & 0x0002) rv |= 0x1000;
        if (ff & 0x0100) rv |= 0x0200;
        if (ff & 0x0200) rv |= 0x0100;
        return rv;
    }
    case 4:  return 0x0080;
    case 5:  return 0x0040;
    case 6:  return 0x0020;
    case 7:  return 0x0010;
    case 8:  return 0x0200;
    case 9:  return 0x0100;
    case 10: return 0x0800;
    case 11: return 0x0400;
    case 12: return 0x0008;
    case 13: return 0x0004;
    case 14: return 0x0001;
    case 15: return 0x2000;
    case 16: return 0x1000;
    case 17: return 0x8000;
    case 18: return 0x4000;
    case 19: return 0x0002;
    case 20: return 0xFFFF;
    case 21: return 0x00C0;
    case 22: return 0x0030;
    default: return 0;          /* arms 1, 2, 3 */
    }
}

/*
 * UART command/telemetry processor (FUN_0800afa4) — invoked per received
 * byte from the USART1 RX path.
 *
 * RX state machine on *(uint16_t*)0x200047D2 into command buffer 0x20004548:
 *   state 0: wait for 0xAA sync.
 *   state 1: command byte, accepted only if (1 << cmd) is in mask 0x10048
 *            (i.e. cmd in {3, 6, 0x10}); zeroes cmdbuf[2..6].
 *   state>=2: cmd 0x10 streams to flash_stream_handler; cmd 3/6 accumulate
 *             an 8-byte frame, then CRC-16 check (cmdbuf[6:7], LE).
 * On a valid frame: cmd != 3 tail-calls modem_command_dispatch(); cmd == 3
 * builds the telemetry response in buffer 0x20004648 — a long cascade of
 * report fields each gated by (report_start_index < N) && report_count, where
 * report_start_index = cmdbuf[2:3] (lower index ⇒ more fields) and
 * report_count = cmdbuf[4:5]*2 — then appends a CRC-16 and transmits it.
 */
void uart_protocol_handler(uint8_t b)
{
    volatile uint16_t * const rx_state = (volatile uint16_t *)0x200047D2;
    uint8_t           * const cmdbuf   = (uint8_t *)0x20004548;
    uint8_t           * const respbuf  = (uint8_t *)0x20004648;

    if (*rx_state == 0) {
        if (b == 0xAA) {
            cmdbuf[0] = 0xAA;
            *rx_state = 1;
        } else {
            *rx_state = 0;
        }
        return;
    }

    if (*rx_state == 1) {
        if (b <= 0x10 && ((1u << b) & 0x10048u) != 0) {
            cmdbuf[1] = b;
            *rx_state = 2;
            cmdbuf[2] = 0; cmdbuf[3] = 0; cmdbuf[4] = 0; cmdbuf[5] = 0; cmdbuf[6] = 0;
        } else {
            *rx_state = 0;
        }
        return;
    }

    /* state >= 2: dispatch / accumulate */
    {
        uint8_t cmd = cmdbuf[1];
        if (cmd == 0x10) { flash_stream_handler(b); return; }
        if (cmd > 0x10)             { *rx_state = 0; return; }
        if (cmd != 3 && cmd != 6)   { *rx_state = 0; return; }
    }

    cmdbuf[*rx_state] = b;
    *rx_state = (uint16_t)(*rx_state + 1);
    if (*rx_state < 8) {
        return;
    }

    /* Frame complete (8 bytes). */
    *(volatile uint32_t *)0x20004748 = 0;
    {
        uint16_t rx_crc = (uint16_t)(cmdbuf[6] | (cmdbuf[7] << 8));
        if (crc16_calc(cmdbuf, 6) != rx_crc) {
            protocol_reset();           /* clears state and ends the handler */
            return;
        }
    }
    *(volatile uint32_t *)0x20002C04 = 0;

    if (cmdbuf[1] != 3) {
        modem_command_dispatch();       /* cmd 6 — tail call, ends the handler */
        return;
    }

    /* ---- cmd 3: telemetry response cascade ---- */
    *(volatile uint16_t *)0x200047D0 = 3;                 /* response length */
    uint16_t r = (uint16_t)((cmdbuf[2] << 8) | cmdbuf[3]); /* report start index */
    *(volatile uint8_t *)(0x200047D4 + 1) = cmdbuf[4];
    *(volatile uint8_t *)0x200047D4       = cmdbuf[5];
    respbuf[0] = 0xAA;
    respbuf[1] = 3;
    *(volatile uint16_t *)0x200047D4 = (uint16_t)(*(volatile uint16_t *)0x200047D4 * 2);
    uint16_t rc = *(volatile uint16_t *)0x200047D4;        /* report count (gate) */
    respbuf[2] = (uint8_t)rc;

    if (r == 0 && rc)    modem_send_2bytes(1, 0);
    if (r < 2 && rc)     modem_send_2bytes(0, 1);
    if (r < 3 && rc) {
        uint16_t rv = 0;
        uint8_t rs = RD8(0x20002B58);
        if ((uint8_t)(rs - 3) < 0x17) rv = report_arm_value((uint8_t)(rs - 3));
        send_be(rv);
    }

    /* Pack/cell subsystem (base 0x20002588 / 0x200028D0 / 0x200029A8). */
    if (r < 4 && rc) {
        uint8_t v1 = RD8(0x20002589), v2 = RD8(0x2000258A);
        temp_offset_send(v2 < v1 ? v1 : v2);
    }
    if (r < 5 && rc)  send_be(RD16(0x2000281C));
    if (r < 6 && rc) {
        temp_offset_send(RD8(0x200029A8 + 0x36));
        if (RD16(0x200025A0) > 0x1c) *(volatile uint16_t *)0x200025A0 = 0x28;
    }
    if (r < 7 && rc) {
        int32_t v = (int32_t)RD32(0x200028C0);
        int16_t out = 0;
        if (v < 0) {
            uint32_t a = (uint32_t)(-v);
            if (RD16(0x200028D0 + 0x16) <= a) out += digit_mod10(-(int32_t)a);
        } else if (RD16(0x200028D0 + 0x16) <= (uint32_t)v) {
            out += digit_mod10(v);
        }
        send_be((uint16_t)out);
    }
    if (r < 8 && rc)   send_be(fg_charge_status());
    if (r < 9 && rc)   modem_send_2bytes(0, fg_status_flag_get());
    if (r < 10 && rc)  modem_send_2bytes(0, RD8(0x200028D0 + 5) == 1 ? 1 : 0);
    if (r < 0xb && rc) send_be(RD16(0x200028D0 + 2));
    if (r < 0xc && rc) send_be(RD16(0x200028D0));
    if (r < 0xd && rc) modem_send_2bytes(RD8(0x08080010), RD8(0x0808000F));
    if (r < 0xe && rc) modem_send_2bytes(RD8(0x08080012), RD8(0x08080011));
    if (r < 0xf && rc) modem_send_2bytes(RD8(0x08080014), RD8(0x08080013));
    if (r < 0x10 && rc) modem_send_2bytes(RD8(0x08080016), RD8(0x08080015));
    if (r < 0x11 && rc) modem_send_2bytes(RD8(0x08080018), RD8(0x08080017));
    if (r < 0x12 && rc) modem_send_2bytes(RD8(0x0808001A), RD8(0x08080019));
    if (r < 0x13 && rc) modem_send_2bytes(RD8(0x0808001C), RD8(0x0808001B));
    if (r < 0x14 && rc) modem_send_2bytes(RD8(0x0808001E), RD8(0x0808001D));
    if (r < 0x15 && rc) modem_send_2bytes(RD8(0x08080020), RD8(0x0808001F));
    if (r < 0x16 && rc) send_be(RD16(0x200028D0 + 6));
    if (r < 0x17 && rc) send_be((uint16_t)RD32(0x200029A8 + 0x28));
    if (r < 0x18 && rc) send_be((uint16_t)RD32(0x200029A8 + 0x2c));
    if (r < 0x19 && rc) modem_send_2bytes(0, RD8(0x200029A8 + 0x37));
    if (r < 0x1a && rc) send_be(RD16(0x200029A8 + 0x34));
    if (r < 0x1b && rc) modem_send_2bytes(0, (RD8(0x20002870) & 2) == 2 ? 1 : 0);

    /* Pack measurement block (base 0x200028A4). */
    if (r < 0x1c && rc) send_be(RD16(0x200028A4));
    if (r < 0x1d && rc) send_be(RD16(0x200028A4 + 2));
    if (r < 0x1e && rc) send_be(RD16(0x200028A4 + 4));
    if (r < 0x1f && rc) send_be(RD16(0x200028A4 + 6));
    if (r < 0x20 && rc) send_be(RD16(0x200028A4 + 8));
    if (r < 0x21 && rc) send_be(RD16(0x200028A4 + 0xa));
    if (r < 0x22 && rc) send_be(RD16(0x200028A4 + 0xc));
    if (r < 0x23 && rc) send_be(RD16(0x200028A4 + 0xe));
    if (r < 0x24 && rc) send_be(RD16(0x200028A4 + 0x10));
    if (r < 0x25 && rc) send_be(RD16(0x200028A4 + 0x12));
    if (r < 0x26 && rc) temp_offset_send(RD8(0x20002589));
    if (r < 0x27 && rc) temp_offset_send(RD8(0x2000258A));
    if (r < 0x28 && rc) temp_offset_send(RD8(0x20002588));
    if (r < 0x29 && rc) send_be(RD16(0x20002C0A));
    if (r < 0x2a && rc) send_be(RD16(0x200027FA));
    if (r < 0x2b && rc) send_be(RD16(0x2000282A));
    if (r < 0x2c && rc) modem_send_2bytes(RD8(0x20002821), RD8(0x20002820));
    if (r < 0x2d && rc) {
        *(volatile uint32_t *)0x20002A48 = 0x08004FE4;
        uint32_t s = RD32(0x20002A48);
        if (RD8(s + 0x17) == '0' && RD8(s + 0x16) == '0') {
            uint8_t hi = hex_to_nibble((char)RD8(s + 0x17));
            uint8_t lo = (uint8_t)((hex_to_nibble((char)RD8(s + 0x16)) << 4) |
                                   hex_to_nibble((char)RD8(s + 0x15)));
            modem_send_2bytes(hi, lo);
        } else {
            modem_send_2bytes(0, 4);
        }
    }

    /* Charger telemetry (base 0x20002AD0). */
    if (r < 0x31 && rc) send_be(RD16(0x20002AD0 + 2));
    if (r < 0x32 && rc) temp_offset_send(RD8(0x20002AD0 + 0x2a));
    if (r < 0x33 && rc) temp_offset_send(RD8(0x20002AD0 + 0x2b));
    if (r < 0x34 && rc) temp_offset_send(RD8(0x20002AD0 + 0x2c));
    if (r < 0x35 && rc) send_be(RD16(0x20002AD0 + 0x1a));
    if (r < 0x36 && rc) send_be((uint16_t)digit_mod10((int32_t)RD32(0x20002AD0 + 0x1c)));
    if (r < 0x37 && rc) send_be((uint16_t)RD32(0x20002AD0 + 0x20));
    if (r < 0x38 && rc) send_be((uint16_t)RD32(0x20002AD0 + 0x24));
    if (r < 0x39 && rc) modem_send_2bytes(0, RD8(0x20002AD0 + 0x28));
    if (r < 0x3a && rc) modem_send_2bytes(0, RD8(0x20002AD0 + 0x29));
    if (r < 0x3b && rc) send_be(RD16(0x20002AD0 + 4));
    if (r < 0x3c && rc) send_be(RD16(0x20002AD0 + 6));
    if (r < 0x3d && rc) send_be(RD16(0x20002AD0 + 8));
    if (r < 0x3e && rc) send_be(RD16(0x20002AD0 + 0xa));
    if (r < 0x3f && rc) send_be(RD16(0x20002AD0 + 0xc));
    if (r < 0x40 && rc) send_be(RD16(0x20002AD0 + 0xe));
    if (r < 0x41 && rc) send_be(RD16(0x20002AD0 + 0x10));
    if (r < 0x42 && rc) send_be(RD16(0x20002AD0 + 0x12));
    if (r < 0x43 && rc) send_be(RD16(0x20002AD0 + 0x14));
    if (r < 0x44 && rc) send_be(RD16(0x20002AD0 + 0x16));
    if (r < 0x45 && rc) send_be(RD16(0x20002AD0 + 0x18));
    if (r < 0x46 && rc) send_be(RD16(0x200029A8));

    /* Context block (base 0x200029A8). */
    if (r < 0x47 && rc) send_be(RD16(0x200029A8 + 2));
    if (r < 0x48 && rc) send_be(RD16(0x200029A8 + 4));
    if (r < 0x49 && rc) send_be(RD16(0x200029A8 + 6));
    if (r < 0x4a && rc) send_be(RD16(0x200029A8 + 8));
    if (r < 0x4b && rc) send_be(RD16(0x200029A8 + 0xa));
    if (r < 0x4c && rc) send_be(RD16(0x200029A8 + 0xc));
    if (r < 0x4d && rc) send_be(RD16(0x200029A8 + 0xe));
    if (r < 0x4e && rc) send_be(RD16(0x200029A8 + 0x10));
    if (r < 0x4f && rc) send_be(RD16(0x200029A8 + 0x12));
    if (r < 0x50 && rc) send_be(RD16(0x200029A8 + 0x14));
    if (r < 0x51 && rc) send_be(RD16(0x200029A8 + 0x16));
    if (r < 0x52 && rc) send_be(RD16(0x200029A8 + 0x18));
    if (r < 0x53 && rc) send_be(RD16(0x200029A8 + 0x1a));
    if (r < 0x54 && rc) send_be(RD16(0x200029A8 + 0x1c));
    if (r < 0x55 && rc) send_be(RD16(0x200029A8 + 0x1e));
    if (r < 0x56 && rc) send_be(RD16(0x200029A8 + 0x20));
    if (r < 0x57 && rc) send_be(RD16(0x200029A8 + 0x22));

    /* Charger extended block (bases 0x20002AD0 / 0x200029A8). */
    if (r < 0x58 && rc) send_be(RD16(0x20002AD0 + 0x40));
    if (r < 0x59 && rc) send_be(RD16(0x20002AD0 + 0x42));
    if (r < 0x5a && rc) temp_offset_send(RD8(0x20002AD0 + 0x44));
    if (r < 0x5b && rc) temp_offset_send(RD8(0x20002AD0 + 0x45));
    if (r < 0x5c && rc) temp_offset_send(RD8(0x20002AD0 + 0x46));
    if (r < 0x5d && rc) send_be(RD16(0x200029A8 + 0x2e));
    if (r < 0x5e && rc) send_be(RD16(0x200029A8 + 0x30));
    if (r < 0x5f && rc) send_be(RD16(0x200029A8 + 0x32));
    if (r < 0x60 && rc) send_be(RD16(0x200029A8 + 0x34));
    if (r < 0x61 && rc) send_be(RD16(0x200029A8 + 0x36));

    /* OAD image verify report. */
    if (r < 0x82 && rc) {
        if (RD32(0x200047D8) == 0x5000) {
            uint32_t n = (RD32(0x200047D8) - 4) >> 2;
            uint32_t res = dma_transfer_irq((volatile uint32_t *)0x20002C20, (void *)0x0801A800, n);
            if ((int)res == *(volatile int *)(RD32(0x200047D8) + 0x0801A7FC)) {
                modem_send_2bytes(0, 0);
            } else {
                modem_send_2bytes(0, 1);
            }
            dma_transfer_irq((volatile uint32_t *)0x20002C20, (void *)0x0801A800, n);
            uart_printf((uint8_t *)0x080173B4);
            uart_tx_flush();
        } else {
            modem_send_2bytes(0, (uint8_t)flash_verify_header(0x0801A800));
        }
        *(volatile uint32_t *)0x20002C00 &= 0xFFFFFFFBu;
    }

    /* Diagnostic ranges (base 0x20002B10). */
    if (r < 0xf31 && rc)  send_be(RD16(0x20002B10 + 2));
    if (r <= 0xf31 && rc) temp_offset_send(RD8(0x20002B10 + 0x2a));
    if (r <= 0xf32 && rc) temp_offset_send(RD8(0x20002B10 + 0x2b));
    if (r <= 0xf33 && rc) temp_offset_send(RD8(0x20002B10 + 0x2c));
    if (r <= 0xf34 && rc) send_be(RD16(0x20002B10 + 0x1a));
    if (r <= 0xf35 && rc) send_be((uint16_t)digit_mod10((int32_t)RD32(0x20002B10 + 0x1c)));
    if (r <= 0xf36 && rc) send_be((uint16_t)RD32(0x20002B10 + 0x20));
    if (r <= 0xf37 && rc) send_be((uint16_t)RD32(0x20002B10 + 0x24));
    if (r <= 0xf38 && rc) modem_send_2bytes(0, RD8(0x20002B10 + 0x28));
    if (r <= 0xf39 && rc) modem_send_2bytes(0, RD8(0x20002B10 + 0x29));
    if (r <= 0xf3a && rc) send_be(RD16(0x20002B10 + 4));
    if (r <= 0xf3b && rc) send_be(RD16(0x20002B10 + 6));
    if (r <= 0xf3c && rc) send_be(RD16(0x20002B10 + 8));
    if (r <= 0xf3d && rc) send_be(RD16(0x20002B10 + 0xa));
    if (r <= 0xf3e && rc) send_be(RD16(0x20002B10 + 0xc));
    if (r < 0xf40 && rc)  send_be(RD16(0x20002B10 + 0xe));
    if (r < 0xf41 && rc)  send_be(RD16(0x20002B10 + 0x10));
    if (r <= 0xf41 && rc) send_be(RD16(0x20002B10 + 0x12));
    if (r <= 0xf42 && rc) send_be(RD16(0x20002B10 + 0x14));
    if (r <= 0xf43 && rc) send_be(RD16(0x20002B10 + 0x16));
    if (r <= 0xf44 && rc) send_be(RD16(0x20002B10 + 0x18));
    if (r <= 0xf45 && rc) send_be(RD16(0x20002B10));
    if (r > 0xf01f && rc) {
        if (r <= 0xf020 && rc) modem_send_2bytes(0, RD8(0x200025BE));
        if (r <= 0xf021 && rc) modem_send_2bytes(0, RD8(0x200025C4));
        if (r <= 0xf022 && rc) modem_send_2bytes(0, RD8(0x200025BC));
        if (r <= 0xf023 && rc) send_be(RD16(0x200029A8 + 0x4c));
        if (r <= 0xf024 && rc) send_be(RD16(0x200027F0));
    }

    /* ---- response transmit ---- */
    if (RD8(0x20004748) == 0) {
        protocol_reset();
        return;
    }
    if (RD16(0x200025A0) > 0x27) config_resend_all();

    uint16_t crc = crc16_calc(respbuf, (int16_t)RD16(0x200047D0));
    modem_send_2bytes((uint8_t)crc, (uint8_t)(crc >> 8));
    for (uint8_t i = 0; (uint16_t)i < RD16(0x200047D0); i++) {
        uart_putchar(respbuf[i]);
    }
    *(volatile uint32_t *)0x20002C04 = 0;
    protocol_reset();
}

#undef RD8
#undef RD16
#undef RD32

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

/*
 * ============================================================================
 * STM32L0 HAL UART initialisation chain.
 *
 * These three functions were mis-identified as flash routines in the OEM
 * decomp (flash_page_program / flash_prescaler_setup / flash_program_init,
 * formerly in flash.c). They are HAL_UART_Init, UART_SetConfig and
 * HAL_UART_MspInit, and operate on the UART handle (see s_uart_base and
 * service_uart_init). Their two callees uart_adv_feature_config and
 * uart_check_idle_state (UART_AdvFeatureConfig / UART_CheckIdleState) remain
 * in dma.c for now (also mis-filed there).
 * ============================================================================
 */

/*
 * HAL_UART_MspInit (FUN_08011594) — board-level UART MSP init. Empty in this
 * image (clocks/GPIO are brought up by the caller, e.g. service_uart_init).
 */
void hal_uart_msp_init(void *ctx)
{
    (void)ctx;
}

/*
 * UART_SetConfig (FUN_080115A4) — configure CR1/CR2/CR3 and the baud-rate
 * register from the handle.
 *
 * The variable named `prescaler` is the kernel **clock-source code** read from
 * RCC->CCIPR (RCC[0x13] = offset 0x4C): per instance it is mapped to one of
 * {0=PCLK, 1=PCLK2, 2=HSI16, 4=SYSCLK, 8=LSE} (codes 3/5/6/7/0x10 = error).
 * The kernel frequency is resolved from that code:
 *   - 0 -> fg_read_field_8()   (HAL_RCC_GetPCLK1Freq-shaped)
 *   - 1 -> fg_read_field_11()  (HAL_RCC_GetPCLK2Freq-shaped)
 *   - 2 -> 4 MHz / 16 MHz depending on RCC_CR HSI16 divider (bit 4)
 *   - 4 -> clock_prescaler_val()  (SYSCLK)
 *   - 8 -> 0x8000 (LSE, 32768 Hz)
 *
 * BRR is then written to the instance's BRR (*ctx + 0x0C):
 *   - 0x40004800 is actually **LPUART1**: BRR = (256*freq + baud/2)/baud
 *     (64-bit), valid range [0x300, 0xFFFFF].
 *   - Otherwise a standard USART: OVER8 (ctx[7]==0x8000) uses
 *     ((2*freq + baud/2)/baud) with the low-nibble fixup
 *     `(brr & ~0xF) | ((brr>>1)&7)`; OVER16 uses (freq + baud/2)/baud.
 *     Both require 0xF < BRR < 0x10000.
 *
 * Returns 0 on success, 1 on error (bad range, unknown clock source).
 */
uint32_t uart_set_config(int *ctx)
{
    volatile uint32_t * const RCC = (volatile uint32_t *)0x40021000;
    const uint32_t USART1_BASE = 0x40013800;  /* DAT_080118AC */
    const uint32_t USART2_BASE = 0x40004400;  /* DAT_080118B4 */
    const uint32_t LPUART1_BASE = 0x40004800; /* DAT_080118A4 (prev. "USART3") */
    const uint32_t TIM67_BASE  = 0x40004C00;  /* DAT_080118B8 */
    const uint32_t TIM3_BASE   = 0x40005000;  /* DAT_080118BC */
    const uint32_t MAGIC_4MHZ  = 0x003D0900;  /* DAT_080118C0 */
    const uint32_t MAGIC_16MHZ = 0x00F42400;  /* DAT_080118C4 */

    uint8_t  error = 0;
    uint8_t  prescaler = 0x10;
    uint32_t divisor = 0;

    volatile uint32_t *reg = (volatile uint32_t *)*ctx;

    reg[0] = ctx[7] | ctx[2] | ctx[4] | ctx[5] | (reg[0] & 0xEFFF69F3);
    reg[1] = ctx[3] | (reg[1] & 0xFFFFCFFF);
    uint32_t cr_val = ctx[6];
    if (*ctx != LPUART1_BASE) {
        cr_val |= ctx[8];
    }
    reg[2] = cr_val | (reg[2] & 0xFFFFF4FF);

    /* Clock-source code from RCC->CCIPR, per instance. */
    if (*ctx == USART1_BASE) {
        uint32_t rcc_val = RCC[0x13 / 4] & 3;
        if (rcc_val == 3)       prescaler = 8;
        else if (rcc_val == 2)  prescaler = 2;
        else if (rcc_val == 1)  prescaler = 4;
        else if (rcc_val == 0)  prescaler = 1;
    } else if (*ctx == USART2_BASE) {
        uint32_t rcc_val = RCC[0x13 / 4] & 0xC;
        if (rcc_val == 0xC)      prescaler = 8;
        else if (rcc_val == 0x8) prescaler = 2;
        else if (rcc_val == 0x4) prescaler = 4;
        else if (rcc_val == 0x0) prescaler = 0;
    } else if (*ctx == TIM67_BASE) {
        prescaler = 0;
    } else if (*ctx == TIM3_BASE) {
        prescaler = 0;
    } else if (*ctx == LPUART1_BASE) {
        uint32_t rcc_val = RCC[0x13 / 4] & 0xC00;
        if (rcc_val == 0xC00)      prescaler = 8;
        else if (rcc_val == 0x800) prescaler = 2;
        else if (rcc_val == 0x400) prescaler = 4;
        else if (rcc_val == 0)     prescaler = 0;
    }

    if (*ctx != LPUART1_BASE) {
        /* Standard USART (USART1/USART2): resolve the kernel clock frequency
         * from the clock-source code, then compute the BRR with the OVER8 or
         * OVER16 formula (ctx[7] == 0x8000 selects OVER8). */
        uint32_t freq;
        switch (prescaler) {
        case 0:  freq = fg_read_field_8();             break;  /* PCLK1  */
        case 1:  freq = fg_read_field_11();            break;  /* PCLK2  */
        case 2:  freq = (RCC[0] & 0x10) ? MAGIC_4MHZ : MAGIC_16MHZ; break;  /* HSI16 */
        case 4:  freq = (uint32_t)clock_prescaler_val(); break;  /* SYSCLK */
        case 8:  freq = 0x8000;                        break;  /* LSE 32768 Hz */
        default: freq = 0; error = 1;                  break;  /* unknown source */
        }

        if (freq != 0) {
            uint32_t baud = (uint32_t)ctx[1];
            uint32_t brr;
            if (ctx[7] == 0x8000) {                 /* OVER8 */
                brr = (freq * 2 + baud / 2) / baud;
                if ((brr <= 0xF) || (brr >= 0x10000)) {
                    error = 1;
                } else {
                    reg[3] = (brr & ~0xFu) | ((brr >> 1) & 0x7u);
                }
            } else {                                /* OVER16 */
                brr = (freq + baud / 2) / baud;
                if ((brr <= 0xF) || (brr >= 0x10000)) {
                    error = 1;
                } else {
                    reg[3] = brr;
                }
            }
        }
        goto done;
    }

    /* LPUART1: BRR = (256*freq + baud/2) / baud, range [0x300, 0xFFFFF]. */
    if (prescaler == 8) {
        divisor = 0x8000;
    } else if (prescaler == 4) {
        divisor = (uint32_t)clock_prescaler_val();
    } else if (prescaler == 2) {
        if ((RCC[0] & 0x10) == 0) {
            divisor = MAGIC_16MHZ;
        } else {
            divisor = MAGIC_4MHZ;
        }
    } else if (prescaler == 0) {
        divisor = fg_read_field_8();
    } else {
        divisor = 0;
        error = 1;
    }

    if (divisor != 0) {
        if ((divisor < (uint32_t)(ctx[1] * 3)) ||
            ((uint32_t)(ctx[1] * 0x1000) < divisor)) {
            error = 1;
        } else {
            uint32_t baud = (uint32_t)((uint64_t)(((uint32_t)ctx[1] >> 1) + divisor * 0x100) /
                                       (uint64_t)ctx[1]);
            if ((baud < 0x300) || (baud > 0xFFFFF)) {
                error = 1;
            } else {
                reg[3] = baud;
            }
        }
    }

done:
    ctx[0x19] = 0;
    ctx[0x1A] = 0;
    return (uint32_t)error;
}

/*
 * HAL_UART_Init (FUN_080114EC) — initialise a UART instance from its handle.
 * Disables UE, runs MspInit on first use, applies UART_SetConfig, optional
 * advanced-feature config, sets the CR2/CR3 masks, re-enables UE, then checks
 * the idle state. Returns 0 on success, 1/3 on error.
 */
uint32_t hal_uart_init(int *ctx)
{
    if (ctx == NULL) {
        return 1;
    }

    if (ctx[0x1E] == 0) {
        *(volatile uint8_t *)(ctx + 0x1D) = 0;
        hal_uart_msp_init(ctx);
    }

    ctx[0x1E] = 0x24;
    *(volatile uint32_t *)*ctx &= ~1U;

    if (uart_set_config(ctx) == 1) {
        return 1;
    }

    if (ctx[9] != 0) {
        uart_adv_feature_config(ctx);
    }

    volatile uint32_t *reg = (volatile uint32_t *)*ctx;
    reg[1] &= 0xFFFFB7FF;
    reg[2] &= 0xFFFFFFD5;
    *(volatile uint32_t *)*ctx |= 1;

    return uart_check_idle_state((uint32_t *)ctx);
}
