#include "powerbankware.h"

/*
 * bms_measure_update — OEM FUN_080089B0.
 *
 * Per-tick analog measurement + state-of-charge estimate. Runs at most 49
 * iterations of a sample window (counter 0x200001AC); on the first iteration it
 * primes the AFE (bms_measure_prime), and from the third — once the "sample ready"
 * flag (0x20000204 bit0) is set — it scales the three raw ADC words at
 * 0x20000208 into two cell voltages (0x200001AE, 0x20000202) and a pack-current
 * figure, then maps the current through the descending SOC lookup table to a
 * raw index (0x20000218), applies the signed SOC correction (0x2000020E), and
 * debounces the SOC-history high-water mark (record +0x7A). The window end
 * raises the AFE chip-select; each active pass drops it.
 *
 * The OEM uses 64-bit multiply/divide runtime helpers (FUN_0800845C /
 * FUN_0800841C); expressed here as int64_t arithmetic, which the toolchain
 * lowers to the same libgcc routines — behaviour-identical. The current divisor
 * (0x325AA0 - base) can go negative, hence the signed 64-bit type.
 */

/*
 * bms_measure_prime — OEM FUN_08008980. First-iteration AFE priming: clear the
 * "sample ready" flag (0x20000204 bit0) and the scratch byte 0x20000213, then
 * start the measurement ADC in interrupt mode (handle at 0x200001B4).
 */
/* ── Peripheral base / pin (AFE chip-select on GPIOB) ─────────────────── */
#define GPIOB_BASE     0x48000400u
#define AFE_CS_PIN     0x8000u                   /* PB15 */

/* ── Measurement window / sample-ready (see docs/hardware.md) ─────────── */
#define CNT (*(volatile uint16_t *)0x200001AC)   /* sample-window counter   */
#define CTL (*(volatile uint8_t  *)0x20000204)   /* sample-ready flag byte  */
#define RAW ((volatile uint16_t  *)0x20000208)   /* [0]=cell1 [1]=cell2 [2]=current */

/* ── AFE handle / scratch + computed measurement cells ───────────────── */
#define ADC_HANDLE   ((uint32_t *)0x200001B4)               /* measurement ADC HAL handle */
static volatile uint8_t  * const s_prime_scratch = (volatile uint8_t  *)0x20000213;
static volatile int16_t  * const s_cell1_mv      = (volatile int16_t  *)0x200001AE; /* cell1 mV */
static volatile int16_t  * const s_cell2_mv      = (volatile int16_t  *)0x20000202; /* cell2 mV */
static volatile uint8_t  * const s_soc_idx       = (volatile uint8_t  *)0x20000218; /* SOC index */
static volatile uint8_t  * const s_soc_corr      = (volatile uint8_t  *)0x2000020E; /* signed SOC correction (TS0 cell) */
static volatile uint8_t  * const s_soc_debounce  = (volatile uint8_t  *)0x2000020F; /* SOC high-water debounce */
static volatile uint32_t * const s_evtlog_en     = (volatile uint32_t *)0x2000072C; /* event-log enable/counter */

/* Record (0x200004D0) calibration / SOC-history fields, at the OEM widths. */
#define REC_IOUT_CAL (*(volatile int16_t  *)(0x200004D0u + 0x72)) /* +0x72 Iout cal (signed test) */
#define REC_IOUT_CALU (*(volatile uint16_t *)(0x200004D0u + 0x72)) /* +0x72 Iout cal (unsigned mul) */
#define REC_VOUT_CAL (*(volatile int16_t  *)(0x200004D0u + 0x70)) /* +0x70 Vout cal (signed test) */
#define REC_VOUT_CALU (*(volatile uint16_t *)(0x200004D0u + 0x70)) /* +0x70 Vout cal (unsigned mul) */
#define REC_SOC_HIST (*(volatile uint8_t  *)(0x200004D0u + 0x7A)) /* +0x7A SOC high-water mark    */

void bms_measure_prime(void)
{
    CTL &= 0xFEu;
    *s_prime_scratch = 0;
    adc_start_it(ADC_HANDLE);
}

