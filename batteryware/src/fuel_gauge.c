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
