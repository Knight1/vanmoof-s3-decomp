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

/*
 * uart_ch1_tx_pump — OEM FUN_08016110.
 *
 * Push the next byte of the channel-1 TX ring into the UART when it is idle. The
 * ring (20-byte buffer at 0x20000784, tail 0x2000084a, head 0x2000084e) feeds the
 * HAL handle at 0x200007ac. When gState is READY (0x20) and the ring is non-empty,
 * pop ring[tail], advance tail (wrap at 20), mark the handle BUSY_TX (0x21), write
 * the byte to USART TDR (Instance +0x28) and enable TXEIE (CR1 bit7) so the ISR
 * carries on. When gState is RESET (0), the indices are cleared. Disasm-confirmed.
 */
void uart_ch1_tx_pump(void)
{
    volatile uint8_t  *gstate = (volatile uint8_t  *)(0x200007ac + 0x69);
    volatile uint16_t *tail   = (volatile uint16_t *)0x2000084a;
    volatile uint16_t *head   = (volatile uint16_t *)0x2000084e;
    volatile uint8_t  *ring   = (volatile uint8_t  *)0x20000784;

    if (*gstate == 0x20) {                     /* HAL_UART_STATE_READY */
        if (*tail != *head) {
            uint16_t t = *tail;
            uint8_t b = ring[t];
            *gstate = 0x21;                    /* HAL_UART_STATE_BUSY_TX */
            *tail = (uint16_t)(t + 1);
            if ((uint16_t)(t + 1) > 0x13) {    /* wrap at 20 entries */
                *tail = 0;
            }
            volatile uint32_t *inst = (volatile uint32_t *)*(volatile uint32_t *)0x200007ac;
            *(volatile uint16_t *)((uint8_t *)inst + 0x28) = b;   /* USART TDR */
            inst[0] |= 0x80u;                  /* CR1.TXEIE */
        }
    } else if (*gstate == 0) {                 /* HAL_UART_STATE_RESET */
        *head = 0;
        *tail = 0;
    }
}

/*
 * uart_flush_ch1 — OEM FUN_080161b4.
 *
 * Drain the channel-1 UART before a reset: if the HAL handle's gState byte
 * (handle base 0x200007ac, +0x69) isn't RESET, kick the TX engine and spin until
 * gState returns to HAL_UART_STATE_READY (0x20). Called from the Modbus/OTA
 * reset paths so the final reply leaves the wire before the MCU resets.
 */
void uart_flush_ch1(void)
{
    if (*(volatile uint8_t *)(0x200007ac + 0x69) != 0) {
        uart_ch1_tx_pump();
        while (*(volatile uint8_t *)(0x200007ac + 0x69) != 0x20) {
        }
    }
}

/* hal_uart_msp_init — OEM FUN_0801d234 (HAL_UART_MspInit): empty weak callback
 * (the USART2 clock + GPIO are set up directly in uart_msp_init). */
void hal_uart_msp_init(void *huart)
{
    (void)huart;
}
/*
 * uart_wait_on_flag — OEM FUN_0801d8a0 (UART_WaitOnFlagUntilTimeout).
 * Poll USART ISR (+0x1c) until `flag` differs from `status`, or the timeout
 * elapses (0xffffffff = wait forever). On timeout, disable the transfer (CR1
 * mask 0xfffffe5f, CR3 &= ~EIE), mark gState/RxState READY and return 3; on the
 * flag reaching its target, return 0. Masks/widths disasm-confirmed.
 */