/* Descending current->SOC-index lookup table (OEM 0x0801E820, 0x92 entries). */
static const uint32_t soc_lut[0x92] = {
    0x0002f2fe, 0x0002caa0, 0x0002a49d, 0x000280d3, 0x00025f21, 0x00023f66,
    0x00022184, 0x0002055c, 0x0001ead2, 0x0001d1cd, 0x0001ba32, 0x0001a3ed,
    0x00018ee6, 0x00017b09, 0x00016845, 0x00015689, 0x000145c5, 0x000135e9,
    0x000126e9, 0x000118b7, 0x00010b48, 0x0000fe90, 0x0000f285, 0x0000e71d,
    0x0000dc4f, 0x0000d212, 0x0000c85f, 0x0000bf2d, 0x0000b675, 0x0000ae31,
    0x0000a659, 0x00009ee8, 0x000097d8, 0x00009124, 0x00008ac5, 0x000084b9,
    0x00007ef9, 0x00007982, 0x0000744f, 0x00006f5d, 0x00006aa9, 0x0000662e,
    0x000061ea, 0x00005dda, 0x000059fa, 0x00005649, 0x000052c4, 0x00004f68,
    0x00004c34, 0x00004925, 0x00004639, 0x0000436f, 0x000040c5, 0x00003e39,
    0x00003bca, 0x00003976, 0x0000373c, 0x0000351c, 0x00003312, 0x00003120,
    0x00002f42, 0x00002d79, 0x00002bc4, 0x00002a21, 0x00002890, 0x00002710,
    0x0000259f, 0x0000243f, 0x000022ec, 0x000021a8, 0x00002072, 0x00001f48,
    0x00001e2a, 0x00001d18, 0x00001c11, 0x00001b15, 0x00001a23, 0x0000193b,
    0x0000185b, 0x00001785, 0x000016b8, 0x000015f2, 0x00001534, 0x0000147e,
    0x000013cf, 0x00001326, 0x00001284, 0x000011e9, 0x00001153, 0x000010c3,
    0x00001038, 0x00000fb2, 0x00000f32, 0x00000eb6, 0x00000e3f, 0x00000dcc,
    0x00000d5e, 0x00000cf3, 0x00000c8c, 0x00000c29, 0x00000bca, 0x00000b6e,
    0x00000b15, 0x00000abf, 0x00000a6c, 0x00000a1c, 0x000009cf, 0x00000985,
    0x0000093d, 0x000008f7, 0x000008b4, 0x00000873, 0x00000835, 0x000007f8,
    0x000007be, 0x00000785, 0x0000074e, 0x00000719, 0x000006e6, 0x000006b4,
    0x00000685, 0x00000656, 0x00000629, 0x000005fe, 0x000005d4, 0x000005ab,
    0x00000583, 0x0000055d, 0x00000538, 0x00000514, 0x000004f2, 0x000004d0,
    0x000004af, 0x00000490, 0x00000471, 0x00000454, 0x00000437, 0x0000041b,
    0x00000400, 0x000003e6, 0x000003cc, 0x000003b4, 0x0000039c, 0x00000385,
    0x0000036e, 0x00000358,
};

void bms_measure_update(void)
{
    volatile uint8_t *soc  = s_soc_idx;
    volatile uint8_t *corr = s_soc_corr;
    uint16_t cnt = CNT;
    CNT = cnt + 1;

    if ((uint16_t)(cnt + 1) >= 0x32) {
        CNT = 0;
        gpio_bit_write(GPIOB_BASE, AFE_CS_PIN, 1);     /* AFE CS high */
        return;
    }

    if (CNT == 1) {
        bms_measure_prime();
        return;
    }
    if (CNT <= 2 || (CTL & 1) == 0) {
        return;
    }
    CTL &= 0xFEu;

    int64_t acc;

    /* cell 1 voltage -> 0x200001AE */
    acc = (int64_t)RAW[0] * 0x325;
    acc = acc / 0xC5;
    acc = acc / 3;
    if (REC_IOUT_CAL != 0) {
        acc = acc * REC_IOUT_CALU;
        acc = acc / 1000;
    }
    *s_cell1_mv = (int16_t)acc;

    /* cell 2 voltage -> 0x20000202 */
    acc = (int64_t)RAW[1] * 0x325;
    acc = acc * 0x1A1;
    acc = acc / 0x1B;
    acc = acc / 1000;
    if (REC_VOUT_CAL != 0) {
        acc = acc * REC_VOUT_CALU;
        acc = acc / 1000;
    }
    *s_cell2_mv = (int16_t)acc;

    /* pack current -> descending SOC-index lookup */
    int64_t base = (int64_t)RAW[2] * 0x325;
    uint32_t cur = (uint32_t)((base * 10000) / (0x325AA0 - base));
    *soc = 0x91;
    for (uint8_t i = 0x91; i != 0; i--) {
        if (cur <= soc_lut[i]) {
            *soc = i;
            break;
        }
    }

    /* signed SOC correction + history high-water debounce */
    if (*s_evtlog_en > 4) {
        if (*(volatile int8_t *)s_soc_corr != 0) {
            if (*(volatile int8_t *)s_soc_corr < 0) {
                *soc = (uint8_t)(*soc - (uint8_t)(~(uint32_t)*corr + 1));
            } else {
                *soc = (uint8_t)(*(volatile int8_t *)s_soc_corr + *soc);
            }
        }
        if (REC_SOC_HIST < *soc) {
            uint8_t d = *s_soc_debounce;
            *s_soc_debounce = d + 1;
            if ((uint8_t)(d + 1) > 5) {
                *s_soc_debounce = 0;
                REC_SOC_HIST = *soc;
            }
        } else {
            *s_soc_debounce = 0;
        }
    }
    gpio_bit_write(GPIOB_BASE, AFE_CS_PIN, 0);         /* AFE CS low */
}
