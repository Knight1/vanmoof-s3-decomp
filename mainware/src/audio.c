#include <stdint.h>

#include "audio.h"
#include "stm32f413_gpio.h"

/* Amp control lines on GPIOD. */
#define AMP_MUTE_PIN      (1u << 5)    /* PD5  — strobed low while probing */
#define AMP_BROWNOUT_PIN  (1u << 13)   /* PD13 — asserted on low supply */
#define SUPPLY_BROWNOUT   25000u       /* 0x61A7, in the supply reading's units */

/* Opaque helpers (kept as OEM externs until decoded): supply read, table probe,
 * and the per-device bus TX engine. */
extern void     HAL_GPIO_WritePin(void *GPIOx, uint16_t pin, int state);
extern uint32_t supply_voltage_read(void);                                  /* supply/voltage read */
extern int      hw_version_lookup(uint8_t *out_index);                    /* probe 16-entry table */
extern uint32_t FUN_08024760(void *dev, uint8_t cmd, uint8_t *data,
                             uint16_t len, uint32_t timeout);         /* bus TX engine */
extern void    *g_amp_dev_ctx;   /* SRAM 0x20009B04 — amp device/transport handle */

uint32_t amp_volume_brownout_apply(uint8_t *level)
{
    uint32_t supply = supply_voltage_read();

    if (*level != 0) {
        HAL_GPIO_WritePin((void *)GPIOD_BASE, AMP_MUTE_PIN, 0);
        uint8_t probe_index;
        hw_version_lookup(&probe_index);
        if (probe_index > 5) {
            *level = (uint8_t)(*level + 1);
        }
    }

    if (supply < SUPPLY_BROWNOUT) {
        HAL_GPIO_WritePin((void *)GPIOD_BASE, AMP_BROWNOUT_PIN, 1);
        if (*level > 0x14) {
            *level = 0x14;
        }
    } else {
        HAL_GPIO_WritePin((void *)GPIOD_BASE, AMP_BROWNOUT_PIN, 0);
    }

    /* transmit the (possibly clamped) 1-byte level to the amp, cmd 0x96. */
    return FUN_08024760(g_amp_dev_ctx, 0x96, level, 1, 0x32);
}
