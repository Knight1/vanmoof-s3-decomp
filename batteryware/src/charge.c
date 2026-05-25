#include "batteryware.h"

/* SRAM status register — bit 6 tracks charge MOSFET state */
static volatile uint32_t * const s_mosfet_status = (volatile uint32_t *)0x20002C00;

/*
 * Charge MOSFET control via GPIOB pin 9 (bit 0x200).
 *
 * Idempotent — only toggles the GPIO if the cached state in the SRAM
 * status register differs. Sets/clears bit 6 to track current state.
 */
void charge_mosfet_set(bool on)
{
    if (on) {
        if (((*s_mosfet_status >> 6) & 1) == 0) {
            *s_mosfet_status |= 0x40;
            gpio_bit_write(0x50000400, 0x200, 1);
        }
    } else {
        if (((*s_mosfet_status >> 6) & 1) != 0) {
            *s_mosfet_status &= ~0x40U;
            gpio_bit_write(0x50000400, 0x200, 0);
        }
    }
}

/* Config/status register for discharge MOSFET */
static volatile uint8_t * const s_discharge_cfg = (volatile uint8_t *)0x20002870;

/*
 * Discharge MOSFET control.
 *
 * If the pre-discharge bit (bit 12) in s_mosfet_status is clear,
 * forces the charge MOSFET off. Otherwise, if 'on' is set and the
 * discharge MOSFET is not already enabled (bit 1 of s_discharge_cfg),
 * enables it and reconfigures the BMS. If 'on' is clear and the
 * discharge MOSFET is enabled, disables it.
 */
void discharge_mosfet_set(bool on)
{
    if (((*s_mosfet_status >> 12) & 1) == 0) {
        charge_mosfet_set(false);
        return;
    }

    if (on) {
        if ((*s_discharge_cfg & 2) == 2) {
            return;  /* already on */
        }
        *s_discharge_cfg |= 2;
        extern void bms_configure(uint8_t cfg);  /* FUN_080052d8 */
        bms_configure(*s_discharge_cfg);
    } else {
        if ((*s_discharge_cfg & 2) == 2) {
            *s_discharge_cfg &= ~2U;
            bms_configure(*s_discharge_cfg);
        }
    }
}

/*
 * Charge MOSFET turn-on — GPIOB pin 2 high.
 */
void charge_mosfet_on(void)
{
    *(volatile uint32_t *)0x20002C48 = 0;
    *(volatile uint32_t *)0x20002C4C |= 0x400;
    gpio_bit_write(0x50000400, 2, 1);
}

/*
 * Charge MOSFET turn-off — GPIOB pin 2 low.
 */
void charge_mosfet_off(void)
{
    *(volatile uint32_t *)0x20002C50 = 0;
    *(volatile uint32_t *)0x20002C54 &= 0xFFFFFBFF;
    gpio_bit_write(0x50000400, 2, 0);
}
