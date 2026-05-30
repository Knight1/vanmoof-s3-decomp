#include "batteryware.h"

/* System tick counter — SRAM struct at 0x200047E0, field at offset 0x14 */
static volatile uint32_t * const s_tick_counter = (volatile uint32_t *)0x200047E0;

/*
 * Read the current system tick counter (milliseconds since boot).
 * Writes the value to *out and returns 0.
 */
uint32_t get_tick_ms(uint32_t *out)
{
    *out = s_tick_counter[0x14 / 4];
    return 0;
}

/* Additional tick counter at SRAM 0x200047DC */
static volatile uint32_t * const s_tick2 = (volatile uint32_t *)0x200047DC;

/*
 * Read the secondary system tick counter.
 *
 * Used pervasively as the general-purpose tick reference
 * (timers, flash operations, DMA polls).
 */
uint32_t tick_get(void)
{
    return *s_tick2;
}

/* Raw tick counter at SRAM 0x200000C8 */
static volatile uint32_t * const s_tick_raw = (volatile uint32_t *)0x200000C8;

/*
 * Read the raw hardware tick counter.
 *
 * Used by clock-prescaler-dependent functions (fg_read_field_8/11)
 * to determine APB prescaler scaling factors.
 */
uint32_t tick_counter_read(void)
{
    return *s_tick_raw;
}

/*
 * Clock prescaler value computation.
 *
 * Reads RCC at 0x40021000, extracts the APB clock configuration
 * from CFGR (bits 2-3 for SWS, bits 18-19 for PPRE2, bits 16-17
 * for PPRE1, bit 4 for HSE). Depending on the current system
 * clock source and prescaler settings, returns a clock divider
 * value used for baud rate or timer calculations:
 *
 *   - SWS=3 (PLL): shift-table lookup × multiplier ÷ divisor
 *   - SWS=2 (HSE): returns fixed values 0x3D0900 or 0xF42400
 *   - SWS=1 (MSI): returns 0x007A1200
 *   - otherwise: returns 0x8000 << (prescaler_shift + 1)
 */
int32_t clock_prescaler_val(void)
{
    volatile uint32_t * const RCC = (volatile uint32_t *)0x40021000;
    const uint8_t * const s_shift_tbl = PLLMulTable;  /* OEM flash 0x08018200 */
    /* constant table at 0x08010920-0x08010928 */
    const uint32_t MAGIC_MSI_ALT = 0x003D0900;  /* DAT_08010920 */
    const uint32_t MAGIC_MSI     = 0x00F42400;  /* DAT_08010924 */
    const uint32_t MAGIC_HSE     = 0x007A1200;  /* DAT_08010928 */

    uint32_t cfgr = RCC[3];
    uint32_t sws  = cfgr & 0xC;

    if (sws == 0xC) {
        uint8_t shift  = s_shift_tbl[(cfgr >> 18) & 0xF];
        int32_t divisor = ((cfgr >> 22) & 3) + 1;

        if ((cfgr & 0x10000) == 0) {
            if ((RCC[0] & 0x10) == 0) {
                return (int32_t)(((uint32_t)shift * MAGIC_MSI) / (uint32_t)divisor);
            } else {
                return (int32_t)(((uint32_t)shift * MAGIC_MSI_ALT) / (uint32_t)divisor);
            }
        } else {
            return (int32_t)(((uint32_t)shift * MAGIC_HSE) / (uint32_t)divisor);
        }
    }

    if (sws < 0xD) {
        if (sws == 4) {
            if ((RCC[0] & 0x10) != 0) {
                return (int32_t)MAGIC_MSI_ALT;
            }
            return (int32_t)MAGIC_MSI;
        }
        if (sws == 8) {
            return (int32_t)MAGIC_HSE;
        }
    }

    return (int32_t)(0x8000U << (((RCC[1] >> 13) & 7) + 1));
}

/*
 * HAL_RCCEx_PeriphCLKConfig — peripheral-clock source selection.
 *
 * 'param' points to a 9-word config struct: param[0] is a selection
 * bitmask (RCC_PERIPHCLK_*), param[1] is the RTC clock selection, and
 * param[2..8] carry the per-peripheral clock-source fields.
 *
 *   - param[0] & 0x20 (RTC): enables PWREN (APB1ENR bit 28) and the
 *     PWR DBP bit (100-tick timeout), then drives the RTC clock domain
 *     in RCC_CSR (offset 0x50): if RTCSEL (bits 16-17) is already set
 *     to a different source, performs a backup-domain reset (RTCRST
 *     bit 19) and re-waits LSE (LSEON bit 8 / LSERDY bit 9, 5000-tick
 *     timeout). RTCPRE (CR bits 20-21) and RTCSEL are then written. If
 *     this routine enabled PWREN, it clears it again on the way out.
 *     Returns 1 if RTCSEL already differs while RTCPRE is locked.
 *   - param[0] bits 0/1/2/3/8/0x40/0x80: masked writes into RCC_CCIPR
 *     (offset 0x4C) for USART1, USART2, LPUART1, I2C1, LPTIM1, the
 *     bit-26 clk source, and bits 18-19 respectively.
 *
 * Returns 0 on success, 1 on immediate error, 3 on timeout.
 *
 * Register word offsets (RCC base 0x40021000): CR 0x00, APB1ENR 0x38,
 * CCIPR 0x4C, CSR 0x50; PWR_CR at 0x40007000.
 */
