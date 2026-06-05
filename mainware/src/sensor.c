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
