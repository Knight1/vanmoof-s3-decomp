#include "batteryware.h"

/*
 * Clear fuel gauge status registers.
 */
void fg_clear_status(void)
{
    *(volatile uint32_t *)0x20002C88 = 0;
    *(volatile uint32_t *)0x20002C8C = 0;
}

/* Fuel gauge status byte in SRAM */
static volatile uint8_t * const s_fg_status = (volatile uint8_t *)0x20002870;

/* Cell voltage readings (in SRAM) */
static volatile uint8_t * const s_cell_voltages = (volatile uint8_t *)0x20002588;

/* Protection thresholds and debounce config */
static volatile uint8_t * const s_protection_cfg = (volatile uint8_t *)0x200028D0;

/* Protection fault flags — central status register */
volatile uint32_t * const g_fault_flags = (volatile uint32_t *)0x20002C44;

/* Protection debounce counters */
static volatile uint16_t * const s_uvp1_counter = (volatile uint16_t *)0x20002A80;
static volatile uint16_t * const s_uvp2_counter = (volatile uint16_t *)0x20002A8C;
static volatile uint16_t * const s_ovp1_counter = (volatile uint16_t *)0x20002A94;
static volatile uint16_t * const s_ovp2_counter = (volatile uint16_t *)0x20002A52;

/* Tick counter in SRAM */
static volatile uint32_t * const s_tick_counter = (volatile uint32_t *)0x200000C8;

/* RCC base for clock tree queries */
static volatile uint32_t * const RCC_FG = (volatile uint32_t *)0x40021000;

/* Shift table in flash for fg_read_field_8 / fg_read_field_11 */
static const uint8_t * const s_fg_shift_table = (const uint8_t *)0x080181F8;

/* Fault flag bits */
#define FAULT_UVP1  0x01
#define FAULT_UVP2  0x02
#define FAULT_OVP1  0x04
#define FAULT_OVP2  0x08

#define DEBOUNCE_DIVISOR 100

/*
 * Read fuel gauge status flag (bit 0 of the cached status register).
 */
bool fg_status_flag_get(void)
{
    return (*s_fg_status & 1) == 1;
}

/*
 * Read fuel gauge status flag (bit 1 of the cached status register).
 */
bool fg_status_flag2_get(void)
{
    return (*s_fg_status & 2) == 2;
}

/*
 * Under-voltage protection 1 (UVP1) check.
 *
 * Compares cell voltages 1 and 2 against the UVP1 threshold at
 * s_protection_cfg[0x6E]. If both cells are below the threshold,
 * the counter is reset. Otherwise the counter increments each
 * call. When the counter exceeds (debounce_value / 100), FAULT_UVP1
 * is set in g_fault_flags.
 */
void fg_uvp1_check(void)
{
    uint8_t threshold = s_protection_cfg[0x6E];

    if (s_cell_voltages[1] < threshold && s_cell_voltages[2] < threshold) {
        *s_uvp1_counter = 0;
    } else {
        uint16_t count = *s_uvp1_counter + 1;
        *s_uvp1_counter = count;
        uint16_t debounce = *(volatile uint16_t *)(s_protection_cfg + 0x70) / DEBOUNCE_DIVISOR;
        if (debounce <= count) {
            *g_fault_flags |= FAULT_UVP1;
        }
    }
}

/*
 * Under-voltage protection 2 (UVP2) check.
 *
 * Compares threshold at s_protection_cfg[0x76] against cell voltages.
 */
void fg_uvp2_check(void)
{
    uint8_t threshold = s_protection_cfg[0x76];

    if (threshold < s_cell_voltages[1] && threshold < s_cell_voltages[2]) {
        *s_uvp2_counter = 0;
    } else {
        uint16_t count = *s_uvp2_counter + 1;
        *s_uvp2_counter = count;
        uint16_t debounce = *(volatile uint16_t *)(s_protection_cfg + 0x78) / DEBOUNCE_DIVISOR;
        if (debounce <= count) {
            *g_fault_flags |= FAULT_UVP2;
        }
    }
}

/*
 * Over-voltage protection 1 (OVP1) check.
 *
 * Compares cell voltages against the OVP1 threshold at s_protection_cfg[0x7E].
 */
void fg_ovp1_check(void)
{
    uint8_t threshold = s_protection_cfg[0x7E];

    if (s_cell_voltages[1] < threshold && s_cell_voltages[2] < threshold) {
        *s_ovp1_counter = 0;
    } else {
        uint16_t count = *s_ovp1_counter + 1;
        *s_ovp1_counter = count;
        uint16_t debounce = *(volatile uint16_t *)(s_protection_cfg + 0x80) / DEBOUNCE_DIVISOR;
        if (debounce <= count) {
            *g_fault_flags |= FAULT_OVP1;
        }
    }
}

/*
 * Over-voltage protection 2 (OVP2) check.
 *
 * Compares threshold at s_protection_cfg[0x86] against cell voltages.
 */
void fg_ovp2_check(void)
{
    uint8_t threshold = s_protection_cfg[0x86];

    if (threshold < s_cell_voltages[1] && threshold < s_cell_voltages[2]) {
        *s_ovp2_counter = 0;
    } else {
        uint16_t count = *s_ovp2_counter + 1;
        *s_ovp2_counter = count;
        uint16_t debounce = *(volatile uint16_t *)(s_protection_cfg + 0x88) / DEBOUNCE_DIVISOR;
        if (debounce <= count) {
            *g_fault_flags |= FAULT_OVP2;
        }
    }
}

/* Over-current protection counters */
static volatile uint16_t * const s_discharge_oc_counter = (volatile uint16_t *)0x20002800;
static volatile uint16_t * const s_charge_oc_counter    = (volatile uint16_t *)0x200028A0;

/* Fuel gauge status register */
static volatile uint8_t * const s_fg_status_reg = (volatile uint8_t *)0x20002870;

#define FAULT_DISCHARGE_OC  0x40
#define FAULT_CHARGE_OC     0x80

/*
 * Discharge over-current check.
 *
 * Monitors a threshold from s_protection_cfg[0x98]. If the discharge
 * current stays above threshold for the debounce period, sets
 * FAULT_DISCHARGE_OC in g_fault_flags.
 */
void fg_discharge_oc_check(void)
{
    if ((*s_fg_status_reg & 2) == 2) {
        *s_discharge_oc_counter = 0;
    } else if (*s_discharge_oc_counter < *(volatile uint16_t *)(s_protection_cfg + 0x98)) {
        *s_discharge_oc_counter = 0;
    } else {
        uint16_t count = *s_discharge_oc_counter + 1;
        *s_discharge_oc_counter = count;
        uint16_t debounce = *(volatile uint16_t *)(s_protection_cfg + 0x96) / DEBOUNCE_DIVISOR;
        if (debounce <= count) {
            *g_fault_flags |= FAULT_DISCHARGE_OC;
        }
    }
}

/*
 * Charge over-current check.
 *
 * Monitors a threshold from s_protection_cfg[0x9A]. If the charge
 * current stays above threshold for the debounce period, sets
 * FAULT_CHARGE_OC in g_fault_flags.
 */
void fg_charge_oc_check(void)
{
    if ((*s_fg_status_reg & 1) == 1) {
        *s_charge_oc_counter = 0;
    } else if (*s_charge_oc_counter < *(volatile uint16_t *)(s_protection_cfg + 0x9A)) {
        *s_charge_oc_counter = 0;
    } else {
        uint16_t count = *s_charge_oc_counter + 1;
        *s_charge_oc_counter = count;
        uint16_t debounce = *(volatile uint16_t *)(s_protection_cfg + 0x96) / DEBOUNCE_DIVISOR;
        if (debounce <= count) {
            *g_fault_flags |= FAULT_CHARGE_OC;
        }
    }
}

/* Temperature protection counter and fault bit */
static volatile uint16_t * const s_ts_counter = (volatile uint16_t *)0x20002ABA;
#define FAULT_TS   0x10

/*
 * Temperature sensor threshold check.
 *
 * Compares cell voltage at s_cell_voltages[0] (used as analog temp reading)
 * against the threshold at s_protection_cfg[0x8E]. If above threshold,
 * increments the debounce counter. When exceeded, sets FAULT_TS in
 * g_fault_flags.
 */
void fg_threshold_check(void)
{
    uint8_t threshold = s_protection_cfg[0x8E];

    if (s_cell_voltages[0] < threshold) {
        *s_ts_counter = 0;
    } else {
        uint16_t count = *s_ts_counter + 1;
        *s_ts_counter = count;
        uint16_t debounce = *(volatile uint16_t *)(s_protection_cfg + 0x90) / DEBOUNCE_DIVISOR;
        if (debounce <= count) {
            *g_fault_flags |= FAULT_TS;
        }
    }
}

/* Shipping mode state variables */
static volatile uint8_t  * const s_shipping_enable = (volatile uint8_t *)0x20002BFC;
static volatile uint32_t * const s_shipping_timer  = (volatile uint32_t *)0x20002AC8;
static volatile uint8_t  * const s_shipping_state  = (volatile uint8_t *)0x20002B58;