int uart_wait_on_flag(void *handle, uint32_t flag, uint8_t status,
                      uint32_t tickstart, uint32_t timeout)
{
    uint32_t *huart = (uint32_t *)handle;
    uint8_t  *b     = (uint8_t  *)handle;
    volatile uint32_t *inst = (volatile uint32_t *)huart[0];

    for (;;) {
        uint8_t flag_set = (flag == (inst[7] & flag)) ? 1u : 0u;   /* ISR (+0x1c) */
        if (flag_set != status) {
            return 0;
        }
        if (!(timeout == 0xffffffffu ||
              (timeout != 0 && (uint32_t)(tick_get() - tickstart) <= timeout))) {
            break;
        }
    }
    inst[0] &= 0xfffffe5fu;                         /* CR1: disable TE/RE etc. */
    inst[2] &= 0xfffffffeu;                         /* CR3: clear EIE */
    b[0x69] = 0x20;                                 /* gState  = READY */
    b[0x6a] = 0x20;                                 /* RxState = READY */
    b[0x68] = 0;
    return 3;
}

/*
 * uart_advfeature_config — OEM FUN_0801d678 (UART_AdvFeatureConfig).
 * Apply each enabled advanced feature (per the AdvFeatureInit bitmask at +0x24)
 * to USART CR2 (+0x04) / CR3 (+0x08): Tx/Rx-invert, data-invert, swap, overrun-
 * disable, DMA-disable-on-RxError, auto-baud (+ its mode), MSB-first. Each is a
 * masked field write; the clear masks are disasm-confirmed.
 */
void uart_advfeature_config(void *handle)
{
    uint32_t *h = (uint32_t *)handle;
    volatile uint32_t *inst = (volatile uint32_t *)h[0];
    uint32_t adv = h[9];                            /* AdvFeatureInit selector */

    if (adv & 1u)    { inst[1] = h[10]   | (inst[1] & 0xfffdffffu); }   /* CR2 TXINV  */
    if (adv & 2u)    { inst[1] = h[0xb]  | (inst[1] & 0xfffeffffu); }   /* CR2 RXINV  */
    if (adv & 4u)    { inst[1] = h[0xc]  | (inst[1] & 0xfffbffffu); }   /* CR2 DATAINV*/
    if (adv & 8u)    { inst[1] = h[0xd]  | (inst[1] & 0xffff7fffu); }   /* CR2 SWAP   */
    if (adv & 0x10u) { inst[2] = h[0xe]  | (inst[2] & 0xffffefffu); }   /* CR3 OVRDIS */
    if (adv & 0x20u) { inst[2] = h[0xf]  | (inst[2] & 0xffffdfffu); }   /* CR3 DDRE   */
    if (adv & 0x40u) {
        inst[1] = h[0x10] | (inst[1] & 0xffefffffu);                    /* CR2 ABREN  */
        if (h[0x10] == 0x100000u) {
            inst[1] = h[0x11] | (inst[1] & 0xff9fffffu);               /* CR2 ABRMOD */
        }
    }
    if (adv & 0x80u) { inst[1] = h[0x12] | (inst[1] & 0xfff7ffffu); }   /* CR2 MSBFIRST */
}

/*
 * uart_set_config — OEM FUN_0801d244 (UART_SetConfig).
 * Program CR1 (WordLength|Parity|Mode|OverSampling), CR2 (StopBits) and CR3
 * (HwFlowCtl|OneBitSampling) from the Init fields, resolve the per-instance
 * USARTxSW clock source (RCC_CFGR3 +0x30), read the matching clock frequency,
 * and compute USART_BRR (over-8 or over-16). Handle words: [1]BaudRate
 * [2]WordLength [3]StopBits [4]Parity [5]Mode [6]HwFlowCtl [7]OverSampling
 * [8]OneBitSampling. Masks/clock-source fields/BRR formulas disasm-confirmed.
 * Returns 0 = OK, 1 = ERROR. (The UART_DIV divide is the OEM's libgcc udiv —
 * reproduced as plain `/`, which lowers to __aeabi_uidiv.)
 */
