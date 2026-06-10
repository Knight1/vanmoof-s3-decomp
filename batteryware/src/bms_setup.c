/* bms_setup.c — BMS power-on initialisation.
 *
 * OEM: FUN_080078c8, runs once during boot. Sequence:
 *
 *   1. Zero a fixed set of single-byte and halfword status fields in
 *      SRAM (state timer, fault shadows, per-cell counters).
 *   2. Drive three GPIOC pins (PC2/PC9 low, PC15 high).
 *   3. Set bit 0x1000 in g_state_timer.
 *   4. Zero the 128-byte BMS config struct at 0x200029a8, then refill
 *      it from EEPROM at 0x08080c00.
 *   5. Zero a 56-byte secondary struct at 0x20002ad0 and refill from
 *      EEPROM at 0x08080c80.
 *   6. Call config_init() (parses the loaded EEPROM struct).
 *   7. Scale cfg[+0x06] (u16) into two large 32-bit calibration
 *      constants at 0x20002b50 / 0x20002b54.
 *   8. Call bms_init() (= FEDL5236 hardware init / fg_init).
 *   9. Load the charge/discharge current-cal gains from cfg[+0x3a]/[+0x3c]
 *      into 0x200025b2/0x200025b0; if either is out of (900,1099] reset both
 *      to 1000. Set fixed defaults at 0x200025be/0x200025c4/0x200025bc.
 *  10. Call rsoc_lookup().
 *  11. Branch on cfg[+0x28] (first-time-init flag):
 *        zero  → factory init: rewrite zeros, set defaults, store
 *                back to EEPROM via memcmp_verify (SPI poll on each
 *                byte).
 *        non-zero → already initialised: log calibration/voltage
 *                values via uart_printf and, if real_soc > stored,
 *                bump the stored RSOC and call veneer_11ef8.
 *  12. Cache cfg[+0x24] into cfg[+0x2c]; if it overflows 0x3840 take
 *      it mod 0x3840 via __aeabi_uidivmod.
 *  13. If cfg[+0x05] non-zero, delay 100ms; else delay 200ms.
 *  14. dma_stop(), set state byte at 0x200024e0 to 10 and the flag
 *      at 0x20002550 to 0, delay 10ms, then cell_balance_update().
 *
 * The OEM uses its private `memcpy@0x08009412` (non-standard
 * (src,u16 count,dst) ABI). We inline the byte copy locally — it's
 * the same effect, and avoids depending on whichever name that
 * routine gets given when its own decomp lands.
 *
 * NOTE: `memcmp_verify` is still the buggy byte-copy stub in nops.c
 * (see progress.md "Known bug"). We invoke it as the OEM does so the
 * structure is right; the SPI poll semantics will land with that fix.
 */

#include "batteryware.h"

extern void dma_stop(void);
extern void veneer_11ef8(uint32_t arg);

/* uart_printf format strings (OEM flash 0x080171f0..0x08017280), defined
 * in src/strings.c. The OEM passes a value as the %i/%d argument; our
 * uart_printf models the format pointer only, so the value args are
 * dropped pending variadic-printf support. */
static uint8_t * const s_fmt_max_voltage = (uint8_t *)s_max_cell_voltage;
static uint8_t * const s_fmt_min_voltage = (uint8_t *)s_min_cell_voltage;
static uint8_t * const s_fmt_total       = (uint8_t *)s_total_voltage;
static uint8_t * const s_fmt_real_rsoc   = (uint8_t *)s_real_rsoc;
static uint8_t * const s_fmt_record_soc  = (uint8_t *)s_record_soc;
static uint8_t * const s_fmt_rsoc_adj    = (uint8_t *)s_rsoc_adjust;

static void byte_copy(const uint8_t *src, uint16_t len, uint8_t *dst)
{
    /* OEM memcpy ABI: (src, u16 count, dst). Plain byte loop. */
    for (uint16_t i = 0; i < len; i++) {
        dst[i] = src[i];
    }
}

