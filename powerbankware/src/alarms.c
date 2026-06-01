#include "powerbankware.h"

/*
 * Slow-tick alarm-debounce monitors. Each runs once per slow tick from the
 * state super-loop and watches one operating limit; a condition that persists
 * past its debounce count latches a request bit in the alarm word 0x200006E4
 * (and, for the last three, stamps a fault-type code into BMS-record byte
 * 0x200004D0+0x5C). The state handler then maps those request bits to the
 * dedicated fault-state entries. Translated from this image.
 *
 * Inputs: cal/limit block 0x20000218 (per-sensor bytes [1]/[2]); the latched
 * count is its own SRAM word per monitor. Debounce is 0x3B (≈60 ticks) for the
 * threshold monitors, 0x13 (20) for the PUPIN GPIO monitor.
 *
 *   alarm_scan_b0..b7 = OEM FUN_08013C10 / C60 / CB0 / D00 / D50 / D98 / DF4 / E64
 */

#define LIMIT ((volatile uint8_t  *)0x20000218)
#define REQ   (*(volatile uint16_t *)0x200006E4)
#define FETB  (*(volatile uint8_t  *)0x20000412)   /* reg-9 FET-control shadow */
#define RECF  (*(volatile uint8_t  *)0x2000052C)   /* record +0x5C fault-type  */

/* bit0: limit[1] & limit[2] both ≥ 0x55 sustained */
void alarm_scan_b0(void)
{
    volatile uint16_t *cnt = (volatile uint16_t *)0x20000550;
    if (LIMIT[1] < 0x55 && LIMIT[2] < 0x55) {
        *cnt = 0;
    } else if (++*cnt > 0x3B) {
        REQ |= 1;
    }
}

/* bit1: limit[1] or limit[2] < 0x29 sustained */
void alarm_scan_b1(void)
{
    volatile uint16_t *cnt = (volatile uint16_t *)0x20000580;
    if (LIMIT[1] < 0x29 || LIMIT[2] < 0x29) {
        if (++*cnt > 0x3B) {
            REQ |= 2;
        }
    } else {
        *cnt = 0;
    }
}

/* bit2: limit[1] & limit[2] both ≥ 0x6E sustained */
void alarm_scan_b2(void)
{
    volatile uint16_t *cnt = (volatile uint16_t *)0x20000584;
    if (LIMIT[1] < 0x6E && LIMIT[2] < 0x6E) {
        *cnt = 0;
    } else if (++*cnt > 0x3B) {
        REQ |= 4;
    }
}

/* bit3: limit[1] or limit[2] < 0x15 sustained */
void alarm_scan_b3(void)
{
    volatile uint16_t *cnt = (volatile uint16_t *)0x200004CA;
    if (LIMIT[1] < 0x15 || LIMIT[2] < 0x15) {
        if (++*cnt > 0x3B) {
            REQ |= 8;
        }
    } else {
        *cnt = 0;
    }
}

/* bit4: SOC-index byte (0x20000218[0]) ≥ 0x82 sustained */
void alarm_scan_b4(void)
{
    volatile uint16_t *cnt = (volatile uint16_t *)0x20000588;
    if (*(volatile uint8_t *)0x20000218 < 0x82) {
        *cnt = 0;
    } else if (++*cnt > 0x3B) {
        REQ |= 0x10;
    }
}

/* bit5: PUPIN (GPIOB PB6) held low for 0x13 ticks -> fault code 3 */
void alarm_scan_b5(void)
{
    volatile uint8_t *cnt = (volatile uint8_t *)0x2000058D;
    if (!gpio_bit_read(0x48000400u, 0x40)) {
        if (++*cnt > 0x13) {
            *cnt = 0;
            RECF = 3;
            REQ |= 0x20;
        }
    } else {
        *cnt = 0;
    }
}

/* bit6: charge FET off (FETB bit1 clear) and current ≥ 500 sustained -> code 1 */
void alarm_scan_b6(void)
{
    volatile uint16_t *cnt = (volatile uint16_t *)0x200004BC;
    if ((FETB & 2) == 2) {
        *cnt = 0;
    } else if (*(volatile uint32_t *)0x200003A8 < 500) {
        *cnt = 0;
    } else if (++*cnt > 0x3B) {
        RECF = 1;
        REQ |= 0x40;
    }
}

/* bit7: discharge FET off (FETB bit0 clear) and current ≥ 500 sustained -> code 2 */
void alarm_scan_b7(void)
{
    volatile uint16_t *cnt = (volatile uint16_t *)0x200005AE;
    if ((FETB & 1) == 1) {
        *cnt = 0;
    } else if (*(volatile uint32_t *)0x20000420 < 500) {
        *cnt = 0;
    } else if (++*cnt > 0x3B) {
        RECF = 2;
        REQ |= 0x80;
    }
}
