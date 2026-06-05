#include <stdint.h>

#include "sensor.h"

/* ADC/sensor context (OEM SRAM 0x20000914): raw ADC sample (u16) at +0x2A, a
 * status/flag byte at +0x22. */
#define ADC_CTX  0x20000914u

/* 10-sample circular moving-average push; returns the smoothed value. */
extern uint16_t FUN_08032ab0(uint16_t sample);

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

    return FUN_08032ab0(scaled);   /* smoothed reading */
}
