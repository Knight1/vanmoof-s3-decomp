/* system.c — clock / IWDG / RTC-backup / GPIO bring-up and the persisted
 * upgrade flag.
 *
 * Reconstructed from boot_hw_init (0x080018CC), clock_periph_init (0x08001C08),
 * iwdg_init (0x08001E4C), gpio_init (0x08001A68), comms_rx_state_init
 * (0x08002214), boot_read_persistent_flags (0x08001948), store_boot_flag
 * (0x080020FC), rtc_bkp_read (0x080048AA) and rtc_bkp_write (0x0800487A).
 *
 * Clock / RTC / UART / GPIO setup is HAL orchestration (vendor-stock); only the
 * sequencing and the VanMoof-specific values are reproduced here. The HAL
 * leaves are declared extern and resolve once the STM32F0 HAL is vendored in.
 */
#include "powerbankboot.h"

/* ---- STM32F0 HAL leaves (vendor-stock) ---- */
extern int  hal_rcc_osc_config(void *osc);
extern int  hal_rcc_clock_config(void *clk, uint32_t flatency);
extern int  hal_rccex_periphclk_config(void *periph);
extern int  hal_rtc_init(void *hrtc);
extern int  hal_uart_init(void *huart);
extern void hal_iwdg_init(void *hiwdg);
extern void hal_gpio_init(uint32_t port, void *gpio);
extern void hal_init(void);                   /* FUN_08002B88 = HAL_Init         */
/* flash_lock (FUN_080033F4 = HAL_FLASH_Lock) and comms_uart_init / gpio_write_pins
 * are declared in powerbankboot.h. */

/* RTC / IWDG handles (OEM SRAM 0x20000B94 / 0x20000B54). The backup helpers
 * address the RTC registers directly, so the handle is carried only to match
 * the OEM signatures. */
static void *g_hrtc;
static uint8_t g_upgrade_finished_inv;        /* RAM mirror of ~flag             */

/* IWDG: LSI/64, reload 1250 (~2 s), window disabled. */
static struct { uint32_t instance, prescaler, reload, window; } g_hiwdg;

/* ===================================================================== */
/* Watchdog                                                              */
/* ===================================================================== */
void iwdg_init(void)
{
    g_hiwdg.instance  = IWDG_BASE;
    g_hiwdg.prescaler = 4;            /* IWDG_PRESCALER_64                       */
    g_hiwdg.reload    = 0x4E2;        /* 1250 ticks                              */
    g_hiwdg.window    = 0xFFF;        /* window disabled                         */
    hal_iwdg_init(&g_hiwdg);
    iwdg_refresh();                   /* first kick                              */
}

/* iwdg_refresh() — write the reload key to IWDG_KR (the recurring 0xAAAA). */
void iwdg_refresh(void)
{
    REG32(IWDG_BASE) = IWDG_KR_RELOAD;
}

/* ===================================================================== */
/* RTC backup registers — persisted "upgrade finished" flag              */
/* ===================================================================== */
uint32_t rtc_bkp_read(void *hrtc, int idx)
{
    (void)hrtc;
    return REG32(RTC_BASE + 0x50 + (uint32_t)idx * 4);   /* RTC_BKPxR @ +0x50    */
}

void rtc_bkp_write(void *hrtc, int idx, uint32_t v)
{
    (void)hrtc;
    REG32(RTC_BASE + 0x50 + (uint32_t)idx * 4) = v;
}

/* store_boot_flag() — persist the flag and its complement (BKP0R / BKP1R). */
void store_boot_flag(uint8_t v)
{
    g_upgrade_finished     = v;
    g_upgrade_finished_inv = (uint8_t)~v;
    rtc_bkp_write(g_hrtc, BKP_UPGRADE,     v);
    rtc_bkp_write(g_hrtc, BKP_UPGRADE_INV, ~(uint32_t)v);
}

/* boot_read_persistent_flags() — load + redundancy-check the upgrade flag. */
void boot_read_persistent_flags(void)
{
    uint32_t flag = rtc_bkp_read(g_hrtc, BKP_UPGRADE);
    uint32_t inv  = rtc_bkp_read(g_hrtc, BKP_UPGRADE_INV);
    g_upgrade_finished     = (uint8_t)flag;
    g_upgrade_finished_inv = (uint8_t)inv;

    if (flag != ~inv) {              /* corrupt pair -> clear to a safe 0        */
        g_upgrade_finished = 0;
        store_boot_flag(0);
    }

    comms_uart_init();                            /* bring up the USART2 comms bus */
    gpio_write_pins(GPIOA_BASE, 0x1000, 0);       /* drive PA12 low (BRR)          */
}