int uart_set_config(void *handle)
{
    uint32_t *h = (uint32_t *)handle;
    volatile uint32_t *uart = (volatile uint32_t *)h[0];
    volatile uint32_t *cfgr3 = (volatile uint32_t *)(0x40021000u + 0x30);
    uint32_t baud = h[1];
    int      ret  = 0;
    uint32_t clk_src = 0x10;                        /* invalid sentinel */

    uart[0] = (uart[0] & 0xefff69f3u) | (h[2] | h[4] | h[5] | h[7]);  /* CR1 */
    uart[1] = h[3] | (uart[1] & 0xffffcfffu);                          /* CR2: StopBits */
    uart[2] = (h[6] | h[8]) | (uart[2] & 0xfffff4ffu);                 /* CR3 */

    /* clk_src encoding: 0=PCLK1, 4=SYSCLK, 8=LSE, 2=HSI, 0x10=invalid. */
    if (h[0] == 0x40013800u) {                      /* USART1: CFGR3[1:0] */
        switch (*cfgr3 & 0x3u) {
        case 0: clk_src = 0; break;  case 1: clk_src = 4; break;
        case 2: clk_src = 8; break;  default: clk_src = 2; break;
        }
    } else if (h[0] == 0x40004400u) {               /* USART2: CFGR3[17:16] */
        switch (*cfgr3 & 0x30000u) {
        case 0x00000: clk_src = 0; break;  case 0x10000: clk_src = 4; break;
        case 0x20000: clk_src = 8; break;  default: clk_src = 2; break;
        }
    } else if (h[0] == 0x40004800u) {               /* USART3: CFGR3[19:18] */
        switch (*cfgr3 & 0xc0000u) {
        case 0x00000: clk_src = 0; break;  case 0x40000: clk_src = 4; break;
        case 0x80000: clk_src = 8; break;  default: clk_src = 2; break;
        }
    } else if (h[0] == 0x40004c00u || h[0] == 0x40005000u || h[0] == 0x40011400u ||
               h[0] == 0x40011800u || h[0] == 0x40011c00u) {   /* UART4..8: PCLK1 */
        clk_src = 0;
    }

    if (h[7] == 0x8000u) {                          /* OverSampling == OVER8 */
        uint32_t div = 0;
        switch (clk_src) {
        case 0: div = ((hal_rcc_get_pclk1_freq() << 1) + (baud >> 1)) / baud; break;
        case 2: div = ((baud >> 1) + 0x00f42400u) / baud; break;            /* 2*HSI */
        case 4: div = (((uint32_t)hal_rcc_get_sysclock_freq() << 1) + (baud >> 1)) / baud; break;
        case 8: div = ((baud >> 1) + 0x10000u) / baud; break;               /* 2*LSE */
        default: ret = 1; break;
        }
        uart[3] = (div & 0xfff0u) | ((div >> 1) & 7u);   /* BRR (over-8 nibble) */
    } else {                                        /* OVER16 */
        switch (clk_src) {
        case 0: uart[3] = hal_rcc_get_pclk1_freq() / baud; break;
        case 2: uart[3] = 0x007a1200u / baud; break;                        /* HSI */
        case 4: uart[3] = (uint32_t)hal_rcc_get_sysclock_freq() / baud; break;
        case 8: uart[3] = 0x8000u / baud; break;                            /* LSE */
        default: ret = 1; break;
        }
    }

    return ret;
}

/*
 * uart_check_idle_state — OEM FUN_0801d7e0 (UART_CheckIdleState).
 * For a real USART instance, wait (≤0x01ffffff ticks) for TEACK (if TE set) and
 * REACK (if RE set) in ISR, then mark gState/RxState READY (0x20). gState +0x69,
 * RxState +0x6a, +0x68 (cleared), +0x6c (cleared) — widths disasm-confirmed.
 * Returns 0 = OK, 3 = TIMEOUT.
 */