/*
 * Shipping mode entry check.
 *
 * If shipping mode is enabled (bit 1 of s_shipping_enable) and the
 * timer has exceeded 0x1D (29 seconds), enters shipping mode depending
 * on the current state:
 *   - state 0x11 → forward to shipping mode handler
 *   - state 0x10/0x0F → set config flag then enter
 * Otherwise clears the enable flag (timeout with no entry).
 *
 * The shipping mode handler is called via a veneer at FUN_080155cc.
 */
void shipping_mode_check(void)
{
    if ((*s_shipping_enable & 2) == 0) {
        return;
    }

    if (*s_shipping_timer > 0x1D) {
        if (*s_shipping_state == 0x11) {
            /* shipping mode entry veneer */
            extern void shipping_mode_enter(void);
            shipping_mode_enter();
            return;
        }
        if (*s_shipping_state == 0x10 || *s_shipping_state == 0x0F) {
            s_protection_cfg[5] = 1;
            extern void shipping_mode_enter(void);
            shipping_mode_enter();
            return;
        }
    }

    *s_shipping_enable &= ~2U;
}

/* RSOC lookup table (voltage → state-of-charge) at flash 0x080174B0 */
static volatile uint16_t * const s_rsoc_table     = (volatile uint16_t *)0x080174B0;
static volatile uint8_t  * const s_rsoc_percent   = (volatile uint8_t *)0x200025BD;
static volatile uint16_t * const s_rsoc_voltage   = (volatile uint16_t *)0x2000282A;
static volatile uint32_t * const s_rsoc_register  = (volatile uint32_t *)0x200025AC;

/* Capacity field in SRAM (for capacity_decrement) */
static volatile uint32_t * const s_capacity       = (volatile uint32_t *)0x200029A8;

/*
 * Relative State-of-Charge setter.
 *
 * If percent < 101 (valid range): stores it at s_capacity[0x36],
 * zeroes the RSOC register, then adds the full capacity
 * (s_capacity[0x28]) and multiplies by percent * 0x90.
 * Same formula as rsoc_lookup but driven by an external percent value.
 */
void rsoc_set(uint8_t percent)
{
    if (percent < 0x65) {
        *(volatile uint8_t *)(s_capacity + 0x36 / 4) = percent;
        s_capacity[0x24 / 4] = 0;
        s_capacity[0x24 / 4] += s_capacity[0x28 / 4];
        s_capacity[0x24 / 4] *= (uint32_t)percent;
        s_capacity[0x24 / 4] *= 0x90;
    }
}

/*
 * Saturating capacity decrement.
 *
 * Decrements the capacity counter at 0x200029A8+0x24 by 'amount'.
 * If the remaining capacity is less than 'amount', zeroes it instead
 * (saturating subtract — prevents underflow).
 */
void capacity_decrement(uint32_t amount)
{
    if (s_capacity[0x24 / 4] < amount) {
        s_capacity[0x24 / 4] = 0;
    } else {
        s_capacity[0x24 / 4] -= amount;
    }
}

/*
 * State-of-charge lookup.
 *
 * Searches the 100-entry voltage→SoC table (at flash 0x080174B0)
 * descending from 100% down to 0%, finding the first entry where
 * table[percent] <= current_voltage. Then computes:
 *   RSOC = (base_capacity + 0) * percent * 0x90
 * where base_capacity is read from s_protection_cfg[0x28].
 */
void rsoc_lookup(void)
{
    *s_rsoc_percent = 0;
    uint8_t percent = 100;

    while (percent != 0) {
        if (s_rsoc_table[percent] <= *s_rsoc_voltage) {
            *s_rsoc_percent = percent;
            *s_rsoc_register = 0;
            *s_rsoc_register = *(volatile uint32_t *)(s_protection_cfg + 0x28);
            *s_rsoc_register = *s_rsoc_register * (uint32_t)*s_rsoc_percent;
            *s_rsoc_register = *s_rsoc_register * 0x90;
            return;
        }
        percent--;
    }
    /* Fallthrough: percent already 0, result stays at 0 */
    *s_rsoc_register = 0;
    *s_rsoc_register = *(volatile uint32_t *)(s_protection_cfg + 0x28);
    *s_rsoc_register = *s_rsoc_register * (uint32_t)*s_rsoc_percent;
    *s_rsoc_register = *s_rsoc_register * 0x90;
}

/* Alert monitor — GPIOB pin 6 */
#define FAULT_ALERT  0x20
static volatile uint8_t  * const s_alert_counter = (volatile uint8_t *)0x20002ABF;
static volatile uint8_t  * const s_alert_cfg     = (volatile uint8_t *)0x200029A8;

/*
 * Fuel gauge alert pin monitor.
 *
 * Reads GPIOB pin 6 (0x40). If high (inactive), clears the counter.
 * If low (active alert), increments the debounce counter. After 10
 * consecutive active reads, sets FAULT_ALERT in g_fault_flags and
 * sets the BMS status field at s_alert_cfg[0x38] to 3.
 */
void fg_alert_monitor(void)
{
    if (gpio_bit_read(0x50000400, 0x40)) {
        *s_alert_counter = 0;
    } else {
        uint8_t count = *s_alert_counter + 1;
        *s_alert_counter = count;
        if (count > 9) {
            *s_alert_counter = 0;
            s_alert_cfg[0x38] = 3;
            *g_fault_flags |= FAULT_ALERT;
        }
    }
}

/* Charge/discharge state byte */
static volatile uint8_t  * const s_charge_state     = (volatile uint8_t *)0x20002B58;
static volatile uint16_t * const s_discharge_counter = (volatile uint16_t *)0x20002800;
static volatile uint16_t * const s_charge_current    = (volatile uint16_t *)0x200028C8;
#define CHARGE_CURRENT_LOW_THRESHOLD  19999

/* Charge/discharge status flags */
#define CHG_STATUS_DISCHARGING  0
#define CHG_STATUS_CHARGING     1
#define CHG_STATUS_IDLE         2
#define CHG_STATUS_CHARGE_LOW   4

/*
 * Determine charge/discharge status.
 *
 * Uses the current BMS state to determine the mode:
 *   - state > 6   → IDLE (2)
 *   - state == 2  → CHARGING (1)
 *   - state == 3  → config[0x16] <= s_discharge_counter ? 1 : 0
 *   - otherwise   → DISCHARGING (0)
 * ORs bit 4 (CHG_STATUS_CHARGE_LOW) if charge current <= 19999.
 */
uint8_t fg_charge_status(void)
{
    uint8_t status = *s_charge_state;

    if (status < 0x1A) {
        if (status > 6) {
            status = CHG_STATUS_IDLE;
        } else if (status == 2) {
            status = CHG_STATUS_CHARGING;
        } else if (status == 3) {
            uint16_t thresh = *(volatile uint16_t *)(s_protection_cfg + 0x16);
            status = (thresh <= *s_discharge_counter) ? 1 : 0;
        } else {
            status = CHG_STATUS_DISCHARGING;
        }
    } else {
        status = CHG_STATUS_DISCHARGING;
    }

    if (*s_charge_current <= CHARGE_CURRENT_LOW_THRESHOLD) {
        status |= CHG_STATUS_CHARGE_LOW;
    }

    return status;
}

/* Config resend counter */
static volatile uint32_t * const s_config_counter = (volatile uint32_t *)0x200025A0;

/*
 * Re-send all configuration blocks to the fuel gauge.
 *
 * Zeroes the config counter, then calls memcmp_verify for three
 * configuration blocks (4+4+1 bytes) to restore FEDL5236 settings.
 */
void config_resend_all(void)
{
    *s_config_counter = 0;
    memcmp_verify((char *)0x200029CC,   4, (char *)0x08080C24);
    memcmp_verify((char *)0x200029D4,   4, (char *)0x08080C2C);
    memcmp_verify((char *)0x200029DE,   1, (char *)0x08080C36);
}

/* DMA compare helper */
static volatile uint32_t * const s_dma_compare_reg = (volatile uint32_t *)0x20002000;
static volatile uint32_t * const s_dma_compare_flags = (volatile uint32_t *)0x20002000;
#define DMA_COMPARE_BLOCK_SIZE  0x40

/*
 * DMA transfer compare.
 *
 * Compares two memory regions block-by-block (0x40 bytes each).
 * Returns 1 if any block fails comparison, 0 if all match.
 */
uint32_t dma_compare(uint32_t addr_a, uint16_t count, uint32_t addr_b)
{
    extern int dma_memcmp(uint32_t a, uint32_t b);  /* veneer_11ea8 */
    *s_dma_compare_reg = 0;
    *s_dma_compare_flags = 0;

    int16_t remaining = (int16_t)count;
    uint32_t a = addr_a;
    uint32_t b = addr_b;

    while (remaining != 0) {
        if (dma_memcmp(a, b) != 0) {
            return 1;
        }
        remaining -= DMA_COMPARE_BLOCK_SIZE;
        a += DMA_COMPARE_BLOCK_SIZE;
        b += DMA_COMPARE_BLOCK_SIZE;
    }

    return 0;
}

/* FEDL5236 SPI comms data */
static volatile uint32_t * const s_fg_i2c_ctx  = (volatile uint32_t *)0x20003250;
static volatile uint8_t  * const s_fg_i2c_data = (volatile uint8_t  *)0x20003254;
static volatile uint8_t  * const s_fg_watchdog  = (volatile uint8_t  *)0x20003258;

