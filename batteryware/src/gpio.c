#include "batteryware.h"
#include <stdbool.h>

/*
 * GPIO atomic bit write using BSRR (set) and BRR (reset) registers.
 *
 * On STM32L0:
 *   GPIOx_BSRR (offset 0x18) — writing 1 to bit[N] sets pin N
 *   GPIOx_BRR  (offset 0x28) — writing 1 to bit[N] resets pin N
 *
 * Both registers are write-only and atomic — no read-modify-write needed.
 */
void gpio_bit_write(uint32_t gpio_base, uint16_t pin_bit, uint8_t value)
{
    if (value == 0) {
        *(volatile uint32_t *)(gpio_base + 0x28) = (uint32_t)pin_bit;
    } else {
        *(volatile uint32_t *)(gpio_base + 0x18) = (uint32_t)pin_bit;
    }
}

/*
 * GPIO input read via IDR register (offset 0x10).
 * Returns true if the pin is high.
 */
bool gpio_bit_read(uint32_t gpio_base, uint16_t pin_bit)
{
    return (*(volatile uint32_t *)(gpio_base + 0x10) & (uint32_t)pin_bit) != 0;
}

/*
 * GPIO pin configuration.
 *
 * Configures GPIO pins using a 20-byte configuration array and a GPIO
 * port base address. The config array contains per-pin settings packed
 * as 2 bytes per pin (10 pins max).
 *
 * Writes the packed configuration to the GPIO port's MODER, OTYPER,
 * OSPEEDR, and PUPDR registers.
 */
/*
 * gpio_pin_config — multi-register GPIO pin programmer.
 *
 * OEM: FUN_0800F7E4 (732 B).
 *
 * Iterates over the bits set in `cfg[0]` (pin mask). For each selected
 * pin index `i`, writes some or all of:
 *   GPIOx_MODER     (always)
 *   GPIOx_OTYPER    (if mode is 1, 2, 17, or 18 — i.e. output/AF
 *                    with-or-without the OTYPE bit set)
 *   GPIOx_OSPEEDR   (same condition as OTYPER)
 *   GPIOx_PUPDR     (always)
 *   GPIOx_AFRL/H    (if mode is 2 or 18 — alternate-function modes)
 *   SYSCFG_EXTICRx  (if cfg[1] bit 28 — EXTI block enable)
 *   EXTI->IMR/EMR/RTSR/FTSR  (each gated by its own bit in cfg[1])
 *
 * cfg layout (5×u32 = 20 bytes; same buffer reused by callers):
 *   cfg[0]  pin mask (which pins to configure)
 *   cfg[1]  mode word — see bitfield below
 *   cfg[2]  PUPDR value (0/1/2)
 *   cfg[3]  OSPEEDR value (0..3)
 *   cfg[4]  AF number (0..15) — only used when cfg[1] selects an AF mode
 *
 * cfg[1] bitfield:
 *   [1:0]   MODER bits  (00=input, 01=output, 10=AF, 11=analog)
 *   [4]     OTYPER bit  (0=push-pull, 1=open-drain)
 *   [16]    EXTI IMR enable
 *   [17]    EXTI EMR enable
 *   [20]    EXTI RTSR enable (rising)
 *   [21]    EXTI FTSR enable (falling)
 *   [28]    EXTI block-enable trigger (all of [16..21] are conditional
 *           on this — when [28]=0 the EXTI/SYSCFG path is skipped)
 *
 * If [28] is set, the SYSCFG clock is enabled (RCC->APB2ENR |= 1),
 * SYSCFG_EXTICRx is updated to point the relevant EXTI line at this
 * GPIO port (port codes: A=0, B=1, C=2, D=3, E=4, H=5, other=6),
 * then EXTI->IMR/EMR/RTSR/FTSR are each cleared at this line and
 * re-set if the corresponding cfg[1] enable bit is on.
 *
 * The previous decomp interpreted cfg as a byte-packed 20-byte array
 * with `bits[1:0]=mode, bits[5:4]=speed, bits[7:6]=pupd` per pin — that
 * was guesswork and doesn't match OEM at all.
 */
