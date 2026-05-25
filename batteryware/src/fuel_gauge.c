#include "batteryware.h"

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

    extern int smbus_read(uint8_t addr, uint8_t count);
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
        extern void smbus_write_reg(uint8_t addr, uint8_t val, uint8_t mask);
        smbus_write_reg(8, 0x91, 0xFF);
        extern void fg_read_done(void);
        fg_read_done();
    }
}