/* ===================================================================== */
/* Comms RX state                                                        */
/* ===================================================================== */
void comms_rx_state_init(void)
{
    /* reset the comms framing counters (a value + complement pair) */
    extern uint32_t g_comms_a, g_comms_b, g_comms_c, g_comms_d;
    g_comms_a = 0;
    g_comms_b = 0;
    g_comms_c = 0;
    g_comms_d = 0xFFFFFFFFu;
}

/* ===================================================================== */
/* Clock + peripheral bring-up                                           */
/* ===================================================================== */
/* HAL config structs are modelled loosely (the HAL owns the real layout); the
 * values below are the ones the OEM programs. */
void clock_periph_init(void)
{
    /* enable PWR + backup-domain access, ready the RTC clock source */
    REG32(RCC_BASE + 0x1C) |= 0x10000000u;   /* APB1ENR.PWREN                   */
    REG32(RCC_BASE + 0x20) |= 0x18u;          /* BDCR: LSE drive bits            */

    /* HSE + PLL -> 48 MHz SYSCLK, then HCLK/PCLK, then RTC (LSE) + USART1 */
    uint8_t osc[0x34], clk[0x10], periph[0x1C];
    for (unsigned i = 0; i < sizeof osc; i++) osc[i] = 0;
    for (unsigned i = 0; i < sizeof clk; i++) clk[i] = 0;
    for (unsigned i = 0; i < sizeof periph; i++) periph[i] = 0;

    if (hal_rcc_osc_config(osc) != 0)            stl_failsafe();
    if (hal_rcc_clock_config(clk, 1) != 0)       stl_failsafe();
    if (hal_rccex_periphclk_config(periph) != 0) { dbg_printf("PeriphClkInit NG\n\r"); stl_failsafe(); }

    REG32(RCC_BASE + 0x20) |= 0x8000u;        /* BDCR.RTCEN                       */

    extern uint8_t g_hrtc_obj[0x20];
    g_hrtc = g_hrtc_obj;
    if (hal_rtc_init(g_hrtc) != 0)               stl_failsafe();

    extern uint8_t g_huart1[0x6C];               /* debug-console UART (USART1)     */
    *(uint32_t *)g_huart1 = USART1_BASE;          /* handle->Instance = USART1       */
    if (hal_uart_init(g_huart1) != 0)            stl_failsafe();
    g_huart1[0x69] = 0x20u;                       /* gState = HAL_UART_STATE_READY   */
}

/* gpio_write_pins() (0x08003978) — atomically set (level != 0 -> BSRR) or clear
 * (level == 0 -> BRR) the masked pins on a GPIO port. */
void gpio_write_pins(uint32_t port, uint16_t mask, uint8_t level)
{
    if (level == 0)
        REG32(port + 0x28) = mask;                /* GPIO_BRR  — reset */
    else
        REG32(port + 0x18) = mask;                /* GPIO_BSRR — set   */
}

void gpio_init(void)
{
    /* enable GPIO port clocks (AHBENR: IOPA/B/C/F) */
    REG32(RCC_BASE + 0x14) |= 0x20000u | 0x40000u | 0x80000u | 0x400000u;

    /* The OEM then drives initial output levels via gpio_write_pins() and sets
     * pin modes via hal_gpio_init(); the board pin masks / AF selections live in
     * the .data init image (DAT_08001BEC..08001C04) and are not yet decoded. */
}

/* ===================================================================== */
/* Top-level bring-up                                                    */
/* ===================================================================== */
void boot_hw_init(void)
{
    REG32(FLASH_R_BASE + 0x00) |= 0x10u;          /* FLASH_ACR.PRFTBE (prefetch)   */
    hal_init();                                   /* HAL_Init: prefetch + tick + Msp*/
    REG32(RCC_BASE + 0x18) |= 0x1u;               /* APB2ENR.SYSCFGEN              */
    REG32(RCC_BASE + 0x1C) |= 0x10000000u;        /* APB1ENR.PWREN                */
    REG32(RCC_BASE + 0x24) |= 0x1000000u;         /* CSR.RMVF (clear reset flags) */
    clock_periph_init();
    comms_rx_state_init();
    iwdg_init();
    flash_lock();                                 /* HAL_FLASH_Lock (re-lock flash)*/
    gpio_init();
}