void gpio_pin_config(uint32_t *gpio_base, gpio_pin_cfg_t *cfg)
{
    volatile uint32_t * const SYSCFG       = (volatile uint32_t *)0x40010000;
    volatile uint32_t * const EXTI         = (volatile uint32_t *)0x40010400;
    volatile uint32_t * const RCC_APB2ENR  = (volatile uint32_t *)0x40021034;

    volatile uint32_t *gp = (volatile uint32_t *)gpio_base;
    const uint32_t mode_lo4 = cfg->mode & 0xFU;

    for (uint32_t i = 0; (cfg->pin_mask >> i) != 0; i++) {
        const uint32_t pin_mask = cfg->pin_mask & (1U << i);
        if (pin_mask == 0) {
            continue;
        }

        const uint32_t shift2 = i * 2;

        /* --- OSPEEDR + OTYPER ---------------------------------------
         * Only configured for output (1) / AF (2) modes, with or
         * without the OTYPE_OD bit set (so {1, 2, 0x11, 0x12}).
         * Other modes (input/analog) ignore both registers. */
        if (mode_lo4 == GPIO_MODE_OUTPUT ||
            mode_lo4 == GPIO_MODE_AF     ||
            mode_lo4 == (GPIO_MODE_OUTPUT | GPIO_OTYPE_OD) ||
            mode_lo4 == (GPIO_MODE_AF     | GPIO_OTYPE_OD)) {

            uint32_t v = gp[2];                              /* OSPEEDR (+0x08) */
            v &= ~(3U << shift2);
            v |=  (cfg->speed << shift2);
            gp[2] = v;

            v  = gp[1];                                       /* OTYPER  (+0x04) */
            v &= ~(1U << i);
            v |= (((cfg->mode & GPIO_OTYPE_OD) ? 1U : 0U) << i);
            gp[1] = v;
        }

        /* --- PUPDR (always) ----------------------------------------- */
        {
            uint32_t v = gp[3];                               /* PUPDR (+0x0C) */
            v &= ~(3U << shift2);
            v |=  (cfg->pupd << shift2);
            gp[3] = v;
        }

        /* --- AFRL / AFRH (alternate-function modes only) ------------ */
        if (mode_lo4 == GPIO_MODE_AF ||
            mode_lo4 == (GPIO_MODE_AF | GPIO_OTYPE_OD)) {

            const uint32_t afr_idx   = (i >> 3) + 8;           /* AFRL=8 (+0x20), AFRH=9 (+0x24) */
            const uint32_t afr_shift = (i & 7) * 4;
            uint32_t v = gp[afr_idx];
            v &= ~(0xFU << afr_shift);
            v |=  (cfg->af << afr_shift);
            gp[afr_idx] = v;
        }

        /* --- MODER (always) ----------------------------------------- */
        {
            uint32_t v = gp[0];                                /* MODER (+0x00) */
            v &= ~(3U << shift2);
            v |=  ((cfg->mode & 3U) << shift2);
            gp[0] = v;
        }

        /* --- SYSCFG_EXTICRx + EXTI ---------------------------------- */
        if ((cfg->mode & GPIO_EXTI_ENABLE) == 0) {
            continue;
        }

        *RCC_APB2ENR |= 1U;                                    /* SYSCFG clock */

        uint32_t port_code;
        if      (gpio_base == (uint32_t *)0x50000000) port_code = 0;  /* GPIOA */
        else if (gpio_base == (uint32_t *)0x50000400) port_code = 1;  /* GPIOB */
        else if (gpio_base == (uint32_t *)0x50000800) port_code = 2;  /* GPIOC */
        else if (gpio_base == (uint32_t *)0x50000C00) port_code = 3;  /* GPIOD */
        else if (gpio_base == (uint32_t *)0x50001000) port_code = 4;  /* GPIOE */
        else if (gpio_base == (uint32_t *)0x50001C00) port_code = 5;  /* GPIOH */
        else                                          port_code = 6;

        const uint32_t exticr_idx   = (i >> 2) + 2;            /* EXTICR1..4 at SYSCFG[2..5] */
        const uint32_t exticr_shift = (i & 3) * 4;
        {
            uint32_t v = SYSCFG[exticr_idx];
            v &= ~(0xFU << exticr_shift);
            v |=  (port_code << exticr_shift);
            SYSCFG[exticr_idx] = v;
        }

        /* EXTI->IMR / EMR / RTSR / FTSR — clear-then-conditionally-set */
        struct { uint32_t bit; volatile uint32_t *reg; } trig[] = {
            { GPIO_EXTI_IMR,  &EXTI[0] },
            { GPIO_EXTI_EMR,  &EXTI[1] },
            { GPIO_EXTI_RTSR, &EXTI[2] },
            { GPIO_EXTI_FTSR, &EXTI[3] },
        };
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t v = *trig[k].reg;
            v &= ~pin_mask;
            if (cfg->mode & trig[k].bit) v |= pin_mask;
            *trig[k].reg = v;
        }
    }
}

