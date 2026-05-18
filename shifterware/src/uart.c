/* uart.c — USART1 driver. The shifterware uses USART1 as the Modbus
 * RTU PHY to talk to the bike's main module.
 *
 * Register map for the MM32F031 USART (rebuilt from OEM image; see
 * `usart_t` in mm32f031.h):
 *   0x00 = TDR (TX data, write only)
 *   0x04 = RDR (RX data, read only)
 *   0x08 = SR  (status; bit 0 = TX ready/complete)
 *   0x0C = ISR (interrupt status; bit 1 = RX data available)
 *   0x10 = IER (interrupt enable)
 *   0x14 = ICR (interrupt clear — write bit to clear)
 *   0x18 = CCR (channel control; bit 0 = UE, bits 0-4 framing)
 *   0x1C = FCR (frame config; parity/stop/data-bits)
 *   0x20 = BRR_INT, 0x24 = BRR_FRA (baud, integer + fractional split)
 *
 * Pin map (OEM-confirmed): TX = PB6 AF0, RX = PB7 AF0.
 *
 * `uart.c.bak` holds the pre-decomp speculative version. */

#include "uart.h"
#include "gpio.h"
#include "hal.h"
#include "mm32f031.h"

static volatile uint8_t  s_rx_buf[UART_RX_BUFFER_SIZE];
static volatile uint16_t s_rx_head;
static volatile uint16_t s_rx_tail;

/* ---- USART HAL leaves (file-static; match OEM bytes) ---------------- */

/* OEM @ 0x08005CA0 (6 B). Write a byte to TDR. */
static void usart_write_data(usart_t *u, uint8_t b)
{
    u->TDR = (uint32_t)b;
}

/* OEM @ 0x08005CAE (16 B). Test bits in SR (offset 0x08). */
static bool usart_test_flag(usart_t *u, uint32_t mask)
{
    return (u->SR & mask) != 0u;
}

/* OEM @ 0x08005CC4 (20 B). Test bits in ISR (offset 0x0C). */
static bool usart_check_status(usart_t *u, uint32_t mask)
{
    return (u->ISR & mask) != 0u;
}

/* OEM @ 0x08005CA6 (8 B). Read one byte from RDR (offset 0x04). */
static uint8_t usart_read_data(usart_t *u)
{
    return (uint8_t)(u->RDR & 0xFFu);
}

/* OEM @ 0x08005CD8 (4 B). Write `mask` to ICR (offset 0x14) to clear
 * the matching interrupt-status bits. */
static void usart_clear_flag(usart_t *u, uint32_t mask)
{
    u->ICR = mask;
}

/* USART config struct passed to usart_init. Field semantics unknown
 * without the MM32 reference manual; the OEM-observed values for
 * USART1 (Modbus link) are programmed verbatim by uart1_init. */
typedef struct {
    uint32_t baud;          /* 0x00 */
    uint16_t fcr_a;         /* 0x04 — OR'd into FCR (offset 0x1C) */
    uint16_t fcr_b;         /* 0x06 — OR'd into FCR */
    uint16_t fcr_c;         /* 0x08 — OR'd into FCR */
    uint16_t ccr_a;         /* 0x0A — OR'd into CCR (offset 0x18) */
    uint16_t ccr_b;         /* 0x0C — OR'd into CCR */
} usart_config_t;

#define USART_PCLK_HZ   48000000u

/* OEM @ 0x08005BD2 (116 B). */
static void usart_init(usart_t *u, const usart_config_t *cfg)
{
    u->FCR = (u->FCR & ~0xCFu) | cfg->fcr_a | cfg->fcr_b | cfg->fcr_c;
    u->CCR = (u->CCR & ~0x1Fu) | cfg->ccr_a | cfg->ccr_b;

    const uint32_t divisor = USART_PCLK_HZ / cfg->baud;
    u->BRR_INT = divisor >> 4;
    u->BRR_FRA = divisor & 0xFu;
}

/* OEM @ 0x08005C78 (20 B). Toggle bits in IER. */
static void usart_ier_bits(usart_t *u, uint32_t mask, int enable)
{
    if (enable) u->IER |= mask;
    else        u->IER &= ~mask;
}

/* OEM @ 0x08005C60 (24 B). Toggle UE (CCR bit 0). */
static void usart_set_enable(usart_t *u, int enable)
{
    if (enable) u->CCR |= 1u;
    else        u->CCR &= ~1u;
}

/* NVIC + RCC HAL leaves now live in hal.c (shared with timer.c and
 * main.c). See `include/hal.h`. */

/* ---- GPIO HAL ------------------------------------------------------
 *
 * Direct-offset writes because the speculative `gpio_t` struct in
 * mm32f031.h follows the STM32F0 layout (separate MODER/OTYPER/
 * OSPEEDR/PUPDR, each 2 bits per pin). The OEM's GPIO is STM32F1-
 * style: CRL/CRH at offsets 0x00/0x04, 4 bits per pin encoding
 * MODE+CNF; BSRR @ 0x10, BRR @ 0x14. A proper gpio_t fix is plan-2
 * territory. */

/* GPIO_CRL/CRH/BSRR/BRR offsets moved to gpio.c (which is where the
 * `gpio_pin_configure` consumer also lives now). */
#define GPIO_AFRL_OFFS   0x20u
#define GPIO_AFRH_OFFS   0x24u

