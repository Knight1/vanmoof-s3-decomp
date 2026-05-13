/* sensor.c — Hall position + supply voltage reads. */

#include "sensor.h"
#include "adc.h"
#include "gpio.h"
#include "mm32f031.h"

/* Supply divider on the PCB: 22k / 4k7 → factor ~5.68. */
#define VSENSE_VREF_MV          3300u
#define VSENSE_DIVIDER_NUM      268u   /* (22+4.7)/4.7 * 100 → 568, but use ADC ratio */
#define VSENSE_DIVIDER_DEN      47u

/* Empirical thresholds for the analog Hall outputs at each detent. */
#define HALL_GEAR1_LOW          1600u
#define HALL_GEAR1_HIGH         2200u
#define HALL_GEAR2_LOW          2400u
#define HALL_GEAR2_HIGH         3000u

void sensor_init(void)
{
    gpio_port_clock_enable(GPIOA);
    gpio_pin_mode(GPIOA, 0, GPIO_MODE_ANALOG);
    gpio_pin_mode(GPIOA, 1, GPIO_MODE_ANALOG);
    gpio_pin_mode(GPIOA, 2, GPIO_MODE_ANALOG);
    adc_init();
}

uint16_t sensor_hall_a_raw(void)  { return adc_read_channel(ADC_CH_HALL_A); }
uint16_t sensor_hall_b_raw(void)  { return adc_read_channel(ADC_CH_HALL_B); }
uint16_t sensor_vsense_raw(void)  { return adc_read_channel(ADC_CH_VSENSE); }

uint16_t sensor_supply_mv(void)
{
    const uint32_t raw = sensor_vsense_raw();
    /* mV = raw / 4095 * VREF * divider_ratio */
    const uint32_t mv = (raw * (uint32_t)VSENSE_VREF_MV) / 4095u;
    return (uint16_t)((mv * VSENSE_DIVIDER_NUM) / VSENSE_DIVIDER_DEN);
}

uint8_t sensor_position_gear(void)
{
    const uint16_t a = sensor_hall_a_raw();
    if (a >= HALL_GEAR1_LOW && a <= HALL_GEAR1_HIGH) return 1u;
    if (a >= HALL_GEAR2_LOW && a <= HALL_GEAR2_HIGH) return 2u;
    return 0u;
}