/*
 * GPIO pin reset — reset mode for multiple pins.
 *
 * Iterates through each bit in pin_mask. For each bit set:
 *   1. Maps gpio_base to a port index (0-5 for GPIOA-F, 6=other)
 *   2. If the port matches in the MODER register, clears BSRR/BRR
 *      output registers and sets the MODER field to 0 (input mode)
 *   3. Configures OSPEEDR, PUPDR, and OTYPER for the pin
 *
 * Used to deconfigure GPIO pins after USART/modem operations.
 */
void gpio_pin_reset(uint32_t *gpio_base, uint32_t pin_mask)
{
    volatile uint32_t * const s_mode_reg  = (volatile uint32_t *)0x200024FC;
    uint32_t i;

    for (i = 0; pin_mask >> (i & 0xFF) != 0; i++) {
        uint32_t bit = pin_mask & (1U << (i & 0xFF));
        if (bit == 0) continue;

        int port_idx;
        if (gpio_base == (uint32_t *)0x50000000) {
            port_idx = 0;
        } else if (gpio_base == (uint32_t *)0x50000400) {
            port_idx = 1;
        } else if (gpio_base == (uint32_t *)0x50000800) {
            port_idx = 2;
        } else if (gpio_base == (uint32_t *)0x50000C00) {
            port_idx = 3;
        } else if (gpio_base == (uint32_t *)0x50001000) {
            port_idx = 4;
        } else if (gpio_base == (uint32_t *)0x50001400) {
            port_idx = 5;
        } else {
            port_idx = 6;
        }

        volatile uint32_t *mode = (volatile uint32_t *)(s_mode_reg[(i >> 2) + 2]);
        uint32_t shift = (i & 3) << 2;
        if ((mode[0] & (0xFU << shift)) == ((uint32_t)port_idx << shift)) {
            gpio_base[6] &= ~bit;   /* BSRR */
            gpio_base[7] &= ~bit;   /* BRR */
            gpio_base[8] &= ~bit;   /* clear output */
            gpio_base[9] &= ~bit;
            mode[0] &= ~(0xFU << shift);
        }

        gpio_base[0] |= 3U << ((i & 0x7F) << 1);         /* OSPEEDR */
        gpio_base[(i >> 3) + 8] &= ~(0xFU << ((i & 7) << 2));  /* AFR */
        gpio_base[3] &= ~(3U << ((i & 0x7F) << 1));       /* PUPDR */
        gpio_base[1] &= ~(1U << (i & 0xFF));               /* OTYPER */
        gpio_base[2] &= ~(3U << ((i & 0x7F) << 1));       /* MODER */
    }
}

/*
 * Word-to-bytes — unpack 32-bit words into byte stream.
 *
 * Copies 'byte_count' bytes from the word array at src to dst.
 * Each 32-bit word is split into 4 bytes (little-endian order).
 * Stops when byte_count bytes have been written.
 */
