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
extern void FUN_0801d678(void *huart);   /* UART_AdvFeatureConfig (own pass) */
extern int  FUN_0801d244(void *huart);   /* UART_SetConfig: BRR + CR1/2/3 (own pass) */
extern int  FUN_0801d7e0(void *huart);   /* UART_CheckIdleState (own pass) */

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

    if (FUN_0801d244(h) == 1) {                     /* UART_SetConfig */
        return 1;
    }
    if (*(uint32_t *)(h + 0x24) != 0) {             /* AdvancedInit */
        FUN_0801d678(h);
    }

    inst[1] &= 0xffffb7ffu;                         /* CR2: clear CLKEN (bit11) */
    inst[2] &= 0xffffffd5u;                         /* CR3: clear SCEN/HDSEL/IREN */
    inst[0] |= 1u;                                  /* CR1.UE = 1 */

    return FUN_0801d7e0(h);                         /* UART_CheckIdleState */
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