/* SPI status checker — external */
extern uint32_t i2c_check_ready(void *ctx);  /* FUN_08010f88 */
/* SPI read 2 bytes — external */
extern uint32_t i2c_read_2bytes(uint8_t addr, uint8_t count);  /* FUN_080048cc */

/*
 * Fuel gauge communication watchdog.
 *
 * Checks the SPI bus and reads two status bytes from the FEDL5236.
 * If reading fails or the chip reports an error (both bits set),
 * resets the config. Otherwise re-initializes the BMS with a delay.
 */
void fg_watchdog_kick(void)
{
    if (i2c_check_ready((void *)s_fg_i2c_ctx) == 1) {
        if (i2c_read_2bytes(1, 2) == 0 ||
            (((s_fg_i2c_data[2] & 0xF) == 0xF &&
              (s_fg_i2c_data[3] & 2) == 2))) {
            *s_fg_watchdog = 0;
            /* Reset SPI config */
            extern void i2c_write_reg(uint8_t addr, uint8_t val, uint8_t mask);
            i2c_write_reg(8, 0x91, 0xFF);  /* FUN_08004a18 */
        } else {
            delay_ms(5);
            bms_init();
            delay_ms(5);
        }
    }
}

/* Cell voltage table and balance threshold */
static volatile uint16_t * const s_cell_voltage_table = (volatile uint16_t *)0x08013E50;
static volatile uint16_t * const s_balance_threshold  = (volatile uint16_t *)0x20002000;
static volatile uint8_t  * const s_balance_idx        = (volatile uint8_t  *)0x20002000;

/*
 * Cell voltage balancing.
 *
 * Averages adjacent cell voltages indexed by 'cell_idx'. If the average
 * differs from the threshold by less than ±0x31, updates both entries.
 */
void fg_cell_balance(uint8_t cell_idx)
{
    uint16_t v1  = s_cell_voltage_table[cell_idx];
    uint16_t v2  = s_cell_voltage_table[cell_idx + 1];
    uint16_t avg = (v1 + v2) >> 1;

    if (*s_balance_threshold < avg) {
        if (avg <= *s_balance_threshold + 0x31) {
            s_cell_voltage_table[cell_idx]     = avg;
            s_cell_voltage_table[cell_idx + 1] = avg;
            *s_balance_idx = cell_idx;
        }
    } else if (*s_balance_threshold <= avg + 0x31) {
        s_cell_voltage_table[cell_idx]     = avg;
        s_cell_voltage_table[cell_idx + 1] = avg;
        *s_balance_idx = cell_idx;
    }
}

/*
 * Fuel gauge register field read (field starting at bit 8).
 *
 * Reads the tick counter at 0x200000C8, then right-shifts by
 * a value from the shift table at 0x080181F8, indexed by
 * (RCC_CFGR >> 8) & 7 (the APB1 prescaler field).
 * Used by the flash page program logic to determine the
 * clock scaling factor for timeout calculations.
 */
uint32_t fg_read_field_8(void)
{
    uint32_t val = *s_tick_counter;
    uint8_t shift_idx = (RCC_FG[3] >> 8) & 7;
    uint8_t shift = s_fg_shift_table[shift_idx];
    return val >> shift;
}

/*
 * Fuel gauge register field read (field starting at bit 11).
 *
 * Same as fg_read_field_8 but indexes by (RCC_CFGR >> 11) & 7
 * (the APB2 prescaler field).
 */
uint32_t fg_read_field_11(void)
{
    uint32_t val = *s_tick_counter;
    uint8_t shift_idx = (RCC_FG[3] >> 11) & 7;
    uint8_t shift = s_fg_shift_table[shift_idx];
    return val >> shift;
}

/*
 * Fuel gauge register read loop.
 *
 * Reads 16 registers from the FEDL5236 via SMBus.
 * For each register: reads 2 bytes, combines into 16-bit value,
 * multiplies by 0x4C50, divides by 1000, stores result.
 * Dispatches per-register callback from jump table at 0x08017470.
 * After 16 registers: writes 0x91 to FEDL5236 register 8 (cleanup).
 */
void fg_read_loop(void *ctx)
{
    volatile uint8_t  * const s_data_buf    = (volatile uint8_t  *)0x20002B84;
    volatile uint32_t * const s_index       = (volatile uint32_t *)0x2000287A;
    volatile uint32_t * const s_result_last = (volatile uint32_t *)0x20002824;
    volatile uint32_t * const s_result_base = (volatile uint32_t *)0x20002830;
    void (* const * const s_callback_tbl)(uint32_t) = (void (* const * const)(uint32_t))0x08017470;

    if ((*(volatile uint8_t *)((uintptr_t)ctx + 0x37) & 8) == 0) {
        extern void fg_read_done(void);
        fg_read_done();
        return;
    }

    if (smbus_read(0x34, 2) == 0) {
        extern void fg_read_done(void);
        fg_read_done();
        return;
    }

    uint32_t val = (uint32_t)s_data_buf[2] | ((uint32_t)s_data_buf[3] << 8);
    val = val * 0x4C50;
    val = val / 1000;

    if (*s_index == 9) {
        *s_result_last = val;
    } else {
        s_result_base[*s_index] = val;
    }

    *s_index += 1;

    if (*s_index < 0x10) {
        s_callback_tbl[*s_index](val);
    } else {
        smbus_write_reg(8, 0x91, 0xFF);
        extern void fg_read_done(void);
        fg_read_done();
    }
}

/*
 * Fuel gauge periodic scan — main FEDL5236 monitoring loop.
 *
 * Reads FEDL5236 register 3 (status), dispatches fault bits to
 * smbus_write_reg for clearing, triggers Coulomb counter update,
 * measures cell voltages, and handles charge/discharge transition
 * thresholds with debounce timers.
 */
void fg_scan(void)
{
    volatile uint32_t * const s_timer  = (volatile uint32_t *)0x20003590;
    volatile uint32_t * const s_gpio   = (volatile uint32_t *)0x50000400;

    if (((*s_timer & 1) == 0) && gpio_bit_read((uint32_t)s_gpio, 0x2000)) {
        extern void nop_4764(void);
        nop_4764();
        return;
    }

    *s_timer &= ~1U;

    if (smbus_read(3, 2) == 0) {
        extern void nop_4764(void);
        nop_4764();
        return;
    }

    volatile uint8_t *s_dbuf = (volatile uint8_t *)0x20002B84;
    volatile uint8_t *s_st2  = (volatile uint8_t *)0x20002C80;
    uint8_t status_byte = s_dbuf[2];
    *s_st2 = s_dbuf[3] & 2;

    if (*s_st2 != 0) {
        volatile uint32_t *s_fault = (volatile uint32_t *)0x20002C44;
        *s_fault |= 0x400;
        smbus_write_reg(4, 0, 0xFF);
        smbus_write_reg(3, 0, 0xFF);
        extern void nop_4764(void);
        nop_4764();
        return;
    }

    if ((status_byte & 0xF) != 0) {
        smbus_write_reg(3, 0, 0xFF);
    }

    if ((status_byte & 1) == 0) {
        extern void fg_coulomb_update(void);
        fg_coulomb_update();
    }

    /* Jump table dispatch for register index < 15 */
    volatile uint32_t *s_idx = (volatile uint32_t *)0x2000287A;
    void (* const * const s_jt)(void) = (void (* const * const)(void))0x08017470;

    if (*s_idx < 0xF) {
        s_jt[*s_idx]();
        return;
    }

    /* Threshold tracking for max/min values with debounce */
    extern void veneer_11f48(void);
    veneer_11f48();

    volatile uint32_t *s_max  = (volatile uint32_t *)0x200035B4;
    volatile uint32_t *s_min  = (volatile uint32_t *)0x200035C0;
    volatile uint32_t *s_thr  = (volatile uint32_t *)0x200035B8;

    volatile uint8_t *s_cnt1 = (volatile uint8_t *)0x200035BC;
    volatile uint8_t *s_cnt2 = (volatile uint8_t *)0x200035C4;

    if (*s_max < *s_thr) {
        uint8_t c = *s_cnt1 + 1;
        *s_cnt1 = c;
        if (c > 5) { *s_cnt1 = 0; *s_max = *s_thr; }
    } else { *s_cnt1 = 0; }

    if (*s_min < *(volatile uint32_t *)((uint8_t *)s_max + 0x10) ||
        *(volatile uint32_t *)((uint8_t *)s_max + 0x10) == 0) {
        uint8_t c = *s_cnt2 + 1;
        *s_cnt2 = c;
        if (c > 5) { *s_cnt2 = 0; *(volatile uint32_t *)((uint8_t *)s_max + 0x10) = *s_min; }
    } else { *s_cnt2 = 0; }

    /* Scan complete — write 0x91 marker, optionally read register 0x34 */
    smbus_write_reg(8, 0x91, 0xFF);

    if ((status_byte & 8) != 0) {
        extern void fg_read_loop(void *);
        fg_read_loop(NULL);
    } else {
        extern void nop_4764(void);
        nop_4764();
    }
}