uint32_t rcc_reconfigure(uint32_t *param)
{
    volatile uint32_t * const RCC = (volatile uint32_t *)0x40021000;
    volatile uint32_t * const PWR = (volatile uint32_t *)0x40007000;
    bool dbp_was_clear = false;

    if ((*param & 0x20) != 0) {
        dbp_was_clear = (RCC[0x38 / 4] & 0x10000000) == 0;
        if (dbp_was_clear) {
            RCC[0x38 / 4] |= 0x10000000;
        }

        if ((PWR[0] & 0x100) == 0) {
            PWR[0] |= 0x100;
            uint32_t t1 = tick_get();
            while ((PWR[0] & 0x100) == 0) {
                uint32_t t2 = tick_get();
                if ((t2 - t1) > 100) {
                    return 3;
                }
            }
        }

        if ((((RCC[0] & 0x300000) != (param[1] & 0x300000)) &&
             ((param[1] & 0x30000) == 0x30000)) &&
            ((RCC[0] & 0x20000) == 0x20000)) {
            return 1;
        }

        if ((((RCC[0x50 / 4] & 0x30000) != 0) &&
             ((RCC[0x50 / 4] & 0x30000) != (param[1] & 0x30000))) &&
            ((*param & 0x20) != 0)) {

            uint32_t saved = RCC[0x50 / 4] & 0xFFFCFFFF;
            RCC[0x50 / 4] |= 0x80000;
            RCC[0x50 / 4] &= 0xFFF7FFFF;
            RCC[0x50 / 4] = saved;

            if ((saved & 0x100) != 0) {
                uint32_t t1 = tick_get();
                while ((RCC[0x50 / 4] & 0x200) == 0) {
                    uint32_t t2 = tick_get();
                    if ((t2 - t1) > 5000) {
                        return 3;
                    }
                }
            }
        }

        if ((param[1] & 0x30000) == 0x30000) {
            RCC[0] = (param[1] & 0x300000) | (RCC[0] & 0xFFCFFFFF);
        }
        RCC[0x50 / 4] = (param[1] & 0x30000) | RCC[0x50 / 4];

        if (dbp_was_clear) {
            RCC[0x38 / 4] &= 0xEFFFFFFF;
        }
    }

    /* Direct field updates via bitmask in *param */
    if ((*param & 1) != 0) {
        RCC[0x4C / 4] = param[2] | (RCC[0x4C / 4] & 0xFFFFFFFC);
    }
    if ((*param & 2) != 0) {
        RCC[0x4C / 4] = param[3] | (RCC[0x4C / 4] & 0xFFFFFFF3);
    }
    if ((*param & 4) != 0) {
        RCC[0x4C / 4] = param[4] | (RCC[0x4C / 4] & 0xFFFFF3FF);
    }
    if ((*param & 8) != 0) {
        RCC[0x4C / 4] = param[5] | (RCC[0x4C / 4] & 0xFFFFCFFF);
    }
    if ((*param & 0x100) != 0) {
        RCC[0x4C / 4] = param[6] | (RCC[0x4C / 4] & 0xFFFCFFFF);
    }
    if ((*param & 0x40) != 0) {
        RCC[0x4C / 4] = param[8] | (RCC[0x4C / 4] & 0xFBFFFFFF);
    }
    if ((*param & 0x80) != 0) {
        RCC[0x4C / 4] = param[7] | (RCC[0x4C / 4] & 0xFFF3FFFF);
    }

    return 0;
}

/*
 * Tick value getter — reads SRAM 0x200047E4.
 */
uint32_t tick_val_get(void)
{
    return *(volatile uint32_t *)0x200047E4;
}

/*
 * Tick ms getter — reads SRAM 0x200047FC.
 */
uint32_t tick_ms_get(void)
{
    return *(volatile uint32_t *)0x200047FC;
}

/*
 * Tick timeout getter — reads SRAM 0x20004814.
 */
uint32_t tick_timeout_get(void)
{
    return *(volatile uint32_t *)0x20004814;
}


/*
 * Timer scheduler (FUN_08014af8) — periodic timer with divisors.
 *
 * Increments two counters. At 1000ms: sets all flag bits (0x3F).
 * At 500ms: sets bits 0, 1, 3.
 * At 250ms: sets bits 0, 1, 2.
 * At 100ms: sets bits 0, 1.
 * At 10ms: sets bit 0.
 * Otherwise sets bit 0.
 */
void timer_scheduler(void)
{
    volatile uint32_t * const s_cnt = (volatile uint32_t *)0x20002B40;
    volatile uint16_t * const s_ms  = (volatile uint16_t *)0x20002B44;
    volatile uint32_t * const s_flags = (volatile uint32_t *)0x20002B48;

    *s_cnt += 1;
    uint16_t ms = *s_ms + 1;
    *s_ms = ms;

    if (ms < 1000) {
        extern uint32_t veneer_1558c(uint32_t, uint32_t);
        if ((uint32_t)(veneer_1558c(*s_ms, 500) & 0xFFFF) == 0) {
            *s_flags |= 0x1F;
        } else if ((uint32_t)(veneer_1558c(*s_ms, 250) & 0xFFFF) == 0) {
            *s_flags |= 0xB;
        } else if ((uint32_t)(veneer_1558c(*s_ms, 100) & 0xFFFF) == 0) {
            *s_flags |= 0x7;
        } else if ((uint32_t)(veneer_1558c(*s_ms, 10) & 0xFFFF) == 0) {
            *s_flags |= 0x3;
        } else {
            *s_flags |= 0x1;
        }
    } else {
        *s_ms = 0;
        *s_flags |= 0x3F;
    }
}
