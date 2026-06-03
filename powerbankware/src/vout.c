#include "powerbankware.h"

/*
 * Power-path output stage — bypass MOSFET control and the DAC-regulated Vout
 * set-point. Translated from this image (STM32F091xC). The bypass FET gate is
 * PA12 (GPIOA bit 0x1000), mirrored by mode-word (0x200006A0) bit 10 (0x400).
 * The DAC channel sits behind a TIM/DAC HAL handle whose pointer is stored at
 * 0x20000258; bit 16 of its first control word enables the channel.
 *
 *   bypass_fet_on   = OEM FUN_08013BB8
 *   bypass_fet_off  = OEM FUN_08013BE4
 *   vout_set_dac    = OEM FUN_0800A490
 *   vout_bypass_off = OEM FUN_0800A440
 *   vout_enable     = OEM FUN_0800A3FC
 *   vout_dac_apply  = OEM FUN_0800A520
 */

#define MODE  ((volatile uint16_t *)0x200006A0)

/* ── GPIO / bypass FET ───────────────────────────────────────────────── */
#define GPIOA_BASE   0x48000000u
#define PIN_PA12     0x1000u                      /* bypass MOSFET gate */

/* ── DAC channel HAL handle + Vout set-point shadow words ────────────── */
#define DAC_BASE        ((volatile uint32_t *)0x40007400u)        /* DAC peripheral   */
#define DAC_HANDLE_PTR  (*(volatile uint32_t **)0x20000258u)      /* DAC HAL handle   */
#define VOUT_RAW        (*(volatile uint16_t *)(0x20000254u + 2)) /* raw set-point    */
#define VOUT_SCALED     (*(volatile uint32_t *)(0x2000026Cu + 4)) /* scaled counts    */

/* DAC output-stage log strings (src/strings.c). */
extern const char s_dac_stop[];   /* "\nDAC_Stop()\r"      0x0801DBD0 */
extern const char s_dac_value[];  /* "\nDAC_Value= %l mV\r" 0x0801DBE0 */

/*
 * Push the staged DAC set-point into the TIM/DAC compare register: the scaled
 * shadow (0x2000026C+4) is shifted into the high half-word of CCR (handle+0x20),
 * where the channel's PWM/DAC compare is read from.
 */
void vout_dac_apply(void)
{
    *(volatile uint32_t *)((uint32_t)DAC_HANDLE_PTR + 0x20u) =
        VOUT_SCALED << 0x10;
}

void bypass_fet_on(void)
{
    *MODE |= 0x400u;
    gpio_bit_write(GPIOA_BASE, PIN_PA12, 1);   /* PA12 high */
}

void bypass_fet_off(void)
{
    *MODE &= 0xfbffu;                          /* clear bit 10 */
    gpio_bit_write(GPIOA_BASE, PIN_PA12, 0);   /* PA12 low */
}

/*
 * Program the Vout DAC for `target` (millivolts/units): stage the raw set-point
 * into the two shadow words, scale by 1000/0x325 (≈ counts-per-mV), push it to
 * the DAC compare registers, and log the value.
 */
void vout_set_dac(uint16_t target)
{
    VOUT_RAW = 0;
    VOUT_RAW += target;

    VOUT_SCALED = 0;
    VOUT_SCALED += target;
    VOUT_SCALED *= 1000u;
    VOUT_SCALED = ((VOUT_SCALED) / (0x325u));

    vout_dac_apply();
    log_print(2, s_dac_value, target);
    uart_flush();
}

/* Disable the DAC channel and announce DAC_Stop(), but only when the output
 * was enabled (mode bit 6); always clear the enable bit on the way out. */
void vout_bypass_off(void)
{
    if (*MODE & 0x40u) {                       /* mode bit 6 set */
        volatile uint32_t *h = DAC_HANDLE_PTR;
        h[0] &= 0xFFFEFFFFu;                   /* clear bit 16 (DAC channel) */
        log_print(2, s_dac_stop);
        uart_flush();
    }
    *MODE &= 0xFFBFu;                          /* clear bit 6 */
}

/* Enable the DAC channel, drive Vout to 0x4B0 (1200), and flag the output as
 * active (mode bit 6 set, bit 7 cleared). */
void vout_enable(void)
{
    volatile uint32_t *h = DAC_HANDLE_PTR;
    h[0] |= 0x10000u;                          /* set bit 16 (DAC channel) */
    vout_set_dac(0x4B0);
    *MODE |= 0x40u;                            /* set bit 6 */
    *MODE &= 0xFF7Fu;                          /* clear bit 7 */
}

/*
 * dac_init — OEM FUN_0800a310. One of board_init's peripheral sub-inits.
 * DAC channel-2 (Vout) bring-up: clock-enable DAC (APB1 bit29) + GPIOA, set PA5
 * analog, store the DAC base into the handle pointer at 0x20000258, clear the
 * channel-2 CR field, and apply an all-zero ChannelConfig (so the config-merge
 * just re-clears the field). Mode-word (0x200006A0) bit 6 left low. The DAC_CR
 * mask 0xf001ffff and the strh on the mode word are disasm-confirmed.
 */
void dac_init(void)
{
    volatile uint32_t * const rcc_apb1enr = (volatile uint32_t *)(0x40021000 + 0x1c);
    volatile uint32_t * const rcc_ahbenr  = (volatile uint32_t *)(0x40021000 + 0x14);

    gpio_pin_cfg_t cfg;
    uint32_t cfgch[2];               /* DAC_ChannelConfTypeDef, zeroed */
    mem_set(cfgch, 0, sizeof cfgch);
    mem_set(&cfg, 0, sizeof cfg);

    *rcc_apb1enr |= 0x20000000u;  (void)(*rcc_apb1enr & 0x20000000u);  /* DACEN  */
    *rcc_ahbenr  |= 0x00020000u;  (void)(*rcc_ahbenr  & 0x00020000u);  /* GPIOAEN */

    cfg.pin_mask = 0x20;             /* PA5 */
    cfg.mode     = GPIO_MODE_ANALOG;
    cfg.pupd     = 0;
    gpio_pin_config((uint32_t *)GPIOA_BASE, &cfg);

    DAC_HANDLE_PTR = DAC_BASE;        /* Instance = DAC */

    volatile uint32_t *dac = DAC_HANDLE_PTR;
    dac[0] &= 0xf001ffffu;                                       /* clear CH2 field */
    dac[0] = ((cfgch[0] | cfgch[1]) << 16) | (dac[0] & 0xf001ffffu);

    *MODE &= 0xffbfu;                /* mode bit 6 low */
}