/*
 * Fuel gauge Coulomb counter update.
 *
 * Reads register 0x2E from FEDL5236, computes current delta,
 * updates accumulated capacity, adjusts state-of-charge.
 */
void fg_coulomb_update(void)
{
    volatile uint32_t *s_cap    = (volatile uint32_t *)0x20003C10;
    volatile uint8_t  *s_dbuf   = (volatile uint8_t  *)0x20002B84;
    volatile uint32_t *s_delta  = (volatile uint32_t *)0x20003C18;

    if (*s_cap == 0) {
        if (smbus_read(0x2E, 2) != 0) {
            *s_cap = (uint32_t)s_dbuf[3] << 8 | s_dbuf[2];
            smbus_write_reg(6, 0x90, 0xFF);
            extern void fg_read_loop(void *);
            fg_read_loop(NULL);
        }
        *s_cap = 0;
        smbus_write_reg(6, 0x92, 0xFF);
        extern void fg_read_loop(void *);
        fg_read_loop(NULL);
        return;
    }

    if (smbus_read(0x2E, 2) == 0) {
        fg_read_loop(NULL);
        return;
    }

    uint32_t prev = *s_cap;
    uint32_t raw  = (uint32_t)s_dbuf[2] | ((uint32_t)s_dbuf[3] << 8);
    *s_delta = 0;

    volatile uint32_t *s_disc = (volatile uint32_t *)0x20003C1C;
    volatile uint32_t *s_chg  = (volatile uint32_t *)0x20003C20;
    *s_disc = 0; *s_chg = 0;

    if (raw < prev) {
        uint32_t diff = prev - raw;
        *s_delta = diff;
        *s_chg = *s_delta;
        *s_delta = ~*s_delta + 1;
    } else {
        uint32_t diff = raw - prev;
        *s_delta = diff;
        *s_disc = *s_delta;
    }
}

/*
 * Cell balancing voltage update algorithm.
 *
 * Runs a multi-phase cell balancing measurement cycle:
 *
 * Phase 1 — ADC acquisition (triggered by state timers):
 *   The function is called repeatedly (once per main-loop iteration).
 *   A cycle counter at 0x200024E0 counts 0–99. On each call:
 *     - Counter 0: pulses GPIO PA15 high (start of balance cycle)
 *     - Counter 10: calls dma_stop() (end of ADC DMA capture)
 *     - Counter 11+: checks flags byte (bit 0 at 0x20002554).
 *       When bit 0 is set, a sub-cycle counter at 0x20002550 is
 *       incremented. After 5 sub-cycles, the voltage computation
 *       phase runs.
 *
 * Phase 2 — Voltage computation:
 *   Reads 5 cells × 3 phases = 15 ADC values from the 30-byte array
 *   at 0x20002558 (uint16 per cell/phase). Each ADC value is
 *   converted to a voltage via:
 *     step1 = ADC * 610 * 610 / 10000
 *     step2 = (2500000 - step1) / 10000
 *   The result is scored against a 146-entry threshold lookup table
 *   at 0x08017698 (descending search, scoring 0–0x91).
 *
 * Phase 3 — Averaging and outlier rejection:
 *   Per-phase averages are computed across the 5 cells. Cells
 *   deviating by >5 from the average are clipped to the mean.
 *   A second-pass read of the corrected array computes per-phase
 *   mean voltages. Signed corrections (from 0x200025BE, 0x200025C4,
 *   0x200025BC) are applied.
 *
 * Phase 4 — EEPROM persistence:
 *   The computed phase voltages are compared against stored thresholds
 *   at 0x200029A8+0x44/0x45/0x46. If 5 consecutive readings exceed
 *   the threshold, the stored value is updated and written to data
 *   EEPROM at 0x08080C44/0x08080C45/0x08080C46 via memcmp_verify.
 */
void cell_balance_update(void)
{
    volatile uint32_t * const s_trigger  = (volatile uint32_t *)0x200024F4;
    volatile uint32_t * const s_adc_dr   = (volatile uint32_t *)0x40012400;
    volatile uint8_t  * const s_cycle    = (volatile uint8_t  *)0x200024E0;
    volatile uint8_t  * const s_flags    = (volatile uint8_t  *)0x20002554;
    volatile uint32_t * const s_subcnt   = (volatile uint32_t *)0x20002550;
    const uint32_t * const s_thresh_lut  = (const uint32_t *)0x08017698;
    extern void memcmp_verify(char *actual, uint32_t len, char *expected);

    /* Only run when trigger register matches ADC data register */
    if (*s_trigger != *s_adc_dr) {
        return;
    }

    uint8_t cycle = *s_cycle + 1;
    *s_cycle = cycle;
    if (cycle > 99) {
        *s_cycle = 0;
    }

    /* Phase boundaries */
    if (*s_cycle == 0) {
        gpio_bit_write(0x50000400, 0x8000, 1);
        return;
    } else if (*s_cycle == 10) {
        dma_stop();
        return;
    } else if ((*s_cycle > 10) && ((*s_flags & 1) != 0)) {
        *s_flags &= ~1U;
        gpio_bit_write(0x50000400, 0x8000, 0);

        uint8_t sub = (uint8_t)(*s_subcnt + 1);
        *s_subcnt = sub;

        if (sub < 5) {
            return;
        }
        *s_subcnt = 0;

        /* --- Voltage computation phase (sub-cycle 5+) --- */
        volatile uint16_t * const s_adc_buf = (volatile uint16_t *)0x20002558;
        volatile uint8_t  * const s_scores  = (volatile uint8_t  *)0x200024E4;
        const uint32_t MUL_FACTOR  = 610;
        const uint32_t DIV_FACTOR  = 10000;
        const uint32_t VREF_MV     = 2500000;

        uint8_t cell, phase;

        /* Step 1: score each of 5 cells × 3 phases */
        for (cell = 0; cell < 5; cell++) {
            for (phase = 0; phase < 3; phase++) {
                uint32_t adc  = s_adc_buf[cell * 4 + phase];
                uint32_t step = (adc * MUL_FACTOR * MUL_FACTOR) / DIV_FACTOR;
                uint32_t result = (VREF_MV - step) / DIV_FACTOR;

                uint8_t score = 0x91;
                /* Score against 146-entry descending threshold LUT */
                uint8_t s;
                for (s = 0x91; s != 0; s--) {
                    if (result <= s_thresh_lut[s]) {
                        score = s;
                        break;
                    }
                }
                s_scores[cell * 3 + phase] = score;
            }
        }

        /* Step 2: per-phase averaging with outlier rejection */
        volatile uint8_t * const s_scores2 = (volatile uint8_t *)0x200024E4;
        volatile uint8_t * const s_avg     = (volatile uint8_t *)0x20002588;

        uint16_t sum;
        uint8_t  mean;

        for (phase = 0; phase < 3; phase++) {
            sum = 0;
            for (cell = 0; cell < 5; cell++) {
                sum += s_scores2[cell * 3 + phase];
            }
            mean = (uint8_t)(sum / 5);

            /* Clip outliers: cells deviating by >5 from mean */
            for (cell = 0; cell < 5; cell++) {
                uint8_t val = s_scores2[cell * 3 + phase];
                if (mean < val) {
                    if ((int32_t)(val - mean) > 5) {
                        s_scores2[cell * 3 + phase] = mean;
                    }
                } else if (val < mean) {
                    if ((int32_t)(mean - val) > 5) {
                        s_scores2[cell * 3 + phase] = mean;
                    }
                }
            }

            /* Second-pass average of corrected values */
            sum = 0;
            for (cell = 0; cell < 5; cell++) {
                sum += s_scores2[cell * 3 + phase];
            }
            s_avg[phase] = (uint8_t)(sum / 5);
        }

        /* Step 3: apply per-phase corrections */
        volatile int8_t  * const s_corr0 = (volatile int8_t  *)0x200025BE;
        volatile int8_t  * const s_corr1 = (volatile int8_t  *)0x200025C4;
        volatile int8_t  * const s_corr2 = (volatile int8_t  *)0x200025BC;

        if (*s_corr0 != 0) {
            if (*s_corr0 < 0) {
                s_avg[0] = (uint8_t)((int8_t)s_avg[0] - (int8_t)(~*s_corr0 + 1));
            } else {
                s_avg[0] = (uint8_t)((int8_t)s_avg[0] + *s_corr0);
            }
        }
        if (*s_corr1 != 0) {
            if (*s_corr1 < 0) {
                s_avg[1] = (uint8_t)((int8_t)s_avg[1] - (int8_t)(~*s_corr1 + 1));
            } else {
                s_avg[1] = (uint8_t)((int8_t)s_avg[1] + *s_corr1);
            }
        }
        if (*s_corr2 != 0) {
            if (*s_corr2 < 0) {
                s_avg[2] = (uint8_t)((int8_t)s_avg[2] - (int8_t)(~*s_corr2 + 1));
            } else {
                s_avg[2] = (uint8_t)((int8_t)s_avg[2] + *s_corr2);
            }
        }

        /* Step 4: EEPROM persistence — update stored thresholds */
        volatile uint8_t  * const s_bms_cfg  = (volatile uint8_t  *)0x200029A8;
        volatile uint8_t  * const s_cnt_p0   = (volatile uint8_t  *)0x20002580;
        volatile uint8_t  * const s_cnt_p1   = (volatile uint8_t  *)0x2000258B;
        volatile uint8_t  * const s_cnt_p2   = (volatile uint8_t  *)0x20002581;

        /* Phase 0 threshold */
        if (s_bms_cfg[0x46] < s_avg[0]) {
            uint8_t c = *s_cnt_p0 + 1;
            *s_cnt_p0 = c;
            if (c > 5) {
                s_bms_cfg[0x46] = s_avg[0];
                volatile uint8_t * const s_buf_p0 = (volatile uint8_t *)0x200029EE;
                *s_buf_p0 = s_avg[0];
                memcmp_verify((char *)s_buf_p0, 1, (char *)0x08080C46);
            }
        } else {
            *s_cnt_p0 = 0;
        }

        /* Phase 1/2 thresholds — use larger of the two adjacent phases */
        uint8_t *s_phase1_thresh = &s_bms_cfg[0x44]; /* stored threshold for phase 1 */
        uint8_t *s_phase2_thresh = &s_bms_cfg[0x45]; /* stored threshold for phase 2 */

        if (s_avg[1] < s_avg[2] || s_avg[1] <= *s_phase1_thresh) {
            if (*s_phase1_thresh < s_avg[2]) {
                uint8_t c = *s_cnt_p1 + 1;
                *s_cnt_p1 = c;
                if (c > 5) {
                    *s_phase1_thresh = s_avg[2];
                    volatile uint8_t * const s_buf_p1 = (volatile uint8_t *)0x200029EC;
                    *s_buf_p1 = s_avg[2];
                    memcmp_verify((char *)s_buf_p1, 1, (char *)0x08080C44);
                }
            } else {
                *s_cnt_p1 = 0;
            }
        } else {
            uint8_t c = *s_cnt_p1 + 1;
            *s_cnt_p1 = c;
            if (c > 5) {
                *s_phase1_thresh = s_avg[1];
                volatile uint8_t * const s_buf_p1 = (volatile uint8_t *)0x200029EC;
                *s_buf_p1 = s_avg[1];
                memcmp_verify((char *)s_buf_p1, 1, (char *)0x08080C44);
            }
        }

        if (s_avg[2] < s_avg[1] || *s_phase2_thresh <= s_avg[1]) {
            if (s_avg[2] < *s_phase2_thresh) {
                uint8_t c = *s_cnt_p2 + 1;
                *s_cnt_p2 = c;
                if (c > 5) {
                    *s_phase2_thresh = s_avg[2];
                    volatile uint8_t * const s_buf_p2 = (volatile uint8_t *)0x200029ED;
                    *s_buf_p2 = s_avg[2];
                    memcmp_verify((char *)s_buf_p2, 1, (char *)0x08080C45);
                }
            } else {
                *s_cnt_p2 = 0;
            }
        } else {
            uint8_t c = *s_cnt_p2 + 1;
            *s_cnt_p2 = c;
            if (c > 5) {
                *s_phase2_thresh = s_avg[1];
                volatile uint8_t * const s_buf_p2 = (volatile uint8_t *)0x200029ED;
                *s_buf_p2 = s_avg[1];
                memcmp_verify((char *)s_buf_p2, 1, (char *)0x08080C45);
            }
        }
    }
}

