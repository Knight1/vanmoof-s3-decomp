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
