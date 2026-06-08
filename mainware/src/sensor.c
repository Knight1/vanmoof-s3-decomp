#include <stdint.h>

#include "sensor.h"

/* ADC/sensor context (OEM SRAM 0x20000914). Shared layout: a moving-average
 * ring at the base (write-cursor byte at +0x00, ten u16 samples at +0x04), a
 * status/flag byte at +0x22, and the raw ADC sample (u16) at +0x2A. */
#define ADC_CTX  0x20000914u

/* 10-sample circular moving-average filter (OEM moving_avg10_push, 0x08032AB0).
 * Pushes a sample into the ring, advances/wraps the cursor at 10, and returns
 * the integer mean. Division by 10 uses the 0xCCCCCCCD reciprocal-multiply
 * idiom — exact here since the sum never exceeds 10 * 0xFFFF. */
uint16_t moving_avg10_push(uint16_t sample)
{
    volatile uint8_t  *cursor  = (volatile uint8_t  *)(ADC_CTX + 0x00);
    volatile uint16_t *samples = (volatile uint16_t *)(ADC_CTX + 0x04);

    uint8_t i = *cursor;
    samples[i] = sample;
    i = (uint8_t)(i + 1);
    *cursor = i;
    if (i == 10) {                 /* wrap after the post-increment */
        *cursor = 0;
    }

    uint32_t sum = 0;
    for (int j = 0; j < 10; j++) {
        sum += samples[j];
    }
    return (uint16_t)((uint32_t)(((uint64_t)0xCCCCCCCDULL * sum) >> 32) >> 3);
}

uint16_t supply_voltage_read(void)
{
    uint16_t raw = *(volatile uint16_t *)(ADC_CTX + 0x2A);

    /* stage 1: raw * 0xCE4 >> 12 (fixed-point gain ~0.8017) */
    uint32_t s1 = ((uint32_t)raw * 0xCE4u) >> 12;

    /* the OEM clears the status byte between the two multiply stages */
    *(volatile uint8_t *)(ADC_CTX + 0x22) = 0;

    /* stage 2: * 0x866 */
    uint32_t s2 = s1 * 0x866u;

    /* stage 3: fixed-point divide via the 0x1B4E81B5 reciprocal:
     * (hi32(0x1B4E81B5 * s2) >> 4) & 0xFFFF */
    uint64_t prod = (uint64_t)0x1B4E81B5u * (uint64_t)s2;
    uint16_t scaled = (uint16_t)(((uint32_t)(prod >> 32) >> 4) & 0xFFFFu);

    return moving_avg10_push(scaled);   /* smoothed reading */
}

/* Charger-level byte for the BLE 0x5543 read (OEM charge_level_adc_get,
 * 0x08037160). The raw 12-bit charger ADC sample lives at +0x10 of the lipo/charge
 * context (0x20005DB4, the object lipo_charge_state_monitor maintains). 0xFFF is
 * the 12-bit full-scale "no reading / sensor invalid" sentinel — reported as the
 * 0xFF byte; otherwise the sample is scaled by /10 (OEM signed magic-multiply)
 * and truncated to a byte. Callers consume only the low byte. */
#define CHARGE_CTX_ADC_SAMPLE  (*(volatile int32_t *)(0x20005DB4u + 0x10u))

uint8_t charge_level_adc_get(void)
{
    int32_t sample = CHARGE_CTX_ADC_SAMPLE;

    if (sample == 0xFFF) {
        return 0xFF;
    }
    return (uint8_t)(sample / 10);
}

/* Identify the board HW revision 0..15 from the HW-ID resistor-divider voltage
 * sensed on an ADC channel (OEM hw_version_lookup, 0x08032ce4). The latest
 * 10-sample moving average (raw 12-bit code at ADC_CTX+0x24) is converted to mV
 * as (code*3300)>>12, then matched against a 16-entry table of divider pairs at
 * flash rodata 0x08044f9c: each entry's band centre is r_high*3300/(r_high+r_low)
 * mV and a +/-80 mV window selects it. The matched index is cached at ADC_CTX+0x22
 * and written to *out; returns 1 on a match (index < 16), else 0 (out = 16). */
#define HW_ID_TABLE   ((const float (*)[2])0x08044f9cu)  /* 16 x {r_low, r_high} */
#define HW_VERSION_BYTE  (*(volatile uint8_t  *)(ADC_CTX + 0x22))
#define HW_ADC_AVG       (*(volatile uint16_t *)(ADC_CTX + 0x24))

int hw_version_lookup(uint8_t *out)
{
    float measured_mv = (float)(uint32_t)(((uint32_t)HW_ADC_AVG * 3300u) >> 12);
    uint8_t i;

    HW_VERSION_BYTE = 0;

    for (i = 0; i < 16u; i = (uint8_t)(i + 1)) {
        float r_low  = HW_ID_TABLE[i][0];
        float r_high = HW_ID_TABLE[i][1];
        float band   = (r_high * 3300.0f) / (r_high + r_low);

        if (measured_mv < band - 80.0f) {
            continue;
        }
        if (measured_mv > band + 80.0f) {
            continue;
        }
        break;
    }

    *out = i;
    return i < 16u;
}

/* STC3115 LiPo fuel-gauge register I/O over I2C (sourced elsewhere). read returns
 * the byte value or 0xFFFFFFFF on bus error; write takes (reg, value). */
extern uint32_t stc3115_read_reg(uint32_t reg);             /* OEM 0x080393DC */
extern void     stc3115_write_reg(uint32_t reg, uint8_t value); /* OEM 0x080394A6 */

/* Put the STC3115 into operating ("run") mode: set bit 0 of the MODE register
 * (reg 0) (OEM stc_gas_gauge_set_run, 0x080398B8). The OEM does not check the
 * read result — a -1 bus error becomes 0xFF and still has bit 0 set; the value is
 * truncated to a byte before the write (uxtb). */
void stc_gas_gauge_set_run(void)
{
    uint8_t mode = (uint8_t)stc3115_read_reg(0);
    stc3115_write_reg(0, (uint8_t)(mode | 1u));
}