/*
 * Calculate RSOC (FUN_08013860, 64 B) — convert raw accumulated charge
 * to a 0..5000-ish "promille" reading.
 *
 * Reads the u16 raw counter at SRAM 0x20002B86 (stored as two bytes
 * via byte loads in the OEM), scales it by 5000, then divides by
 * 4095 (= 0x0FFF) via the `veneer_1557c` thunk — which preserves r0
 * and tail-calls into the middle of another function that runs
 * `__aeabi_uidivmod(r0, r1)`, returning the quotient in r0.
 *
 *     return (uint16_t)((raw * 5000) / 4095);
 *
 * The literal pool slots 0x080138A4 (= 5000) and 0x080138A8 (= 0x0FFF)
 * are operands, not addresses — a previous decomp pass mistook them
 * for pointers.
 */
uint16_t calculate_rsoc(void)
{
    extern uint32_t veneer_1557c(uint32_t numerator, uint32_t divisor);

    uint16_t raw = *(volatile uint16_t *)0x20002B86;
    return (uint16_t)veneer_1557c((uint32_t)raw * 5000U, 0x0FFFU);
}

/*
 * coulomb_counter — signed charge/discharge accumulator + RSOC update.
 *
 * OEM: FUN_08013228, **~1496 B** (NOT 36 B as progress.md previously
 * claimed; size revised to 0x5D8).
 *
 * Takes an `int32_t` (positive = charge accumulation, negative =
 * discharge magnitude). Both branches share a long common tail at
 * OEM 0x08013706 that re-computes the RSOC percent from the modular
 * capacity counter.
 *
 *   Discharge path (arg < 0):
 *     val = -arg
 *     if val < cfg_b[+22] (mode threshold)             goto tail_short
 *     coulomb_sum (0x200025AC) -= val   (clamp to 0)
 *     if min_cell_volt (0x2000282A) <= 3020:
 *       veneer_1556c(val) → reset-counter handler
 *       if RSOC > 5:
 *         debounce_counter (0x200025A8)++; if > 11: rsoc_set(5); reset
 *       else: reset debounce
 *     else (min_cell_volt > 3020):
 *       reset debounce
 *       if min_cell_volt <= 3100 and RSOC > 6:
 *         veneer_1556c(val); decrement cfg[+0x24] by val (clamp to 0)
 *       else (min_cell_volt > 3100) and RSOC > 7:
 *         veneer_1556c(val); decrement cfg[+0x24] by val (clamp to 0)
 *     if dispatch_flag (0x2000281C) is zero → tail
 *     dispatch_flag = 0; *(u32)0x200025A4 = 0; *(u16)0x200025B4 = 0
 *     goto tail
 *
 *   Charge path (arg >= 0):
 *     val = arg
 *     if val >= cfg_b[+22]: goto common
 *     if val <= 49: goto tail_short
 *     if *(u16)0x2000281C >= *(u32)0x200028C8: goto tail_short
 *     common: stash val at [r7+16]
 *       if dispatch_flag == 0 and RSOC <= 99: dispatch_flag = coulomb_sum
 *       coulomb_sum += val
 *       (further OEM logic past 0x80133B2 not yet fully traced; treat as
 *        accumulate + various secondary updates)
 *
 *   Common tail (0x08013706):
 *     1.  clamp *(u32)0x20002588 to <= cal_a (= 0x20002B50)
 *     2.  clamp cfg[+0x24] to <= cal_a; if clamped, cfg[+0x28] = cfg_b[+6]
 *     3.  cfg[+0x2c] = cfg[+0x24]; if cfg[+0x2c] >= 0x3840,
 *         cfg[+0x2c] = cfg[+0x2c] / 0x3840 (via veneer_1557c); else 0
 *     4.  if cfg[+0x2c] > cfg[+0x28]: cfg[+0x2c] = cfg[+0x28]
 *     5.  RSOC = (cfg[+0x2c] * 100) / cfg[+0x28]
 *     6.  if cfg[+0x2c] != 0: RSOC++
 *     7.  RSOC clamped to <= 100
 *
 * The previous decomp used `uint32_t` and invented SRAM addresses
 * (0x200028C0 / 0x20003C18 / …) that don't appear in OEM. This rewrite
 * matches the OEM literal pool at 0x08013578..0x80135AC.
 */
