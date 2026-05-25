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
    const uint8_t * const s_shift_tbl = (const uint8_t *)0x08018200;
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