void word_to_bytes(uint32_t *src, uint16_t byte_count, int dst)
{
    uint16_t offset = 0;
    uint32_t *s = src;
    while (offset < byte_count) {
        uint32_t w = *s++;
        if (offset < byte_count) {
            *(volatile uint8_t *)(dst + offset) = (uint8_t)w;
            offset++;
        }
        if (offset < byte_count) {
            *(volatile uint8_t *)(dst + offset) = (uint8_t)(w >> 8);
            offset++;
        }
        if (offset < byte_count) {
            *(volatile uint8_t *)(dst + offset) = (uint8_t)(w >> 16);
            offset++;
        }
        if (offset < byte_count) {
            *(volatile uint8_t *)(dst + offset) = (uint8_t)(w >> 24);
            offset++;
        }
    }
}

/*
 * bytes_to_words — stream a byte buffer to a single destination as
 * big-endian-packed 32-bit chunks (with partial-byte tail handling).
 *
 * OEM: FUN_0800EFF8 (290 B). Called by `dma_transfer_irq` to push a
 * caller-supplied byte buffer into a peripheral data register (SPI or
 * USART) one 4-byte big-endian chunk at a time.
 *
 * Signature:
 *   `dst_pp` is a POINTER-TO-POINTER — `**dst_pp` is the actual write
 *   target (caller stores a field address that itself points at e.g.
 *   `SPI->DR`). OEM dereferences twice (`r3 = *(u32 *)dst_pp;
 *   *(u32 *)r3 = word`) so the destination is **re-loaded every
 *   iteration**, but the pointer in `*dst_pp` is never modified by
 *   this function — every chunk hits the same target, which is the
 *   key signal that the destination is a peripheral data register
 *   that latches each write rather than a memory buffer.
 *
 * Packing is big-endian throughout:
 *   - Full word:  `(src[i*4]<<24) | (src[i*4+1]<<16) | (src[i*4+2]<<8) | src[i*4+3]`
 *   - 2-byte:     `(src[i*4]<<8)  | src[i*4+1]`  written as halfword
 *   - 3-byte:     halfword as above, then `*(u8 *)dst = src[i*4+2]`
 *                 (overwrites the low byte of the halfword — OEM does
 *                 this literally; the side-effect-on-peripheral-write
 *                 model is what makes it sensible)
 *   - 1-byte:     `*(u8 *)dst = src[i*4]`
 *
 * Returns `**dst_pp` (the final word/halfword/byte latched).
 *
 * The previous decomp had two bugs:
 *   1. signature took `uint32_t *dst` instead of `uint32_t **dst_pp`
 *   2. case 2/3 used **little-endian** halfword packing while the
 *      full-word path used big-endian — inconsistent, and wrong vs OEM
 */
uint32_t bytes_to_words(uint32_t **dst_pp, const uint8_t *src, uint32_t byte_count)
{
    uint32_t i;

    for (i = 0; i < (byte_count >> 2); i++) {
        uint32_t word =
            ((uint32_t)src[i * 4    ] << 24) |
            ((uint32_t)src[i * 4 + 1] << 16) |
            ((uint32_t)src[i * 4 + 2] << 8 ) |
            ((uint32_t)src[i * 4 + 3]      );
        *(volatile uint32_t *)(*dst_pp) = word;
    }

    if ((byte_count & 3) != 0) {
        if ((byte_count & 3) == 1) {
            *(volatile uint8_t *)(*dst_pp) = src[i * 4];
        }
        if ((byte_count & 3) == 2) {
            uint16_t halfword =
                (uint16_t)(((uint32_t)src[i * 4    ] << 8) |
                            (uint32_t)src[i * 4 + 1]);
            *(volatile uint16_t *)(*dst_pp) = halfword;
        }
        if ((byte_count & 3) == 3) {
            uint16_t halfword =
                (uint16_t)(((uint32_t)src[i * 4    ] << 8) |
                            (uint32_t)src[i * 4 + 1]);
            *(volatile uint16_t *)(*dst_pp) = halfword;
            *(volatile uint8_t  *)(*dst_pp) = src[i * 4 + 2];
        }
    }

    return *(volatile uint32_t *)(*dst_pp);
}