void coulomb_counter(uint32_t val_u)
{
    extern uint32_t veneer_1557c(uint32_t numerator, uint32_t divisor);
    extern uint32_t veneer_1556c(uint32_t arg);  /* deferred thunk */
    extern void     rsoc_set(uint8_t percent);

    /* OEM treats r0 as int32_t. */
    int32_t arg = (int32_t)val_u;
    uint32_t val;

    volatile uint8_t  * const cfg_b      = (volatile uint8_t  *)0x200028D0;
    volatile uint8_t  * const cfg        = (volatile uint8_t  *)0x200029A8;
    volatile uint32_t * const coulomb    = (volatile uint32_t *)0x200025AC;
    volatile uint16_t * const min_cellv  = (volatile uint16_t *)0x2000282A;
    volatile uint8_t  * const debounce   = (volatile uint8_t  *)0x200025A8;
    volatile uint32_t * const dispatch   = (volatile uint32_t *)0x2000281C;
    volatile uint32_t * const aux32      = (volatile uint32_t *)0x200025A4;
    volatile uint16_t * const aux16      = (volatile uint16_t *)0x200025B4;
    volatile uint16_t * const flag281c   = (volatile uint16_t *)0x2000281C;
    volatile uint32_t * const flag28c8   = (volatile uint32_t *)0x200028C8;
    volatile uint32_t * const cap_high   = (volatile uint32_t *)0x20002588;
    volatile uint32_t * const cal_a      = (volatile uint32_t *)0x20002B50;

    const uint16_t MIN_THR_LO = 0x0BCC;        /* 3020 mV */
    const uint16_t MIN_THR_HI = 0x0C1C;        /* 3100 mV */
    const uint32_t MOD_DIVISOR = 0x3840U;      /* 14400  */

    if (arg < 0) {
        /* ---- Discharge ---- */
        val = (uint32_t)-arg;

        uint16_t mode_thr = *(volatile uint16_t *)(cfg_b + 22);
        if (val < mode_thr) {
            goto tail;
        }

        if (val > *coulomb) {
            *coulomb = 0;
        } else {
            *coulomb -= val;
        }

        if (*min_cellv <= MIN_THR_LO) {
            (void)veneer_1556c(val);
            if (cfg[0x36] > 5) {
                uint8_t c = (uint8_t)(*debounce + 1);
                *debounce = c;
                if (c > 11) {
                    *debounce = 0;
                    rsoc_set(5);
                }
            } else {
                *debounce = 0;
            }
        } else {
            *debounce = 0;
            if (*min_cellv <= MIN_THR_HI) {
                if (cfg[0x36] > 6) {
                    (void)veneer_1556c(val);
                    uint32_t cap = *(volatile uint32_t *)(cfg + 0x24);
                    if (cap > *coulomb) {
                        if (val <= cap) {
                            *(volatile uint32_t *)(cfg + 0x24) = cap - val;
                        } else {
                            *(volatile uint32_t *)(cfg + 0x24) = 0;
                        }
                    }
                }
            } else {
                if (cfg[0x36] > 7) {
                    (void)veneer_1556c(val);
                    uint32_t cap = *(volatile uint32_t *)(cfg + 0x24);
                    if (cap > *coulomb) {
                        if (val <= cap) {
                            *(volatile uint32_t *)(cfg + 0x24) = cap - val;
                        } else {
                            *(volatile uint32_t *)(cfg + 0x24) = 0;
                        }
                    }
                }
            }
        }

        if (*dispatch != 0) {
            *dispatch = 0;
            *aux32 = 0;
            *aux16 = 0;
        }
        goto tail;
    }

    /* ---- Charge path (arg >= 0) ---- */
    val = (uint32_t)arg;

    uint16_t mode_thr = *(volatile uint16_t *)(cfg_b + 22);
    if (val < mode_thr) {
        if (val <= 49U) goto tail;
        if (*flag281c >= *flag28c8) goto tail;
    }

    /* Common accumulate: stash, gate, add. */
    if (*dispatch == 0 && cfg[0x36] <= 99) {
        *dispatch = *coulomb;
    }
    *coulomb += val;

    /* (Further OEM logic past 0x080133B2 not yet fully traced. The
     * unstudied tail there updates secondary capacity counters and
     * some configuration bytes; landing it correctly will require
     * another disasm pass through 0x080133BE..0x08013704.) */

tail:
    /* ---- Common tail (OEM 0x08013706) ---- */
    if (*cap_high < *cal_a) {
        /* no-op: only writes if cap_high >= cal_a (clamp down) */
    } else {
        *cap_high = *cal_a;
    }

    if (*(volatile uint32_t *)(cfg + 0x24) >= *cal_a) {
        *(volatile uint32_t *)(cfg + 0x24) = *cal_a;
        *(volatile uint32_t *)(cfg + 0x28) = *(volatile uint16_t *)(cfg_b + 6);
    }

    *(volatile uint32_t *)(cfg + 0x2C) = *(volatile uint32_t *)(cfg + 0x24);
    if (*(volatile uint32_t *)(cfg + 0x2C) >= MOD_DIVISOR) {
        *(volatile uint32_t *)(cfg + 0x2C) =
            veneer_1557c(*(volatile uint32_t *)(cfg + 0x2C), MOD_DIVISOR);
    } else {
        *(volatile uint32_t *)(cfg + 0x2C) = 0;
    }

    if (*(volatile uint32_t *)(cfg + 0x2C) > *(volatile uint32_t *)(cfg + 0x28)) {
        *(volatile uint32_t *)(cfg + 0x2C) = *(volatile uint32_t *)(cfg + 0x28);
    }

    uint32_t soc = *(volatile uint32_t *)(cfg + 0x2C) * 100U;
    soc = veneer_1557c(soc, *(volatile uint32_t *)(cfg + 0x28));
    cfg[0x36] = (uint8_t)soc;

    if (*(volatile uint32_t *)(cfg + 0x2C) != 0) {
        cfg[0x36]++;
    }
    if (cfg[0x36] > 100) {
        cfg[0x36] = 100;
    }
}

/*
 * cell_voltage_scan — two-pass cell analysis + balance triggering.
 *
 * OEM: FUN_080138AC. progress.md previously claimed 940 B; the real
 * body runs 0x080138AC..0x08013D58 = **1196 B**.
 *
 * Cell-table layout (10 u16 entries, one per cell, populated by
 * `bms_init`):
 *
 *   0x200027D4   cell voltages, u16[N]   (N = cell_count = cfg_b[+4])
 *   0x200027FE   cached cell_count
 *
 *   0x200027F0   fault-flag word (cell-pair mismatch bits)
 *   0x200027FA   running max voltage  (second pass)
 *   0x20002818   min index            (second pass)
 *   0x2000281C   sum accumulator      (second pass)
 *   0x2000282A   running min voltage  (second pass)
 *   0x2000282C   max of the secondary table
 *   0x2000286E   average of middle cells: (sum - max - min) >> 3
 *   0x20002880   secondary cell-voltage table (u16[N])
 *   0x2000289A   min of the secondary table
 *   0x200028A4   tertiary copy table (u16[N])
 *   0x200028CC   max index            (second pass)
 *   0x200028D0   cfg_b (cell count at +4)
 *
 * The earlier decomp used a single-pass min/max scan over a wrong
 * cell table (0x20002880, the *secondary* table, not the primary)
 * with cell_count hard-coded to 10 and ignored the cell-pair fault
 * marker, the middle-cell average, and the per-pair balance
 * triggering. This rewrite implements the full OEM structure.
 *
 * Pass 1 (first-half analysis):
 *   - reset many flags/maxes/mins to their sentinels
 *   - cache cell_count
 *   - scan cells, find max (with index) and min (with index) plus sum
 *   - average = (sum - max - min) >> 3
 *   - if (max - min) > 49: enter pass-1b balance loop
 *
 * Pass 1b (balance loop):
 *   for i = 0 .. cell_count-2:
 *     if i == 4 (middle cell, polarity flip in 10-cell pack):
 *        require max_idx == i AND min_idx == i+1 AND v[i] < v[i+1]
 *        → fg_cell_balance(i)
 *     else:
 *        require min_idx == i AND max_idx == i+1 AND v[i] > v[i+1]
 *        → fg_cell_balance(i)
 *
 * Mismatch switch (cached_cell_count, fault-flag bit pairs):
 *   case k (k in 0..8): set bits k and k+1 in *(u16)0x200027F0.
 *   The jump table sits at 0x08017050 (past the .bin — bootloader
 *   region). Implemented here as a switch since the body of each
 *   case is just two bit-OR-and-store instructions.
 *
 * Outlier patch:
 *   if min_idx == 0 and min_v >= 2000: cell[0] = cell[1]; flag bit 0
 *   if min_idx == 9 and min_v >= 2000: cell[9] = cell[8]; flag bit 9
 *
 * Pass 2:
 *   for i = 0 .. cell_count-1:
 *     update running max/min (with idx) at 0x200027FA / 0x2000282A
 *     update running max/min of secondary table at 0x2000282C / 0x2000289A
 *     copy primary[i] to tertiary table at 0x200028A4 + 2*i
 *     accumulate sum at 0x2000281C
 */
