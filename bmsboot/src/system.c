/* system.c — hardware bring-up: vectors, clocks, GPIO, watchdog, SysTick.
 *
 * Reconstructed from boot_hw_init (0x08000DBC), hal_init (0x08001EB4),
 * systick_config (0x08001F00), clock_periph_init (0x0800112C), gpio_init
 * (0x08000F58), led_init (0x08000E88), download_pin_check (0x08000F14) and
 * iwdg_hal_init (0x08001348).
 *
 * boot_hw_init() is the top-level reset bring-up. It relocates the vector table
 * into SRAM and points VTOR at it (the STM32L0 has a movable vector table),
 * raises the core voltage, brings up the clock tree + CRC unit, arms the
 * independent watchdog, unlocks flash, decides whether to start the serial
 * download server, and finally persists the RCC reset-cause word to EEPROM.
 *
 * The clock / GPIO / CRC / IWDG leaf drivers are vendor STM32L0 HAL (extern).
 */
#include "bmsboot.h"

/* CMSIS intrinsics */
static inline void disable_irq(void) { __asm volatile ("cpsid i" ::: "memory"); }
static inline void enable_irq(void)  { __asm volatile ("cpsie i" ::: "memory"); }
static inline void dsb(void)         { __asm volatile ("dsb 0xf" ::: "memory"); }

/* STM32L0 HAL leaves (vendor-stock). Return 0 == HAL_OK. */
extern uint32_t hal_rcc_osc_config(void *osc);
extern uint32_t hal_rcc_clock_config(void *clk, uint32_t flash_latency);
extern uint32_t hal_rcc_periph_clk_config(void *periph);
extern uint32_t hal_crc_init(void *hcrc);
extern uint32_t hal_iwdg_init(void *hiwdg);
extern void     hal_flash_unlock(void);
extern void     hal_flash_unlock2(void);
extern void     hal_gpio_init(uint32_t port, void *gpio);
extern void     hal_gpio_write(uint32_t port, uint16_t pins, int state);
extern int      hal_gpio_read(uint32_t port, uint16_t pin);
extern uint32_t systick_set_reload(uint32_t ticks);   /* CMSIS SysTick_Config  */
extern void     nvic_set_priority(int irqn, uint32_t pre, uint32_t sub);
extern void    *g_hcrc;                                /* CRC_HandleTypeDef     */

#define SysTick_IRQn   (-1)
#define HAL_RETRY      0x32u   /* 50 attempts before fail-safe */

uint32_t SystemCoreClock = 32000000u;   /* set by the clock config (PLL @ 32 MHz) */
static uint32_t s_tick_freq = 1u;        /* 1 kHz tick */
static uint32_t s_systick_clksrc;
static uint32_t s_reset_flags;           /* 0x20000888 — saved RCC_CSR */

/* ---- 1 ms SysTick (CMSIS HAL_InitTick) ---- */
int systick_config(uint32_t prio)
{
    uint32_t reload = SystemCoreClock / (1000u / s_tick_freq);
    if (systick_set_reload(reload) != 0)        /* reload too large for 24-bit  */
        return 1;
    if (prio >= 4u)
        return 1;
    nvic_set_priority(SysTick_IRQn, prio, 0);
    s_systick_clksrc = prio;
    return 0;
}

/* ---- HAL_Init: flash prefetch + SysTick ---- */
void hal_init(void)
{
    REG32(FLASH_R_BASE) |= 0x2u;                /* FLASH_ACR.PRFTEN */
    if (systick_config(3) == 0) {
        /* systick_clksrc_config() — empty HAL hook in the OEM */
    }
}

