#include <stdint.h>

#include "sensor.h"
#include "scheduler.h"   /* scheduler_alloc/start/slot_is_idle, SCHED_SLOT_NONE */
#include "systick.h"     /* systick_now */

extern void nvic_clear_pending_irq();   /* 0x0802714C */
extern void nvic_set_priority();        /* 0x08027078 */
extern void nvic_enable_irq();          /* 0x080270E0 */

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

/* ── HDC1080 temperature/humidity (I2C dev 0x80) + a sensor de-glitch filter ── */

extern int HAL_I2C_Master_Transmit(void *h, uint16_t addr, const uint8_t *d,
                                   uint16_t n, uint32_t tmo);
extern int HAL_I2C_Master_Receive(void *h, uint16_t addr, uint8_t *d,
                                  uint16_t n, uint32_t tmo);
extern int HAL_I2C_Mem_Write(void *h, uint16_t dev, uint16_t mem, uint16_t memsz,
                             const uint8_t *d, uint16_t n, uint32_t tmo);

/* hdc1080_write_config_reg (OEM 0x08033118) — write the HDC1080 configuration
 * register (0x02): base 0x14 (mode_a==1) or 0x10, OR'd with 1/2 per mode_b; low
 * byte 0. Returns the I2C write status. */
int hdc1080_write_config_reg(void *hi2c, int mode_a, int mode_b)
{
    uint8_t cfg[8];

    cfg[0] = (mode_a == 1) ? 0x14 : 0x10;
    if (mode_b == 1) {
        cfg[0] |= 1;
    } else if (mode_b == 2) {
        cfg[0] |= 2;
    }
    cfg[1] = 0;
    return HAL_I2C_Mem_Write(hi2c, 0x80, 2, 1, cfg, 2, 1000);
}

/* Set the HDC1080 read pointer to register 0 (temperature) — also triggers a
 * conversion. OEM 0x08033164. */
int hdc1080_set_pointer(void *hi2c)
{
    uint8_t reg[5];

    reg[0] = 0;
    return HAL_I2C_Master_Transmit(hi2c, 0x80, reg, 1, 0x32);
}

/* Read the HDC1080's 4 result bytes (temp MSB/LSB, humidity MSB/LSB) and convert
 * to temperature in 0.1 degC and relative humidity in %:
 *   T  = (raw / 2^16) * 165 - 40, scaled x10
 *   RH = (raw / 2^16) * 100
 * (OEM 0x08033188 — soft-float, constants 2^-16 / 165 / 40 / 10 / 100 from
 * flash). Returns the HAL status (0 = OK). */
int hdc1080_read(void *hi2c, int16_t *temp_dC, uint16_t *rh_pct)
{
    uint8_t b[4];
    int rc = HAL_I2C_Master_Receive(hi2c, 0x80, b, 4, 0x32);

    if (rc == 0) {
        uint16_t t_raw = (uint16_t)((b[0] << 8) | b[1]);
        uint16_t h_raw = (uint16_t)((b[2] << 8) | b[3]);
        *temp_dC = (int16_t)(int)(((double)t_raw * (1.0 / 65536.0) * 165.0 - 40.0) * 10.0);
        *rh_pct  = (uint16_t)(unsigned)((double)h_raw * (1.0 / 65536.0) * 100.0);
    }
    return rc;
}

/* 6-sample min/max-reject mean filter (OEM 0x08038ED4): push `sample` into a
 * 6-deep ring at 0x20006E48 (count byte + six u16) and return the mean of the
 * middle four — the running sum minus the highest and lowest, divided by 4.
 * Used by the EXTI9_5 / TIM7 application hooks to de-glitch a sampled input. */