void cell_voltage_scan(void)
{
    extern void fg_cell_balance(uint8_t cell_idx);

    volatile uint8_t  * const cfg_b          = (volatile uint8_t  *)0x200028D0;
    volatile uint16_t * const cell_tbl       = (volatile uint16_t *)0x200027D4;
    volatile uint16_t * const second_tbl     = (volatile uint16_t *)0x20002880;
    volatile uint16_t * const third_tbl      = (volatile uint16_t *)0x200028A4;
    volatile uint8_t  * const cached_count   = (volatile uint8_t  *)0x200027FE;
    volatile uint16_t * const fault_flags    = (volatile uint16_t *)0x200027F0;
    volatile uint16_t * const mid_average    = (volatile uint16_t *)0x2000286E;

    volatile uint16_t * const max_volt_p2    = (volatile uint16_t *)0x200027FA;
    volatile uint8_t  * const max_idx_p2     = (volatile uint8_t  *)0x200028CC;
    volatile uint16_t * const min_volt_p2    = (volatile uint16_t *)0x2000282A;
    volatile uint8_t  * const min_idx_p2     = (volatile uint8_t  *)0x20002818;
    volatile uint16_t * const sec_max        = (volatile uint16_t *)0x2000282C;
    volatile uint16_t * const sec_min        = (volatile uint16_t *)0x2000289A;
    volatile uint16_t * const sum_accum      = (volatile uint16_t *)0x2000281C;

    /* OEM stack locals (mirroring the [r7+N] uses): */
    uint16_t pass1_sum  = 0;     /* [r7+12] */
    uint16_t pass1_max  = 0;     /* [r7+10] */
    uint8_t  pass1_maxi = 0;     /* [r7+9]  */
    uint16_t pass1_min  = 0xFFFFu; /* [r7+6]  (init -1 → real min after scan) */
    uint8_t  pass1_mini = 0;     /* [r7+5]  */
    uint8_t  i;                  /* [r7+15] */

    uint8_t cell_count = cfg_b[4];
    *cached_count      = cell_count;
    *fault_flags       = 0;
    *mid_average       = 0;
    *max_volt_p2       = 0;
    /* OEM sets 0x2000282C := 0 and 0x200028CC := 0xFFFF as well, then
     * idx markers, etc. */
    *(volatile uint16_t *)0x2000282C = 0;
    *(volatile uint8_t  *)0x200028CC = 0;
    *(volatile uint16_t *)0x2000282A = 0xFFFFu;
    *(volatile uint16_t *)0x2000289A = 0xFFFFu;
    *(volatile uint8_t  *)0x20002818 = 0;
    *(volatile uint16_t *)0x2000281C = 0;

    /* --- Pass 1: max/min/sum -------------------------------- */
    for (i = 0; i < cell_count; i++) {
        uint16_t v = cell_tbl[i];
        pass1_sum = (uint16_t)(pass1_sum + v);
        if (v > pass1_max) {        /* OEM: bcs skip if max>=v, so update when max<v */
            pass1_max  = v;
            pass1_maxi = i;
        }
        if (v < pass1_min) {        /* OEM: bls skip if min<=v */
            pass1_min  = v;
            pass1_mini = i;
        }
    }

    /* average of middle cells = (sum - max - min) >> 3 */
    *mid_average = (uint16_t)(((uint32_t)pass1_sum - pass1_max - pass1_min) >> 3);

    if ((uint32_t)pass1_max - pass1_min > 49U) {
        /* --- Pass 1b: balance triggering ------------------- */
        for (i = 0; i < (uint8_t)(cell_count - 1); i++) {
            if (i == 4) {
                if (pass1_maxi == i && pass1_mini == (uint8_t)(i + 1)) {
                    uint16_t v0 = cell_tbl[i];
                    uint16_t v1 = cell_tbl[i + 1];
                    if (v0 < v1) {
                        fg_cell_balance(i);
                    }
                }
            } else {
                if (pass1_mini == i && pass1_maxi == (uint8_t)(i + 1)) {
                    uint16_t v0 = cell_tbl[i];
                    uint16_t v1 = cell_tbl[i + 1];
                    if (v0 > v1) {
                        fg_cell_balance(i);
                    }
                }
            }
        }

        /* --- Mismatch switch (cached_cc < cell_count cases) - */
        if (cell_count != *cached_count) {
            uint8_t k = *cached_count;
            if (k <= 8) {
                /* OEM jumps into a flash table at 0x08017050. Each case
                 * sets bits k and k+1 in fault_flags. Open-coded: */
                *fault_flags = (uint16_t)(*fault_flags | (1U << k) | (1U << (k + 1)));
            }
        }

        /* --- Outlier patch: edge cells with min >= 2000 ----- */
        if (cell_count == *cached_count && pass1_min >= 2000U) {
            if (pass1_mini == 0) {
                cell_tbl[0]   = cell_tbl[1];
                *fault_flags |= 0x0001U;
            } else if (pass1_mini == 9) {
                cell_tbl[9]   = cell_tbl[8];
                *fault_flags |= 0x0200U;
            }
        }
    }

    /* --- Pass 2: rebuild running max/min trackers ----------- */
    for (i = 0; i < cell_count; i++) {
        uint16_t v = cell_tbl[i];

        if (v > *max_volt_p2) {
            *max_volt_p2 = v;
            *max_idx_p2  = i;
        }
        if (v < *min_volt_p2) {
            *min_volt_p2 = v;
            *min_idx_p2  = i;
        }
        if (second_tbl[i] > *sec_max) {
            *sec_max = second_tbl[i];
        }
        if (second_tbl[i] < *sec_min) {
            *sec_min = second_tbl[i];
        }
        third_tbl[i] = v;
        *sum_accum   = (uint16_t)(*sum_accum + v);
    }
}

/*
 * config_init — load + validate the BMS context struct from EEPROM.
 *
 * OEM: FUN_08007368 (1220 B). Two SRAM structs participate:
 *
 *   s_ctx @ 0x200028D0  — primary configuration context (~208 B).
 *                         Filled with hard-coded factory defaults
 *                         throughout, with selected fields validated
 *                         against EEPROM mirrors.
 *   s_cfg @ 0x200029A8  — secondary 184-byte struct (zeroed up
 *                         front, then a single byte at +0x39 set
 *                         to 1; cfg[+0x40] / [+0x42] cleared during
 *                         one of the recovery paths).
 *
 * The OEM does NOT compare SRAM↔EEPROM with `memcmp_verify` directly.
 * For each EEPROM-mirrored field it follows a strict pattern:
 *
 *     1. `memcpy_oem(src=EEPROM, count, dst=&local_stack)` reads
 *        the EEPROM bytes into a local stack slot.
 *     2. Validate: usually `local == 0 || tmp32 != FW_MAGIC`. Some
 *        fields use only the low byte (`ldrb`); the magic field is
 *        a single u32.
 *     3. If invalid: overwrite the local with the fallback default,
 *        then `memcmp_verify(EEPROM, count, &local)` to persist
 *        the new value back via SPI-poll write.
 *     4. Either way, copy the (validated or fallback) local into
 *        the corresponding `s_ctx` field.
 *
 * FW_MAGIC is the OEM 4-byte version stamp `0x011701B1` (= "1.17.1
 * BMS"), originally read from EEPROM 0x0808002E into `tmp32` at the
 * top of the function and re-persisted at the bottom if the EEPROM
 * copy disagrees.
 *
 * The bulk of the body (offsets +6 .. +200 of `s_ctx`) is
 * factory-default initialisation — large blocks of hard-coded
 * `*(u16 *)(s_ctx + N) = LITERAL;` writes that are *not* read back
 * from EEPROM. Five of the fields (+2, +14, +16, +18, +68/+69/+70)
 * are EEPROM-validated.
 */