/* OEM @ 0x08004E22 (70 B). Write the 4-bit AF nibble for `pin` into
 * AFRL (pins 0-7) or AFRH (pins 8-15). */
static void gpio_set_af(void *port, int pin, int af)
{
    char *base = (char *)port;
    volatile uint32_t *reg = (volatile uint32_t *)
        (base + ((pin < 8) ? GPIO_AFRL_OFFS : GPIO_AFRH_OFFS));
    const uint32_t shift = (uint32_t)(pin & 7) * 4u;
    *reg = (*reg & ~(0xFu << shift)) | (((uint32_t)af & 0xFu) << shift);
}

/* `gpio_pin_configure` moved to gpio.c (also used by main's
 * `boot_init_gpio`). See `include/gpio.h`. */

/* ---- Public API ----------------------------------------------------- */

/* OEM @ 0x08004130 (150 B). USART1 init for the Modbus link.
 *   USART1 = 0x40013800, GPIOB = 0x48000400, TX=PB6/AF0, RX=PB7/AF0,
 *   IRQ27 priority 3. Framing bit-values 0x30/0x18 preserved verbatim. */
void uart1_init(uint32_t baud)
{
    rcc_apb2en_bits((1u << 14), 1);     /* USART1EN */
    rcc_ahben_bits ((1u << 18), 1);     /* GPIOB clock */

    const nvic_cfg_t nvic = { .irq = 27, .priority = 3, .enable = 1 };
    nvic_configure(&nvic);

    /* PB6 (USART1_TX) and PB7 (USART1_RX): AF0 and STM32F1-style mode
     * config via the OEM's GPIO HAL. */
    void *const gpiob = (void *)0x48000400u;
    gpio_set_af(gpiob, 6, 0);
    gpio_set_af(gpiob, 7, 0);

    /* PB6 = TX: output 50MHz alt-push-pull (CRL nibble 0xB = 1011b). */
    gpio_pin_cfg_t cfg_tx = { .pin_mask = 0x0040u, .mode_extra = 0x3u, .flags = 0x18u };
    gpio_pin_configure(gpiob, &cfg_tx);

    /* PB7 = RX: input floating (CRL nibble 0x4 = 0100b). */
    gpio_pin_cfg_t cfg_rx = { .pin_mask = 0x0080u, .mode_extra = 0u, .flags = 0x04u };
    gpio_pin_configure(gpiob, &cfg_rx);

    usart_config_t cfg = {
        .baud   = baud,
        .fcr_a  = 0x30u,
        .ccr_a  = 0x18u,
    };
    usart_init(USART1, &cfg);

    /* OEM also touches IER twice with masks 0x40 and 0x80. */
    usart_ier_bits(USART1, 0x40u, 1);
    usart_ier_bits(USART1, 0x80u, 4);

    usart_set_enable(USART1, 1);
}

/* OEM @ 0x0800371E (28 B). */
void uart1_send_byte(uint8_t b)
{
    usart_write_data(USART1, b);
    while (!usart_test_flag(USART1, USART_SR_TX_READY_Msk)) {
        /* wait for TX-ready */
    }
}

size_t uart1_send(const uint8_t *data, size_t len)
{
    for (size_t i = 0u; i < len; i++) {
        uart1_send_byte(data[i]);
    }
    return len;
}

bool uart1_rx_available(void)
{
    return s_rx_head != s_rx_tail;
}

uint8_t uart1_rx_byte(void)
{
    while (!uart1_rx_available()) { /* spin */ }
    const uint16_t tail = s_rx_tail;
    const uint8_t  b    = s_rx_buf[tail];
    s_rx_tail = (uint16_t)((tail + 1u) % UART_RX_BUFFER_SIZE);
    return b;
}

size_t uart1_rx_drain(uint8_t *dst, size_t max_len)
{
    size_t n = 0u;
    while (n < max_len && uart1_rx_available()) {
        dst[n++] = uart1_rx_byte();
    }
    return n;
}

/* OEM @ 0x0800450C (62 B). RX-side of USART1's IRQ.
 *
 * The OEM appends each byte to a linear 45-byte scratch at
 * `0x200001B2` indexed by `*(uint32_t*)0x200000E4`. Once the index
 * reaches the maximum (45), further bytes are silently dropped — no
 * wrap. Each byte also resets the inter-byte wait counter at
 * `0x200000DC` (consumed by `modbus_rx_poll` as the end-of-frame
 * timeout source). The static ring buffer / `uart1_rx_*` API earlier
 * in this file is speculative pre-decomp scaffolding for
 * `protocol.c`; it is not on the OEM RX path. */
void USART1_IRQHandler(void)
{
    if (!usart_check_status(USART1, USART_ISR_RX_READY_Msk)) {
        return;
    }
    usart_clear_flag(USART1, USART_ISR_RX_READY_Msk);

    const uint8_t b = usart_read_data(USART1);

    volatile uint32_t *const rx_head    = (volatile uint32_t *)0x200000E4u;
    volatile uint8_t  *const rx_scratch = (volatile uint8_t  *)0x200001B2u;
    volatile uint32_t *const wait_ctr   = (volatile uint32_t *)0x200000DCu;

    if (*rx_head < 0x2Du) {
        rx_scratch[*rx_head] = b;
        *rx_head = *rx_head + 1u;
        *wait_ctr = 0u;
    }
}