int uart_check_idle_state(void *handle)
{
    uint32_t *h = (uint32_t *)handle;
    uint8_t  *b = (uint8_t  *)handle;

    h[0x1b] = 0;                                    /* +0x6c */
    uint32_t tickstart = tick_get();
    uint32_t inst = h[0];

    if (inst == 0x40013800u || inst == 0x40004400u || inst == 0x40004800u) {   /* USART1/2/3 */
        if ((*(volatile uint32_t *)inst & 8) == 8 &&
            uart_wait_on_flag(handle, 0x200000, 0, tickstart, 0x01ffffff) != 0) {   /* TE -> TEACK */
            return 3;
        }
        if ((*(volatile uint32_t *)inst & 4) == 4 &&
            uart_wait_on_flag(handle, 0x400000, 0, tickstart, 0x01ffffff) != 0) {   /* RE -> REACK */
            return 3;
        }
    }
    b[0x69] = 0x20;                                 /* gState  = READY */
    b[0x6a] = 0x20;                                 /* RxState = READY */
    b[0x68] = 0;
    return 0;
}

/*
 * hal_uart_init — OEM FUN_0801d184 (HAL_UART_Init).
 * Sequences the UART bring-up: (first use) MSP init; UE off; UART_SetConfig
 * (which computes BRR + writes CR1/CR2/CR3); optional AdvancedInit; clear the
 * unused CR2.CLKEN and CR3.{SCEN,HDSEL,IREN}; UE on; UART_CheckIdleState. The
 * baud math lives in UART_SetConfig (kept extern). gState +0x69 (0x24 = BUSY),
 * scratch +0x68, AdvancedInit gate at +0x24. Masks disasm-confirmed.
 */
int hal_uart_init(void *handle)
{
    if (handle == NULL) {
        return 1;
    }
    uint8_t *h = (uint8_t *)handle;

    if (h[0x69] == 0) {                             /* gState == RESET */
        h[0x68] = 0;
        hal_uart_msp_init(h);                       /* HAL_UART_MspInit */
    }
    h[0x69] = 0x24;                                 /* gState = BUSY */

    volatile uint32_t *inst = *(volatile uint32_t **)h;   /* Instance */
    inst[0] &= 0xfffffffeu;                         /* CR1.UE = 0 */

    if (uart_set_config(h) == 1) {                  /* UART_SetConfig */
        return 1;
    }
    if (*(uint32_t *)(h + 0x24) != 0) {             /* AdvancedInit */
        uart_advfeature_config(h);
    }

    inst[1] &= 0xffffb7ffu;                         /* CR2: clear CLKEN (bit11) */
    inst[2] &= 0xffffffd5u;                         /* CR3: clear SCEN/HDSEL/IREN */
    inst[0] |= 1u;                                  /* CR1.UE = 1 */

    return uart_check_idle_state(h);                /* UART_CheckIdleState */
}

/*
 * uart_msp_init — OEM FUN_0801647c. One of board_init's peripheral sub-inits.
 * Bring up USART2 (PA2/PA3 = AF1 TX/RX, 115200 8N1, RXNEIE) and its IRQ
 * (28 = USART2_IRQn), then arm the TX/RX ring state this module drives. The HAL
 * handle is s_handle (0x20001a60). Field values, ring-index widths (strh) and
 * the CR1.RXNEIE set are disasm-confirmed against the OEM image.
 */