void config_init(void)
{
    uint32_t tmp32 = 0;
    uint16_t tmp16 = 0;

    volatile uint8_t  * const s_ctx = (volatile uint8_t *)0x200028D0;
    volatile uint8_t  * const s_cfg = (volatile uint8_t *)0x200029A8;
    const uint32_t FW_MAGIC = 0x011701B1U;

    extern void memcmp_verify(char *actual, uint32_t len, char *expected);

    *(volatile uint32_t *)0x20002C74 = 0;

    for (uint16_t i = 0; i <= 0xB7; i++) {
        s_cfg[i] = 0;
    }

    /* s_ctx[0..1] sentinel — OEM: `r2 = 24; r2 += 255; strh r2,[r3,#0]`. */
    *(volatile uint16_t *)(s_ctx + 0) = 0x0117U;

    /* --- 4-byte FW magic into tmp32 ---------------- */
    memcpy_oem((const uint8_t *)0x0808002E, 4, (uint8_t *)&tmp32);

    s_cfg[0x39] = 1;

    /* --- s_ctx[+2] (u16) — EEPROM 0x08080006, fallback 0x310 ---- */
    memcpy_oem((const uint8_t *)0x08080006, 2, (uint8_t *)&tmp16);
    if (tmp16 == 0 || tmp32 != FW_MAGIC) {
        tmp16 = 0x0310U;
        memcmp_verify((char *)0x08080006, 2, (char *)&tmp16);
    }
    *(volatile uint16_t *)(s_ctx + 2) = tmp16;

    s_ctx[4]                              = 10;
    *(volatile uint16_t *)(s_ctx + 6)     = 0x3138U;
    *(volatile uint16_t *)(s_ctx + 8)     = 0x9E34U;
    *(volatile uint16_t *)(s_ctx + 10)    = 0x012CU;
    *(volatile uint16_t *)(s_ctx + 12)    = 0x00F0U;

    /* --- s_ctx[+14] (u16) — EEPROM 0x08080032, fallback 0x0D01 -- */
    memcpy_oem((const uint8_t *)0x08080032, 2, (uint8_t *)&tmp16);
    if (tmp16 == 0 || tmp32 != FW_MAGIC) {
        tmp16 = 0x0D01U;
        memcmp_verify((char *)0x08080032, 2, (char *)&tmp16);
    }
    *(volatile uint16_t *)(s_ctx + 14)    = tmp16;

    /* --- s_ctx[+16] (u8) — EEPROM 0x08080034, fallback 7
     * (OEM tests only the low byte via `ldrb`). */
    memcpy_oem((const uint8_t *)0x08080034, 2, (uint8_t *)&tmp16);
    if ((tmp16 & 0xFFU) == 0 || tmp32 != FW_MAGIC) {
        tmp16 = (uint16_t)((tmp16 & 0xFF00U) | 7U);
        memcmp_verify((char *)0x08080034, 2, (char *)&tmp16);
    }
    s_ctx[16]                             = (uint8_t)tmp16;

    /* --- s_ctx[+18] (u16) — EEPROM 0x08080036, fallback 0x0C4E -- */
    memcpy_oem((const uint8_t *)0x08080036, 2, (uint8_t *)&tmp16);
    if (tmp16 == 0 || tmp32 != FW_MAGIC) {
        tmp16 = 0x0C4EU;
        memcmp_verify((char *)0x08080036, 2, (char *)&tmp16);
    }
    *(volatile uint16_t *)(s_ctx + 18)    = tmp16;

    /* --- Hard-coded factory defaults, offsets +20 .. +200 ------- */
    s_ctx[20]                             = 0;
    *(volatile uint16_t *)(s_ctx + 22)    = 100;
    *(volatile uint16_t *)(s_ctx + 24)    = 4000;
    *(volatile uint32_t *)(s_ctx + 28)    = 0x000061A8U; /* 25000 */
    *(volatile uint32_t *)(s_ctx + 36)    = 0x00002710U; /* 10000 */
    *(volatile uint16_t *)(s_ctx + 42)    = 0x109AU;
    *(volatile uint16_t *)(s_ctx + 44)    = 0x0BB8U;     /* 3000 */
    *(volatile uint16_t *)(s_ctx + 46)    = 0x1036U;
    *(volatile uint16_t *)(s_ctx + 48)    = 300;
    *(volatile uint16_t *)(s_ctx + 50)    = 0x10CCU;
    *(volatile uint16_t *)(s_ctx + 52)    = 300;
    *(volatile uint16_t *)(s_ctx + 54)    = 0x1036U;
    *(volatile uint16_t *)(s_ctx + 56)    = 300;
    *(volatile uint16_t *)(s_ctx + 58)    = 0x0BB8U;
    *(volatile uint16_t *)(s_ctx + 60)    = 0x1770U;     /* 6000 */
    *(volatile uint16_t *)(s_ctx + 62)    = 0x0CE4U;     /* 3300 */
    *(volatile uint16_t *)(s_ctx + 64)    = 300;
    *(volatile uint16_t *)(s_ctx + 66)    = 0x0AF0U;     /* 2800 */
    *(volatile uint16_t *)(s_ctx + 68)    = 0x03E8U;     /* 1000 (overridden below) */
    *(volatile uint16_t *)(s_ctx + 70)    = 0x0CE4U;     /* 3300 (overridden below) */
    *(volatile uint16_t *)(s_ctx + 72)    = 300;
    *(volatile uint16_t *)(s_ctx + 74)    = 0x1194U;     /* 4500 */
    *(volatile uint16_t *)(s_ctx + 76)    = 0x07D0U;     /* 2000 */
    *(volatile uint16_t *)(s_ctx + 78)    = 0x1194U;
    *(volatile uint16_t *)(s_ctx + 80)    = 0x1770U;
    *(volatile uint16_t *)(s_ctx + 82)    = 300;
    *(volatile uint16_t *)(s_ctx + 84)    = 0x1194U;
    *(volatile uint16_t *)(s_ctx + 86)    = 0x7530U;     /* 30000 */
    *(volatile uint16_t *)(s_ctx + 88)    = 0x07D0U;
    *(volatile uint16_t *)(s_ctx + 90)    = 0x1194U;
    *(volatile uint16_t *)(s_ctx + 92)    = 0xC350U;     /* 50000 */
    *(volatile uint16_t *)(s_ctx + 94)    = 300;
    *(volatile uint16_t *)(s_ctx + 96)    = 0x1194U;
    *(volatile uint16_t *)(s_ctx + 98)    = 0x1194U;
    *(volatile uint16_t *)(s_ctx + 100)   = 150;
    *(volatile uint16_t *)(s_ctx + 102)   = 0x0BB8U;
    *(volatile uint16_t *)(s_ctx + 104)   = 0x1194U;
    *(volatile uint16_t *)(s_ctx + 106)   = 300;
    *(volatile uint16_t *)(s_ctx + 108)   = 500;
    s_ctx[110]                            = 0x55;
    *(volatile uint16_t *)(s_ctx + 112)   = 0x0BB8U;
    s_ctx[114]                            = 0x52;
    *(volatile uint16_t *)(s_ctx + 116)   = 0x05DCU;     /* 1500 */
    s_ctx[118]                            = 40;
    *(volatile uint16_t *)(s_ctx + 120)   = 0x0BB8U;
    s_ctx[122]                            = 43;
    *(volatile uint16_t *)(s_ctx + 124)   = 0x05DCU;
    s_ctx[126]                            = 110;
    *(volatile uint16_t *)(s_ctx + 128)   = 0x0BB8U;
    s_ctx[130]                            = 100;
    *(volatile uint16_t *)(s_ctx + 132)   = 0x05DCU;
    s_ctx[134]                            = 20;
    *(volatile uint16_t *)(s_ctx + 136)   = 0x0BB8U;
    s_ctx[138]                            = 23;
    *(volatile uint16_t *)(s_ctx + 140)   = 0x05DCU;
    s_ctx[142]                            = 135;
    *(volatile uint16_t *)(s_ctx + 144)   = 0x0BB8U;
    s_ctx[146]                            = 125;
    *(volatile uint16_t *)(s_ctx + 148)   = 0x05DCU;
    *(volatile uint16_t *)(s_ctx + 150)   = 0x07D0U;
    *(volatile uint16_t *)(s_ctx + 152)   = 500;
    *(volatile uint16_t *)(s_ctx + 154)   = 500;
    s_ctx[156]                            = 10;
    *(volatile uint16_t *)(s_ctx + 158)   = 0x1081U;
    *(volatile uint16_t *)(s_ctx + 160)   = 0x0BB8U;
    *(volatile uint16_t *)(s_ctx + 162)   = 0x0C80U;     /* 3200 */
    *(volatile uint16_t *)(s_ctx + 164)   = 0x0BB8U;
    /* +166 / +168 — OEM skips. s_ctx is not zeroed beforehand. */
    *(volatile uint16_t *)(s_ctx + 170)   = 0xC350U;
    *(volatile uint16_t *)(s_ctx + 172)   = 0x0BB8U;
    *(volatile uint16_t *)(s_ctx + 174)   = 0x61A8U;
    *(volatile uint16_t *)(s_ctx + 176)   = 0x0BB8U;
    *(volatile uint16_t *)(s_ctx + 178)   = 120;
    *(volatile uint16_t *)(s_ctx + 180)   = 0x02BCU;     /* 700 */
    s_ctx[182]                            = 0x52;
    *(volatile uint16_t *)(s_ctx + 184)   = 0x0BB8U;
    s_ctx[186]                            = 0x2B;
    *(volatile uint16_t *)(s_ctx + 188)   = 0x0BB8U;
    s_ctx[190]                            = 0x69;
    *(volatile uint16_t *)(s_ctx + 192)   = 0x0BB8U;
    s_ctx[194]                            = 0x1E;
    *(volatile uint16_t *)(s_ctx + 196)   = 0x0BB8U;
    s_ctx[198]                            = 0x7D;
    *(volatile uint16_t *)(s_ctx + 200)   = 0x0BB8U;

    s_ctx[5] = 0;

    /* --- s_ctx[+70] (u8) — EEPROM 0x08080C46, fallback 'A' ------- */
    memcpy_oem((const uint8_t *)0x08080C46, 1, (uint8_t *)&tmp16);
    if ((tmp16 & 0xFFU) == 0) {
        tmp16 = (uint16_t)((tmp16 & 0xFF00U) | 0x41U);
        memcmp_verify((char *)0x08080C46, 1, (char *)&tmp16);
    }
    s_ctx[70]                             = (uint8_t)tmp16;

    /* --- s_ctx[+68] (u8) — EEPROM 0x08080C44, fallback 'A'.
     * The "missing" path also zeroes cfg[+0x40] / cfg[+0x42] and
     * persists those zeros to EEPROM at 0x08080C40 / 0x08080C42. */
    memcpy_oem((const uint8_t *)0x08080C44, 1, (uint8_t *)&tmp16);
    if ((tmp16 & 0xFFU) == 0) {
        tmp16 = (uint16_t)((tmp16 & 0xFF00U) | 0x41U);
        memcmp_verify((char *)0x08080C44, 1, (char *)&tmp16);

        *(volatile uint16_t *)(s_cfg + 0x40) = 0;
        memcmp_verify((char *)0x08080C40, 2, (char *)(s_cfg + 0x40));
        *(volatile uint16_t *)(s_cfg + 0x42) = 0;
        memcmp_verify((char *)0x08080C42, 2, (char *)(s_cfg + 0x42));
    }
    s_ctx[68]                             = (uint8_t)tmp16;

    /* --- s_ctx[+69] (u8) — EEPROM 0x08080C45, fallback 'A' ------- */
    memcpy_oem((const uint8_t *)0x08080C45, 1, (uint8_t *)&tmp16);
    if ((tmp16 & 0xFFU) == 0) {
        tmp16 = (uint16_t)((tmp16 & 0xFF00U) | 0x41U);
        memcmp_verify((char *)0x08080C45, 1, (char *)&tmp16);
    }
    s_ctx[69]                             = (uint8_t)tmp16;

    /* If EEPROM FW magic didn't match, persist the canonical stamp. */
    if (tmp32 != FW_MAGIC) {
        tmp32 = FW_MAGIC;
        memcmp_verify((char *)0x0808002E, 4, (char *)&tmp32);
    }
}