/*
 * gpio_init_buttons — GPIO power-on configuration.
 *
 * OEM: FUN_08007D78 (430 B). Faithful translation of the OEM sequence:
 *
 *   1. Zero a 20-byte stack `cfg` block (memset_byte_fill).
 *   2. Enable GPIOA/GPIOB/GPIOC/GPIOH clocks via RCC->IOPENR (offset
 *      0x2C from RCC base, **not** 0x34 — APB2ENR — as the previous
 *      source had it). Each enable is followed by a verify-read dance
 *      `RCC->IOPENR & bit` stored to a stack local — looks like
 *      leftover scaffolding for a wait-for-clock pattern (the result
 *      is never inspected). Replicated here for fidelity; GCC may
 *      optimise the dead reads away.
 *   3. Five `gpio_bit_write` calls drive initial pin states **before**
 *      configuring pin modes:
 *         GPIOA[ pin 4              ] = LOW
 *         GPIOA[ 0x91CF mask        ] = HIGH (pins 0,1,2,3,6,7,8,11,12,15)
 *         GPIOB[ 0x287 mask         ] = LOW  (pins 0,1,2,7,9)
 *         GPIOB[ 0xF104 mask        ] = HIGH (pins 2,8,12,13,14,15)
 *         GPIOH[ pin 1              ] = HIGH
 *   4. Seven `gpio_pin_config` calls program MODER/OTYPER/OSPEEDR/PUPDR
 *      using the SAME 20-byte cfg buffer with different fields:
 *         cfg[0] = pin mask
 *         cfg[1] = mode word (analog/output/AF; 0x10210000 is a
 *                  multi-pin packed setting whose internal decoding
 *                  belongs to gpio_pin_config)
 *         cfg[2] = otype/speed
 *         cfg[3] = pull bits (`3` = pull-down on most calls)
 *      The cfg buffer is NOT re-zeroed between calls — later calls
 *      inherit unchanged fields. The 7 calls target:
 *         GPIOC pin 13       (cfg[1] = 0x10210000)
 *         GPIOB pin 0        (cfg[1] = 0x10210000)
 *         GPIOB pin 1        (cfg[1] = 1, cfg[3] = 3)
 *         GPIOA mask 0x911F  (cfg[1] = 1, cfg[3] = 3)
 *         GPIOA mask 0xC00   (cfg[1] = 0)
 *         GPIOB mask 0xF387  (cfg[1] = 1, cfg[3] = 3)
 *         GPIOB mask 0xC40   (cfg[1] = 0)
 *
 * The previous decomp was guesswork:
 *   - Wrong RCC register (APB2ENR @ +0x34 instead of IOPENR @ +0x2C)
 *   - Wrong ports (GPIOD instead of GPIOH)
 *   - Only 4 calls instead of 7
 *   - Identical templated cfg for all (OEM uses 5 different patterns)
 *   - Missing the 5 initial gpio_bit_write calls
 *   - Inline MODER/PUPDR writes for PB9/PB13/PA15 that don't appear in OEM
 */