void uart_msp_init(void)
{
    volatile uint32_t * const RCC = (volatile uint32_t *)0x40021000u;
    uint32_t * const hu = (uint32_t *)s_handle;   /* 0x20001a60 */

    gpio_pin_cfg_t gcfg;
    mem_set(&gcfg, 0, sizeof gcfg);

    RCC[7] |= 0x20000u;   (void)(RCC[7] & 0x20000u);   /* APB1ENR (+0x1c) USART2EN */
    RCC[5] |= 0x20000u;   (void)(RCC[5] & 0x20000u);   /* AHBENR  (+0x14) IOPAEN   */

    gcfg.pin_mask = 0xc;             /* PA2, PA3 */
    gcfg.mode     = GPIO_MODE_AF;
    gcfg.pupd     = 0;
    gcfg.speed    = 3;
    gcfg.af       = 1;               /* AF1 = USART2 */
    gpio_pin_config((uint32_t *)0x48000000u, &gcfg);

    hu[0]  = 0x40004400u;   /* Instance = USART2 */
    hu[1]  = 0x1c200u;      /* Init.BaudRate (115200) */
    hu[2]  = 0;             /* WordLength 8-bit  */
    hu[3]  = 0;             /* StopBits 1        */
    hu[4]  = 0;             /* Parity none       */
    hu[5]  = 0xc;           /* Mode = TX | RX    */
    hu[6]  = 0;             /* HwFlowCtl none    */
    hu[7]  = 0;             /* OverSampling 16   */
    hu[8]  = 0;             /* OneBitSampling    */
    hu[9]  = 0x20;          /* AdvancedInit      */
    hu[15] = 0x2000;        /* +0x3c */

    {
        volatile uint32_t *inst = (volatile uint32_t *)hu[0];
        inst[0] = 0;   /* CR1 */
        inst[1] = 0;   /* CR2 */
        inst[2] = 0;   /* CR3 */
    }
    *(volatile uint32_t *)0x20002614u = 0;   /* free-running ms tick */

    if (hal_uart_init(hu) != 0) { spi_error_reset(); }

    hu[0x1b] = 0;                                            /* +0x6c */
    *(volatile uint8_t *)((uint8_t *)hu + 0x69) = 0x20;     /* TX state = idle */
    *(volatile uint8_t *)((uint8_t *)hu + 0x6a) = 0x20;     /* RX state = idle */

    *(volatile uint16_t *)0x20000a5c = 0;   /* TX ring write index */
    *(volatile uint16_t *)0x20000854 = 0;   /* TX ring read index  */
    *(volatile uint16_t *)0x20000a58 = 0;   /* RX ring write index */
    *(volatile uint16_t *)0x20000a5a = 0;   /* RX ring read index  */

    *(volatile uint32_t *)hu[0] |= 0x20u;   /* USART2 CR1: RXNEIE */

    nvic_set_priority(28, 0, 0);   /* USART2_IRQn */
    nvic_enable_irq(28);
}

/*
 * USART2_IRQHandler — OEM FUN_080161e0 (HAL_UART_IRQHandler, USART2 instance).
 *
 * Pure leaf ISR: no callees, all accesses are direct loads/stores. It is the
 * producer/continuation counterpart to the polled helpers already in this file:
 *   - RXNE feeds the RX ring (0x20000858, wr idx 0x20000a58) that uart_rx_handler
 *     drains; wrap at 0x1ff (DAT_08016464).
 *   - TXE pops the TX ring (base 0x20000a60, rd idx 0x20000854, wr idx 0x20000a5c)
 *     that uart_putchar/uart_tx_isr fill; wrap at 0xfff (DAT_08016478), same ring
 *     this module already owns.
 * The error branch follows HAL's UART_EndRxTransfer cleanup: on a real frame/parity/
 * noise/overrun error with the matching IE bit set, it clears the flag via ICR
 * (+0x20), records the bit in ErrorCode (handle +0x6c), and — for overrun, or once an
 * error has latched — masks RXNEIE|PEIE (CR1 &= 0xfffffedf), clears CR3.EIE and parks
 * RxState READY (+0x6a = 0x20). gState READY (+0x69 = 0x20) is set on a TC with TCIE.
 *
 * Register/field resolutions (literal pool at 0x8016458):
 *   DAT_08016458 -> 0x20001a60  s_handle (HAL UART handle; Instance = USART2 @ +0x00)
 *   DAT_0801645c -> 0x20000a58  RX ring write index   (uart_rx_handler rx_wr)
 *   DAT_08016460 -> 0x20000858  RX ring base          (uart_rx_handler rx_ring)
 *   DAT_08016464 -> 0x000001ff  RX ring wrap mask compare
 *   DAT_08016468 -> 0xfffffedf  CR1 clear mask (RXNEIE bit5 | PEIE bit8)
 *   DAT_0801646c -> 0x20000854  TX ring read index    (s_rd_idx)
 *   DAT_08016470 -> 0x20000a5c  TX ring write index   (s_wr_idx)
 *   DAT_08016474 -> 0x20000a60  TX ring base          (UART_RING)
 *   DAT_08016478 -> 0x00000fff  TX ring wrap compare
 * USART (F0): ISR +0x1c, CR1 +0x00, CR3 +0x08, ICR +0x20, RDR +0x24, TDR +0x28.
 */