void bms_setup(void)
{
    /* SRAM scratch / status fields. */
    volatile uint32_t * const g_state_timer = (volatile uint32_t *)0x20002C00;
    volatile uint8_t  * const g_byte_2bfc   = (volatile uint8_t  *)0x20002BFC;
    volatile uint8_t  * const g_byte_2bfd   = (volatile uint8_t  *)0x20002BFD;
    volatile uint8_t  * const g_byte_2c73   = (volatile uint8_t  *)0x20002C73;
    volatile uint8_t  * const g_byte_2c49   = (volatile uint8_t  *)0x20002C49;
    volatile uint16_t * const g_hw_47d2     = (volatile uint16_t *)0x200047D2;
    volatile uint16_t * const g_word_2c0a   = (volatile uint16_t *)0x20002C0A;
    volatile uint16_t * const g_word_25a0   = (volatile uint16_t *)0x200025A0;
    volatile uint16_t * const g_word_2820   = (volatile uint16_t *)0x20002820;

    /* BMS config struct (128 B) — fields referenced by offset below. */
    volatile uint8_t  * const cfg           = (volatile uint8_t  *)0x200029A8;
    /* Secondary config (56 B). */
    volatile uint8_t  * const cfg2          = (volatile uint8_t  *)0x20002AD0;
    /* Sibling config block (used for cfg2_b[+5], cfg2_b[+6] reads). */
    volatile uint8_t  * const cfg2_b        = (volatile uint8_t  *)0x200028D0;

    /* Two large 32-bit calibration constants derived from cfg[+6]. */
    volatile uint32_t * const g_cal_a       = (volatile uint32_t *)0x20002B50;
    volatile uint32_t * const g_cal_b       = (volatile uint32_t *)0x20002B54;

    /* Charge / discharge current-calibration gains (per-mille; cfg+0x3a/+0x3c,
     * EEPROM 0x08080C3A/0x3C). fg_coulomb_update multiplies the measured pack
     * current by these /1000. Set by the CHG CAL / DSG CAL console commands.
     * (Previously mislabeled "voltage threshold hi/lo" — see docs/eeprom.md §5.) */
    volatile uint16_t * const g_chg_cal_gain = (volatile uint16_t *)0x200025B2;
    volatile uint16_t * const g_dsg_cal_gain = (volatile uint16_t *)0x200025B0;

    /* Misc byte flags written to fixed defaults. */
    volatile uint8_t  * const g_byte_25be   = (volatile uint8_t  *)0x200025BE;
    volatile uint8_t  * const g_byte_25c4   = (volatile uint8_t  *)0x200025C4;
    volatile uint8_t  * const g_byte_25bc   = (volatile uint8_t  *)0x200025BC;
    volatile uint8_t  * const g_byte_25bd   = (volatile uint8_t  *)0x200025BD;

    /* SOC trackers. */
    volatile uint32_t * const g_real_soc    = (volatile uint32_t *)0x200025AC;
    volatile uint16_t * const g_volt_max    = (volatile uint16_t *)0x200027FA;
    volatile uint16_t * const g_volt_min    = (volatile uint16_t *)0x2000282A;
    volatile uint16_t * const g_volt_total  = (volatile uint16_t *)0x2000281C;

    /* Two more BMS state bytes touched at the tail. */
    volatile uint8_t  * const g_state_24e0  = (volatile uint8_t  *)0x200024E0;
    volatile uint8_t  * const g_state_2550  = (volatile uint8_t  *)0x20002550;

    /* 1. Zero status. */
    *g_state_timer = 0;
    *g_byte_2bfc   = 0;
    *g_byte_2bfd   = 0;
    *g_byte_2c73   = 0;
    *g_byte_2c49   = 0;
    *g_hw_47d2     = 0;
    *g_word_2c0a   = 0;
    *g_word_25a0   = 0;
    *g_word_2820   = 0;

    /* 2. GPIOC: PC2 low, PC9 low, PC15 high. */
    gpio_bit_write(0x50000400, 0x0004, 0);
    gpio_bit_write(0x50000400, 0x0200, 0);
    gpio_bit_write(0x50000400, 0x8000, 1);

    /* 3. Set bit 0x1000 in g_state_timer (= 0x80 << 5). */
    *g_state_timer |= 0x1000U;

    /* 4. Zero + reload 128-byte cfg struct from EEPROM @ 0x08080c00. */
    for (int8_t i = 0; i >= 0; i++) {
        cfg[(uint8_t)i] = 0;
    }
    byte_copy((const uint8_t *)0x08080C00, 128, (uint8_t *)cfg);

    /* 5. Zero + reload 56-byte cfg2 from EEPROM @ 0x08080c80. */
    for (uint8_t i = 0; i <= 0x37; i++) {
        cfg2[i] = 0;
    }
    byte_copy((const uint8_t *)0x08080C80, 56, (uint8_t *)cfg2);
    /* OEM also zeros a third 56-byte block at 0x20002b10 (cfg3) — same loop pattern,
     * no following copy. Kept as a clear-only step. */
    for (uint8_t i = 0; i <= 0x37; i++) {
        ((volatile uint8_t *)0x20002B10)[i] = 0;
    }

    /* 6. Parse config. */
    config_init();

    /* 7. Calibration constants from cfg2_b[+6] (u16).
     *      g_cal_a = val * 14400        (= ((val*16 - val)*16 - val*15) << 6 = val*225*64)
     *      g_cal_b = val * 11520        (= ((val*2+val)*16 - val*3) << 8 = val*45*256)
     */
    uint32_t v = *(volatile uint16_t *)((uint8_t *)cfg2_b + 6);
    *g_cal_a = v * 14400U;
    *g_cal_b = v * 11520U;

    /* 8. FEDL5236 hardware init. */
    bms_init();

    /* 9. Load charge/discharge current-cal gains from cfg[+0x3a]/[+0x3c].
     *    Valid (901..1099); if EITHER is <= 0x384 (900) or > 0x44B (1099),
     *    reset BOTH to 0x3E8 (1000 = x1.000, no correction). */
    *g_chg_cal_gain = *(volatile uint16_t *)((uint8_t *)cfg + 0x3A);
    *g_dsg_cal_gain = *(volatile uint16_t *)((uint8_t *)cfg + 0x3C);
    if (*g_chg_cal_gain > 0x44B || *g_chg_cal_gain <= 0x384 ||
        *g_dsg_cal_gain > 0x44B || *g_dsg_cal_gain <= 0x384) {
        *g_chg_cal_gain = 0x3E8;  /* 1000 */
        *g_dsg_cal_gain = 0x3E8;
    }
    *g_byte_25be = 0;
    *g_byte_25c4 = 3;
    *g_byte_25bc = 3;

    /* 10. RSOC lookup. */
    rsoc_lookup();

    /* 11. First-time init vs already-initialised. */
    if (*(volatile uint32_t *)((uint8_t *)cfg + 0x28) == 0) {
        /* Factory init path: clear cfg again, copy current thresholds back,
         * set defaults, persist to EEPROM via memcmp_verify (SPI poll). */
        for (int8_t i = 0; i >= 0; i++) {
            cfg[(uint8_t)i] = 0;
        }
        *(volatile uint16_t *)((uint8_t *)cfg + 0x3A) = *g_chg_cal_gain;
        *(volatile uint16_t *)((uint8_t *)cfg + 0x3C) = *g_dsg_cal_gain;
        cfg[0x36] = 0;
        *(volatile uint32_t *)((uint8_t *)cfg + 0x24) = *g_real_soc;
        *(volatile uint32_t *)((uint8_t *)cfg + 0x28) =
            *(volatile uint16_t *)((uint8_t *)cfg2_b + 6);
        cfg[0x37] = 100;
        memcmp_verify((char *)0x08080C00, 128, (char *)cfg);
        for (uint8_t i = 0; i <= 0x37; i++) {
            cfg2[i] = 0;
        }
        memcmp_verify((char *)0x08080C80, 56, (char *)cfg2);
    } else {
        /* Already initialised: log voltages and SOC, possibly bump RSOC. */
        (void)s_fmt_max_voltage; (void)s_fmt_min_voltage; (void)s_fmt_total;
        (void)s_fmt_real_rsoc;   (void)s_fmt_record_soc;  (void)s_fmt_rsoc_adj;
        uart_printf((uint8_t *)(uintptr_t)*g_volt_max);
        uart_printf((uint8_t *)(uintptr_t)*g_volt_min);
        uart_printf((uint8_t *)(uintptr_t)*g_volt_total);
        uart_tx_flush();
        uart_printf((uint8_t *)(uintptr_t)*g_byte_25bd);
        uart_printf((uint8_t *)(uintptr_t)cfg[0x36]);

        if (cfg[0x36] > *g_byte_25bd) {
            uint8_t inc = *g_byte_25bd + 9;
            if (inc < cfg[0x36]) {
                uart_printf((uint8_t *)(uintptr_t)*g_byte_25bd);
                veneer_11ef8(*g_byte_25bd);
            }
        } else {
            uint8_t inc = cfg[0x36] + 9;
            if (inc < *g_byte_25bd) {
                uart_printf((uint8_t *)(uintptr_t)*g_byte_25bd);
                veneer_11ef8(*g_byte_25bd);
            }
        }
        uart_tx_flush();
    }

    /* 12. Cache cfg[+0x24] → cfg[+0x2c] (mod 0x3840 if it overflows). */
    uint32_t soc24 = *(volatile uint32_t *)((uint8_t *)cfg + 0x24);
    *(volatile uint32_t *)((uint8_t *)cfg + 0x2C) = soc24;
    if (soc24 >= 0x3840U) {
        *(volatile uint32_t *)((uint8_t *)cfg + 0x2C) = soc24 % 0x3840U;
    } else {
        *(volatile uint32_t *)((uint8_t *)cfg + 0x2C) = 0;
    }

    /* 13. Settle delay. */
    if (cfg2_b[5] != 0) {
        delay_ms(100);
    } else {
        delay_ms(200);
    }

    /* 14. Stop DMA, prime cell-balance state, kick balance once. */
    dma_stop();
    *g_state_24e0 = 10;
    *g_state_2550 = 0;
    delay_ms(10);
    cell_balance_update();
}