void gpio_init_buttons(void)
{
    gpio_pin_cfg_t cfg;
    memset_byte_fill((uint8_t *)&cfg, 0, sizeof cfg);

    /* RCC->IOPENR (0x4002102C) — enable GPIOA(bit0), GPIOB(bit1),
     * GPIOC(bit2), GPIOH(bit7). The "store-bit-to-stack-then-read"
     * scaffolding is preserved for fidelity (it's dead code semantically
     * but lives in OEM, possibly intended as a sync barrier). */
    {
        volatile uint32_t * const iopenr = (volatile uint32_t *)0x4002102C;
        volatile uint32_t check_a, check_b, check_c, check_h;

        *iopenr |= 0x01;  check_a = *iopenr & 0x01;  (void)check_a;
        *iopenr |= 0x02;  check_b = *iopenr & 0x02;  (void)check_b;
        *iopenr |= 0x04;  check_c = *iopenr & 0x04;  (void)check_c;
        *iopenr |= 0x80;  check_h = *iopenr & 0x80;  (void)check_h;
    }

    /* Initial pin drives — BEFORE pin-mode configuration. */
    gpio_bit_write(0x50000000, 0x0010, 0);   /* GPIOA  PA4                       LOW  */
    gpio_bit_write(0x50000000, 0x91CF, 1);   /* GPIOA  PA0/1/2/3/6/7/8/11/12/15  HIGH */
    gpio_bit_write(0x50000400, 0x0287, 0);   /* GPIOB  PB0/1/2/7/9               LOW  */
    gpio_bit_write(0x50000400, 0xF104, 1);   /* GPIOB  PB2/8/12/13/14/15         HIGH */
    gpio_bit_write(0x50001C00, 0x0002, 1);   /* GPIOH  PH1                       HIGH */

    /* The OEM reuses the same cfg buffer across all 7 calls, mutating
     * fields between calls; calls 5 and 7 inherit `speed` from the
     * preceding call (immaterial since both target input pins). */

    /* PC13 — power button (falling-edge EXTI). */
    cfg.pin_mask = 0x2000;
    cfg.mode     = GPIO_MODE_INPUT | GPIO_EXTI_ENABLE | GPIO_EXTI_IMR | GPIO_EXTI_FTSR;
    cfg.pupd     = GPIO_PUPD_NONE;
    gpio_pin_config((uint32_t *)0x50000800, &cfg);

    /* PB0 — second button (falling-edge EXTI). */
    cfg.pin_mask = 0x0001;
    cfg.mode     = GPIO_MODE_INPUT | GPIO_EXTI_ENABLE | GPIO_EXTI_IMR | GPIO_EXTI_FTSR;
    cfg.pupd     = GPIO_PUPD_NONE;
    gpio_pin_config((uint32_t *)0x50000400, &cfg);

    /* PB1 — output (very-high speed). */
    cfg.pin_mask = 0x0002;
    cfg.mode     = GPIO_MODE_OUTPUT;
    cfg.pupd     = GPIO_PUPD_NONE;
    cfg.speed    = GPIO_SPEED_VHIGH;
    gpio_pin_config((uint32_t *)0x50000400, &cfg);

    /* GPIOA mask 0x911F — outputs (very-high speed). */
    cfg.pin_mask = 0x911F;
    cfg.mode     = GPIO_MODE_OUTPUT;
    cfg.pupd     = GPIO_PUPD_NONE;
    cfg.speed    = GPIO_SPEED_VHIGH;
    gpio_pin_config((uint32_t *)0x50000000, &cfg);

    /* GPIOA mask 0x0C00 — inputs; .speed inherits from previous call. */
    cfg.pin_mask = 0x0C00;
    cfg.mode     = GPIO_MODE_INPUT;
    cfg.pupd     = GPIO_PUPD_NONE;
    gpio_pin_config((uint32_t *)0x50000000, &cfg);

    /* GPIOB mask 0xF387 — outputs (very-high speed). */
    cfg.pin_mask = 0xF387;
    cfg.mode     = GPIO_MODE_OUTPUT;
    cfg.pupd     = GPIO_PUPD_NONE;
    cfg.speed    = GPIO_SPEED_VHIGH;
    gpio_pin_config((uint32_t *)0x50000400, &cfg);

    /* GPIOB mask 0x0C40 — inputs; .speed inherits from previous call. */
    cfg.pin_mask = 0x0C40;
    cfg.mode     = GPIO_MODE_INPUT;
    cfg.pupd     = GPIO_PUPD_NONE;
    gpio_pin_config((uint32_t *)0x50000400, &cfg);
}