uint32_t sensor_filter6_push(uint16_t sample)
{
    volatile uint8_t  *ring    = (volatile uint8_t *)0x20006e48u;
    volatile uint16_t *samples = (volatile uint16_t *)(0x20006e48u + 4u);
    uint8_t count = ring[0];
    uint32_t sum = 0, max = 0, min = 0xffff;
    int i;

    samples[count] = sample;
    count = (uint8_t)(count + 1);
    ring[0] = count;
    if (count == 6) {
        ring[0] = 0;
    }
    for (i = 0; i < 6; i++) {
        uint32_t v = samples[i];
        if (v < min) {
            min = v;
        }
        if (max < v) {
            max = v;
        }
        sum = (sum + v) & 0xffff;
    }
    return ((sum - max - min) & 0x3ffff) >> 2;
}

/* ADC sampling-config shadow copy (OEM 0x08032CBC), called from the ADC
 * config-apply path. While the ADC status byte (ADC_CTX+0x22) is clear it
 * latches the live sampling config — the two words at +0x18/+0x1c and the half
 * at +0x20 — into the shadow fields at +0x24/+0x28/+0x2c. The OEM loads +0x20 as
 * a word and stores only its low half. */
void adc_config_shadow_copy(void)
{
    if (*(volatile uint8_t *)(ADC_CTX + 0x22) != 0) {
        return;
    }
    uint32_t cfg_lo = *(volatile uint32_t *)(ADC_CTX + 0x18);
    uint32_t cfg_hi = *(volatile uint32_t *)(ADC_CTX + 0x1c);
    uint16_t cfg_x  = (uint16_t)*(volatile uint32_t *)(ADC_CTX + 0x20);

    *(volatile uint32_t *)(ADC_CTX + 0x24) = cfg_lo;
    *(volatile uint32_t *)(ADC_CTX + 0x28) = cfg_hi;
    *(volatile uint16_t *)(ADC_CTX + 0x2c) = cfg_x;
}

/* supply_voltage_sample_step (OEM 0x08029B24) — sample the main supply voltage
 * (moving-averaged mV) at most every 100 ticks into the cache at G_CLK+0x0A, using
 * the timer slot at G_STATE[3]; returns the cached value. Called each super-loop. */
uint16_t supply_voltage_sample_step(void)
{
    uint8_t  *slot = (uint8_t  *)0x2000002cu;   /* G_STATE[3] timer slot */
    uint16_t *samp = (uint16_t *)0x200001e2u;   /* G_CLK+0x0A cached mV */

    if (*slot == SCHED_SLOT_NONE) {
        *slot = scheduler_alloc();
        scheduler_start(*slot, 100, 0);
    }
    if (scheduler_slot_is_idle(*slot) != 0) {
        *samp = supply_voltage_read();
        scheduler_start(*slot, 100, 0);
    }
    return *samp;
}

/* output_value_filter_step (OEM 0x08038F78) — sample-and-hold filter over a 32-bit
 * value: below 25000 the accumulators (state block 0x20006E48, +0x18/+0x1A/+0x1C)
 * are reset; every 100 ticks the held value (+0x24) is refreshed from the
 * accumulator (+0x18). A one-shot init counter at +0x2A (re)enables IRQ 23 when it
 * counts down to 0. Returns the currently-held value. Called each super-loop. */
uint32_t output_value_filter_step(uint32_t value)
{
    uint8_t *f = (uint8_t *)0x20006e48u;

    if (value < 25000) {
        *(uint16_t *)(f + 0x1a) = 0;
        *(uint16_t *)(f + 0x18) = 0;
        *(uint8_t  *)(f + 0x1c) = 0;
    }
    if (*(uint32_t *)(f + 0x20) + 100 < systick_now()) {
        *(uint32_t *)(f + 0x20) = systick_now();
        *(uint32_t *)(f + 0x24) = *(uint32_t *)(f + 0x18);
        *(uint8_t  *)(f + 0x28) = 0;
    }
    {
        int16_t cd = *(int16_t *)(f + 0x2a);
        if (cd != 0) {
            *(int16_t *)(f + 0x2a) = (int16_t)(cd - 1);
            if (cd == 1) {
                nvic_clear_pending_irq(0x17);
                nvic_set_priority(0x17, 0, 0);
                nvic_enable_irq(0x17);
            }
        }
    }
    return *(uint32_t *)(f + 0x24);
}
