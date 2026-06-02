#include "powerbankware.h"

/*
 * clock_rtc_init — OEM FUN_080136c0.
 *
 * System clock tree + RTC bring-up, called from hal_bringup. Enables backup-
 * domain write access, sets the LSE drive strength, runs the HAL oscillator /
 * peripheral-clock / system-clock config, enables the RTC clock, then inits the
 * RTC peripheral and recomputes SystemCoreClock.
 *
 * The three HAL init structs are zeroed with the project mem_set (an explicit
 * call — a `= {0}` initialiser would lower to a libc memset that this -nostdlib
 * build cannot link). Struct field offsets, constants and call arity below are
 * disasm-confirmed against the OEM image.
 */

extern void FUN_0801b51c(void);                 /* HAL_PWR_EnableBkUpAccess (PWR_CR DBP) */
extern int  FUN_0801b538(void *osc);            /* HAL_RCC_OscConfig */
extern int  FUN_0801bbf8(void *pclk, int sel);  /* HAL_RCCEx_PeriphCLKConfig */
extern int  FUN_0801bf4c(void *clk);            /* HAL_RCC_ClockConfig */
extern int  FUN_0801c150(void *hrtc);           /* HAL_RTC_Init */
extern void FUN_0801d9c0(void);                 /* SystemCoreClockUpdate */
extern void FUN_0800fe3a(void);                 /* OEM error handler */

void clock_rtc_init(void)
{
    uint32_t osc[13];   /* RCC_OscInitTypeDef     (0x34) */
    uint32_t pclk[4];   /* RCC_PeriphCLKInitTypeDef (0x10) */
    uint32_t clk[7];    /* RCC_ClkInitTypeDef     (0x1c) */

    mem_set(osc, 0, sizeof osc);
    mem_set(pclk, 0, sizeof pclk);
    mem_set(clk, 0, sizeof clk);

    FUN_0801b51c();                                       /* PWR_CR |= DBP */
    *(volatile uint32_t *)(0x40021000 + 0x20) |= 0x18u;   /* RCC_BDCR: LSEDRV = 0b11 */

    osc[0]  = 0x0000000du;   /* OscillatorType = HSE | LSE | LSI */
    osc[1]  = 1;
    osc[2]  = 1;
    osc[5]  = 0;
    osc[6]  = 0x10;
    osc[7]  = 1;
    osc[4]  = 0x10;
    osc[9]  = 2;
    osc[10] = 0x00010000u;
    osc[11] = 0;
    osc[12] = 0;
    *(volatile uint32_t *)0x20002614 = 0;                 /* reset ms tick */
    if (FUN_0801b538(osc) != 0) { FUN_0800fe3a(); }

    pclk[0] = 7;
    pclk[1] = 2;
    pclk[2] = 0;
    pclk[3] = 0;
    if (FUN_0801bbf8(pclk, 1) != 0) { FUN_0800fe3a(); }

    clk[0] = 0x00010003u;
    clk[2] = 0;
    clk[3] = 0;
    clk[1] = 0x100;
    if (FUN_0801bf4c(clk) != 0) { FUN_0800fe3a(); }

    *(volatile uint32_t *)(0x40021000 + 0x20) |= 0x8000u; /* RCC_BDCR: RTCEN */

    /* RTC handle @0x200006f0 (shared with rtc.c): Instance = RTC (0x40002800). */
    volatile uint32_t *hrtc = (volatile uint32_t *)0x200006f0;
    hrtc[0] = 0x40002800u;
    hrtc[1] = 0;
    hrtc[2] = 0x7f;
    hrtc[3] = 0xff;
    hrtc[4] = 0;
    hrtc[5] = 0;
    hrtc[6] = 0;
    if (FUN_0801c150((void *)hrtc) != 0) { FUN_0800fe3a(); }

    FUN_0801d9c0();          /* SystemCoreClockUpdate */
}