/* ---- clock tree + CRC unit (HSE/PLL via HAL, retry + fail-safe) ---- */
void clock_periph_init(void)
{
    uint32_t osc[0x38 / 4];
    uint32_t clk[0x14 / 4];
    uint32_t periph[0x24 / 4];
    uint8_t  tries;

    mem_set(osc, 0, sizeof osc);
    mem_set(clk, 0, sizeof clk);
    mem_set(periph, 0, sizeof periph);

    REG32(PWR_BASE) = (0xFFFFE7FFu & REG32(PWR_BASE)) | 0x800u;   /* VOS range 1 */

    osc[0]  = 10u;          /* RCC_OscInitTypeDef.OscillatorType */
    osc[12] = 1u;           /* +0x30 */
    osc[11] = 0x10u;        /* +0x2C */
    osc[10] = 1u;           /* +0x28 */
    osc[5]  = 2u;           /* +0x14 */
    osc[3]  = 0x40000u;     /* +0x0C  PLL config */
    osc[2]  = 0x400000u;    /* +0x08 */
    tries = 0;
    do {
        if (hal_rcc_osc_config(osc) == 0) tries = HAL_RETRY;
        else if (++tries > (HAL_RETRY - 1)) failsafe();
    } while (tries < HAL_RETRY);

    clk[0] = 0xFu;          /* ClockType SYSCLK|HCLK|PCLK1|PCLK2 */
    clk[1] = 1u;
    tries = 0;
    do {
        if (hal_rcc_clock_config(clk, 1u) == 0) tries = HAL_RETRY;
        else if (++tries > (HAL_RETRY - 1)) failsafe();
    } while (tries < HAL_RETRY);

    periph[0] = 1u;
    periph[1] = 2u;
    tries = 0;
    do {
        if (hal_rcc_periph_clk_config(periph) == 0) tries = HAL_RETRY;
        else if (++tries > (HAL_RETRY - 1)) failsafe();
    } while (tries < HAL_RETRY);

    /* CRC unit: enable its clock, then HAL_CRC_Init (used by image_verify) */
    *(volatile uint32_t *)g_hcrc = CRC_BASE;    /* hcrc->Instance */
    ((volatile uint8_t *)g_hcrc)[0x20] = 3u;    /* default polynomial/init */
    REG32(RCC_BASE + RCC_AHBENR) |= 0x1000u;    /* CRCEN */
    tries = 0;
    do {
        if (hal_crc_init(g_hcrc) == 0) tries = HAL_RETRY;
        else if (++tries > (HAL_RETRY - 1)) failsafe();
    } while (tries < HAL_RETRY);
}

/* ---- IWDG: HAL_IWDG_Init (prescaler /16, reload 0x908) ---- */
void iwdg_hal_init(void)
{
    static uint32_t hiwdg[0x10 / 4];
    uint8_t tries;

    hiwdg[0] = IWDG_BASE;   /* Instance        */
    hiwdg[1] = 4u;          /* Init.Prescaler  */
    hiwdg[2] = 0x908u;      /* Init.Reload     */
    hiwdg[3] = 0xFFFu;      /* Init.Window     */

    tries = 0;
    do {
        if (hal_iwdg_init(hiwdg) == 0) tries = HAL_RETRY;
        else if (++tries > (HAL_RETRY - 1)) failsafe();
    } while (tries < HAL_RETRY);

    REG32(IWDG_BASE) = IWDG_KR_RELOAD;          /* start: write the reload key */
}