void USART2_IRQHandler(void)
{
    volatile uint8_t  *h    = s_handle;                          /* 0x20001a60 */
    volatile uint32_t *inst = *(volatile uint32_t * volatile *)s_handle;  /* USART2 */
    volatile uint32_t *errcode = (volatile uint32_t *)(s_handle + 0x6c);

    volatile uint16_t * const rx_wr   = (volatile uint16_t *)0x20000a58;
    uint8_t           * const rx_ring = (uint8_t *)0x20000858;

    uint32_t isr_reg = inst[7];   /* ISR (+0x1c) */
    uint32_t cr1     = inst[0];   /* CR1 (+0x00) */
    uint32_t cr3     = inst[2];   /* CR3 (+0x08) */

    if ((isr_reg & 0xf) == 0) {
        /* RXNE with RXNEIE: stash the received byte into the RX ring. */
        if ((isr_reg & 0x20) != 0 && (cr1 & 0x20) != 0) {
            rx_ring[*rx_wr] = (uint8_t)*(volatile uint16_t *)((uint8_t *)inst + 0x24);  /* RDR */
            uint16_t next = (uint16_t)(*rx_wr + 1);
            *rx_wr = next;
            if (next > 0x1ff) {
                *rx_wr = 0;
            }
        }
    } else if ((cr3 & 1) != 0 || (cr1 & 0x120) != 0) {
        /* An error flag is set and either EIE or RXNEIE/PEIE is enabled. */
        if ((isr_reg & 1) != 0 && (cr1 & 0x100) != 0) {            /* PE  & PEIE */
            *(volatile uint32_t *)((uint8_t *)inst + 0x20) = 1;    /* ICR: PECF */
            *errcode |= 1;
        }
        if ((isr_reg & 2) != 0 && (cr3 & 1) != 0) {               /* FE  & EIE  */
            *(volatile uint32_t *)((uint8_t *)inst + 0x20) = 2;    /* ICR: FECF */
            *errcode |= 4;
        }
        if ((isr_reg & 4) != 0 && (cr3 & 1) != 0) {               /* NE  & EIE  */
            *(volatile uint32_t *)((uint8_t *)inst + 0x20) = 4;    /* ICR: NCF  */
            *errcode |= 2;
        }
        if ((isr_reg & 8) != 0 && ((cr1 & 0x20) != 0 || (cr3 & 1) != 0)) {  /* ORE */
            *(volatile uint32_t *)((uint8_t *)inst + 0x20) = 8;    /* ICR: ORECF */
            *errcode |= 8;
        }

        if (*errcode != 0) {
            /* If the error is recoverable, the byte is still in RDR: ingest it. */
            if ((isr_reg & 0x20) != 0 && (cr1 & 0x20) != 0) {     /* RXNE & RXNEIE */
                rx_ring[*rx_wr] = (uint8_t)*(volatile uint16_t *)((uint8_t *)inst + 0x24);
                uint16_t next = (uint16_t)(*rx_wr + 1);
                *rx_wr = next;
                if (next > 0x1ff) {
                    *rx_wr = 0;
                }
            }

            if ((*errcode & 8) == 0 && (inst[2] & 0x40) == 0) {   /* not ORE and not DMAR */
                *errcode = 0;
            } else {
                /* Hard error: end the RX transfer (HAL UART_EndRxTransfer). */
                inst[0] &= 0xfffffedfu;                            /* CR1: clear RXNEIE|PEIE */
                inst[2] &= 0xfffffffeu;                            /* CR3: clear EIE */
                inst[0] |= 0x20u;                                  /* CR1: re-enable RXNEIE */
                h[0x6a] = 0x20;                                    /* RxState = READY */
            }
        }
    }

    if ((isr_reg & 0x80) != 0 && (cr1 & 0x80) != 0) {
        /* TXE with TXEIE: drain the TX ring or, when empty, switch to TC. */
        volatile uint16_t * const tx_rd   = (volatile uint16_t *)0x20000854;
        volatile uint16_t * const tx_wr   = (volatile uint16_t *)0x20000a5c;
        uint8_t           * const tx_ring = (uint8_t *)0x20000a60;

        if (*tx_rd == *tx_wr) {
            inst[0] &= 0xffffff7fu;                                /* CR1: clear TXEIE */
            inst[0] |= 0x40u;                                      /* CR1: set TCIE   */
        } else {
            uint8_t b = tx_ring[*tx_rd];
            uint16_t next = (uint16_t)(*tx_rd + 1);
            *tx_rd = next;
            if (next > 0xfff) {
                *tx_rd = 0;
            }
            *(volatile uint16_t *)((uint8_t *)inst + 0x28) = b;    /* TDR */
        }
    } else if ((isr_reg & 0x40) != 0 && (cr1 & 0x40) != 0) {
        /* TC with TCIE: transfer complete, return the line to idle. */
        inst[0] &= 0xffffffbfu;                                   /* CR1: clear TCIE */
        h[0x69] = 0x20;                                           /* gState = READY  */
    }
}

