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