/* ---- board GPIO setup ---- */
void gpio_init(void)
{
    gpio_cfg_t cfg;
    mem_set(&cfg, 0, sizeof cfg);

    /* GPIOA/B/C/H port clocks */
    REG32(RCC_BASE + RCC_IOPENR) |= 0x01u;      /* GPIOA */
    REG32(RCC_BASE + RCC_IOPENR) |= 0x02u;      /* GPIOB */
    REG32(RCC_BASE + RCC_IOPENR) |= 0x04u;      /* GPIOC */
    REG32(RCC_BASE + RCC_IOPENR) |= 0x80u;      /* GPIOH */

    /* preset output levels, then configure (pin masks are the OEM literals) */
    hal_gpio_write(GPIOA_BASE, 0x0010u, 0);
    hal_gpio_write(GPIOA_BASE, 0x91CFu, 1);
    hal_gpio_write(GPIOB_BASE, 0x8287u, 0);
    hal_gpio_write(GPIOB_BASE, 0x7104u, 1);
    hal_gpio_write(GPIOH_BASE, 0x0002u, 1);

    cfg.pin = 0x2000u; cfg.mode = 0; cfg.pull = 0;             hal_gpio_init(GPIOC_BASE, &cfg);
    cfg.pin = 0x0001u; cfg.mode = 0; cfg.pull = 0;             hal_gpio_init(GPIOH_BASE, &cfg);
    cfg.pin = 0x0002u; cfg.mode = 1; cfg.pull = 0; cfg.speed = 3; hal_gpio_init(GPIOH_BASE, &cfg);
    cfg.pin = 0x911Fu; cfg.mode = 1; cfg.pull = 0; cfg.speed = 3; hal_gpio_init(GPIOA_BASE, &cfg);
    cfg.pin = 0x0E00u; cfg.mode = 0; cfg.pull = 0;             hal_gpio_init(GPIOA_BASE, &cfg);
    cfg.pin = 0xF387u; cfg.mode = 1; cfg.pull = 0; cfg.speed = 3; hal_gpio_init(GPIOB_BASE, &cfg);
    cfg.pin = 0x0C40u; cfg.mode = 0; cfg.pull = 0;             hal_gpio_init(GPIOB_BASE, &cfg);
}

/* ---- status LEDs off + clear loop flags ---- */
void led_init(void)
{
    g_loop_flags = 0;
    hal_gpio_write(GPIOB_BASE, 0x0002u, 0);     /* PB1 */
    hal_gpio_write(GPIOB_BASE, 0x0200u, 0);     /* PB9 */
    hal_gpio_write(GPIOB_BASE, 0x0004u, 0);     /* PB2 */
    hal_gpio_write(GPIOB_BASE, 0x0001u, 0);     /* PB0 */
}

/* ---- bring up the GPIO, then start the download server iff PA10 is high ---- */
void download_pin_check(void)
{
    gpio_init();
    if (hal_gpio_read(GPIOA_BASE, 0x400u) == 1)     /* PA10 asserted */
        download_uart_init();
    else
        comms_disable();                            /* hold the comms port down */
}

/* ---- top-level reset bring-up ---- */
void boot_hw_init(void)
{
    /* relocate the 48-entry (0xC0-byte) vector table into SRAM and point
     * VTOR at it — the STM32L0 has a movable vector table. */
    mem_copy_bytes((uint8_t *)VTOR_RAM, (const uint8_t *)BOOT_BASE, 0xC0);
    disable_irq();
    REG32(SCB_BASE + 0x08) = VTOR_RAM;          /* SCB->VTOR */
    dsb();
    enable_irq();

    hal_init();                                 /* flash prefetch + SysTick */

    REG32(PWR_BASE) = (0xFFFFE7FFu & REG32(PWR_BASE)) | 0x800u;   /* VOS range 1 */
    REG32(RCC_BASE + RCC_APB2ENR) |= 0x1u;              /* SYSCFGEN */
    REG32(RCC_BASE + RCC_APB1ENR) |= 0x10000000u;       /* PWREN    */

    s_reset_flags = REG32(RCC_BASE + RCC_CSR);  /* save reset cause */
    REG32(RCC_BASE + RCC_CSR) |= 0x800000u;     /* RMVF: clear reset flags */

    clock_periph_init();
    comms_rx_state_init();
    iwdg_hal_init();
    REG32(IWDG_BASE) = IWDG_KR_RELOAD;          /* kick the watchdog */

    hal_flash_unlock();
    hal_flash_unlock2();

    download_pin_check();

    uint32_t flags = s_reset_flags;
    flash_program((uint8_t *)RESET_CAUSE_ADDR, 4, (const uint8_t *)&flags);  /* persist */
}