/*
 * USART1_IRQHandler — OEM FUN_08015e84 (HAL_UART_IRQHandler for the ch-1 link).
 *
 * Strong override of the weak Default_Handler alias for vector slot 27. Drives
 * the same channel-1 HAL UART handle (0x200007ac) and the same TX ring that
 * uart_ch1_tx_pump()/uart_flush_ch1() poll, plus a 20-byte RX ring at
 * 0x20000798 (wr index 0x2000084c). The handler inlines its own ring access
 * (it is not factored through uart_ch1_tx_pump), so this faithfully reproduces
 * that inline path against the identical SRAM addresses.
 *
 * USART1 regs @ Instance (huart[0]): CR1 +0x00, CR3 +0x08, ISR +0x1c, RDR +0x24,
 * TDR +0x28, ICR +0x20. Handle fields: gState +0x69, RxState +0x6a,
 * ErrorCode +0x6c. Masks/widths/wrap-at-0x13 disasm-confirmed.
 */
void USART1_IRQHandler(void)
{
    volatile uint32_t *huart = (volatile uint32_t *)0x200007ac;
    volatile uint32_t *inst  = (volatile uint32_t *)huart[0];   /* USART1 */

    volatile uint16_t * const rx_idx  = (volatile uint16_t *)0x2000084c;
    uint8_t           * const rx_ring = (uint8_t *)0x20000798;
    volatile uint16_t * const tx_tail = (volatile uint16_t *)0x2000084a;
    volatile uint16_t * const tx_head = (volatile uint16_t *)0x2000084e;
    uint8_t           * const tx_ring = (uint8_t *)0x20000784;

    volatile uint8_t  * const gstate  = (volatile uint8_t *)(0x200007ac + 0x69);
    volatile uint8_t  * const rxstate = (volatile uint8_t *)(0x200007ac + 0x6a);
    volatile uint32_t * const errcode = &huart[0x1b];           /* +0x6c */

    uint32_t isrflags   = inst[7];   /* ISR (+0x1c) */
    uint32_t cr1its     = inst[0];   /* CR1 (+0x00) */
    uint32_t errorflags = inst[2];   /* CR3 (+0x08), holds EIE here */

    if ((isrflags & 0xf) == 0) {
        /* No error flags pending: plain RXNE receive. */
        if ((isrflags & 0x20) != 0 && (cr1its & 0x20) != 0) {
            rx_ring[*rx_idx] = (uint8_t)*(volatile uint16_t *)((uint8_t *)inst + 0x24);
            uint16_t next = (uint16_t)(*rx_idx + 1);
            *rx_idx = next;
            if (next > 0x13) {
                *rx_idx = 0;
            }
        }
    } else if ((errorflags & 1) != 0 || (cr1its & 0x120) != 0) {
        /* Error path: latch active error sources into ErrorCode + clear them. */
        if ((isrflags & 1) != 0 && (cr1its & 0x100) != 0) {          /* PE + PEIE */
            *(volatile uint32_t *)((uint8_t *)inst + 0x20) = 1;       /* ICR.PECF */
            *errcode |= 1u;
        }
        if ((isrflags & 2) != 0 && (errorflags & 1) != 0) {          /* FE + EIE */
            *(volatile uint32_t *)((uint8_t *)inst + 0x20) = 2;       /* ICR.FECF */
            *errcode |= 4u;
        }
        if ((isrflags & 4) != 0 && (errorflags & 1) != 0) {          /* NE + EIE */
            *(volatile uint32_t *)((uint8_t *)inst + 0x20) = 4;       /* ICR.NCF */
            *errcode |= 2u;
        }
        if ((isrflags & 8) != 0 &&                                   /* ORE */
            ((cr1its & 0x20) != 0 || (errorflags & 1) != 0)) {       /* RXNEIE | EIE */
            *(volatile uint32_t *)((uint8_t *)inst + 0x20) = 8;       /* ICR.ORECF */
            *errcode |= 8u;
        }

        if (*errcode != 0) {
            if ((isrflags & 0x20) != 0 && (cr1its & 0x20) != 0) {    /* drain RXNE */
                rx_ring[*rx_idx] = (uint8_t)*(volatile uint16_t *)((uint8_t *)inst + 0x24);
                uint16_t next = (uint16_t)(*rx_idx + 1);
                *rx_idx = next;
                if (next > 0x13) {
                    *rx_idx = 0;
                }
            }
            if ((*errcode & 8u) == 0 && (inst[2] & 0x40) == 0) {
                /* Non-overrun, non-blocking (CR3.DMAR clear): nothing to abort. */
                *errcode = 0;
            } else {
                inst[0] &= 0xfffffedfu;   /* CR1: clear RXNEIE + PEIE */
                inst[2] &= 0xfffffffeu;   /* CR3: clear EIE */
                inst[0] |= 0x20u;         /* CR1: re-enable RXNEIE */
                *rxstate = 0x20;          /* RxState = READY */
            }
        }
    }

    if ((isrflags & 0x80) != 0 && (cr1its & 0x80) != 0) {            /* TXE + TXEIE */
        if (*tx_tail == *tx_head) {
            inst[0] &= 0xffffff7fu;       /* CR1: clear TXEIE */
            inst[0] |= 0x40u;             /* CR1: set TCIE */
        } else {
            uint16_t t = *tx_tail;
            uint8_t b = tx_ring[t];
            uint16_t next = (uint16_t)(t + 1);
            *tx_tail = next;
            if (next > 0x13) {
                *tx_tail = 0;
            }
            *(volatile uint16_t *)((uint8_t *)inst + 0x28) = (uint16_t)b;   /* TDR */
        }
    } else if ((isrflags & 0x40) != 0 && (cr1its & 0x40) != 0) {     /* TC + TCIE */
        inst[0] &= 0xffffffbfu;           /* CR1: clear TCIE */
        *gstate = 0x20;                   /* gState = READY */
    }
}
