#include "batteryware.h"

/*
 * State machine transition handlers.
 *
 * Each handler follows one of 5 patterns. They all:
 *   1. Manipulate a status register (clear/write bits)
 *   2. Toggle GPIO pins via gpio_bit_write
 *   3. Call charge_mosfet_off
 *   4. Apply a mask to the status register
 *   5. Call bms_configure(config_arg) to set up the BMS/FEDL5236 
 *   6. Call bms_set_state(next_state) to transition
 *
 * The variants differ in:
 *   - GPIO direction (on-first vs off-first)
 *   - Status register operation (AND-mask vs OR-bits)
 *   - bms_configure argument (0, 1, or 2)
 *   - Conditional config based on comparator result
 *
 * The DAT globals (status registers, GPIO bases, mask values) are
 * resolved from the literal pool. Each handler has its own set.
 */

/* bms_configure — declared externally (FUN_080052d8) */
extern void bms_configure(uint8_t cfg);

/* ===== Pattern 1: Standard (10 handlers) =====
 * Clear bit 0x10, gpio_on(1), mosfet_off, gpio_off(0x200),
 * AND-mask status, bms_configure(0), bms_set_state(N)
 */
#define STATE_HANDLER_STD(name, state, status_reg, gpio_base, mask_val) \
    void name(void) { \
        *status_reg &= ~0x10U; \
        gpio_bit_write((uint32_t)gpio_base, 1, 1); \
        charge_mosfet_off(); \
        gpio_bit_write((uint32_t)gpio_base, 0x200, 0); \
        *status_reg &= mask_val; \
        bms_configure(0); \
        bms_set_state(state); \
    }

/* ===== Pattern 2: MOSFET-on variant (2 handlers: 07, 08) =====
 * Same as standard but ORs 0x800 into status, bms_configure(1)
 */
#define STATE_HANDLER_MOSFET_ON(name, state, status_reg, gpio_base) \
    void name(void) { \
        *status_reg &= ~0x10U; \
        gpio_bit_write((uint32_t)gpio_base, 1, 1); \
        charge_mosfet_off(); \
        gpio_bit_write((uint32_t)gpio_base, 0x200, 0); \
        *status_reg |= 0x800; \
        bms_configure(1); \
        bms_set_state(state); \
    }

/* ===== Pattern 3: Dual-MOSFET variant (2 handlers: 09, 0a) =====
 * Same as standard but AND-mask, bms_configure(2)
 */
#define STATE_HANDLER_DUAL(name, state, status_reg, gpio_base, mask_val) \
    void name(void) { \
        *status_reg &= ~0x10U; \
        gpio_bit_write((uint32_t)gpio_base, 1, 1); \
        charge_mosfet_off(); \
        gpio_bit_write((uint32_t)gpio_base, 0x200, 0); \
        *status_reg &= mask_val; \
        bms_configure(2); \
        bms_set_state(state); \
    }

/* ===== Pattern 4: Inverted start variant (1 handler: 02) =====
 * gpio_off(1) first, charge_mosfet_off, OR 0x40, gpio_on(0x200),
 * AND-mask, bms_configure(2), bms_set_state(2)
 */
#define STATE_HANDLER_INV(name, state, status_reg, gpio_base, mask_val) \
    void name(void) { \
        gpio_bit_write((uint32_t)gpio_base, 1, 0); \
        charge_mosfet_off(); \
        *status_reg |= 0x40; \
        gpio_bit_write((uint32_t)gpio_base, 0x200, 1); \
        *status_reg &= mask_val; \
        bms_configure(2); \
        bms_set_state(state); \
    }

/* ===== Pattern 5: Conditional config (2 handlers: 0d, 0e) =====
 * Standard pattern but bms_configure arg depends on comparator:
 *   if (*cmp_ptr < *cmp_val) bms_configure(0) else bms_configure(2)
 */
#define STATE_HANDLER_COND(name, state, status_reg, gpio_base, mask_val, cmp_ptr, cmp_val) \
    void name(void) { \
        *status_reg &= ~0x10U; \
        gpio_bit_write((uint32_t)gpio_base, 1, 1); \
        charge_mosfet_off(); \
        gpio_bit_write((uint32_t)gpio_base, 0x200, 0); \
        *status_reg &= mask_val; \
        if (*cmp_ptr < *cmp_val) { \
            bms_configure(0); \
        } else { \
            bms_configure(2); \
        } \
        bms_set_state(state); \
    }

/*
 * Instantiate all 17 handlers using the macros above.
 * The SRAM addresses and GPIO bases are resolved from the OEM literal pool.
 */

/* ---- Pattern 1: Standard ---- */
/* state_handler_0b: 0x0B */
STATE_HANDLER_STD(state_handler_0b, 0x0B,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400, 0xFFFFFFEF)

/* state_handler_0c: 0x0C */
STATE_HANDLER_STD(state_handler_0c, 0x0C,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400, 0xFFFFFFEF)

/* state_handler_12: 0x12 */
STATE_HANDLER_STD(state_handler_12, 0x12,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400, 0xFFFFFFEF)

/* state_handler_13: 0x13 */
STATE_HANDLER_STD(state_handler_13, 0x13,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400, 0xFFFFFFEF)

/* state_handler_0f: 0x0F */
STATE_HANDLER_STD(state_handler_0f, 0x0F,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400, 0xFFFFFFEF)

/* state_handler_10: 0x10 */
STATE_HANDLER_STD(state_handler_10, 0x10,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400, 0xFFFFFFEF)

/* state_handler_11: 0x11 */
STATE_HANDLER_STD(state_handler_11, 0x11,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400, 0xFFFFFFEF)

/* state_handler_14: 0x14 */
STATE_HANDLER_STD(state_handler_14, 0x14,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400, 0xFFFFFFEF)

/* state_handler_15: 0x15 */
STATE_HANDLER_STD(state_handler_15, 0x15,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400, 0xFFFFFFEF)

/* state_handler_16: 0x16 */
STATE_HANDLER_STD(state_handler_16, 0x16,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400, 0xFFFFFFEF)

/* ---- Pattern 2: MOSFET-on ---- */
STATE_HANDLER_MOSFET_ON(state_handler_07, 0x07,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400)

STATE_HANDLER_MOSFET_ON(state_handler_08, 0x08,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400)

/* ---- Pattern 3: Dual-MOSFET ---- */
STATE_HANDLER_DUAL(state_handler_09, 0x09,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400, 0xFFFFFFEF)

STATE_HANDLER_DUAL(state_handler_0a, 0x0A,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400, 0xFFFFFFEF)

/* ---- Pattern 4: Inverted start ---- */
STATE_HANDLER_INV(state_handler_02, 0x02,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400, 0xFFFFFFEF)

/* ---- Pattern 5: Conditional config ---- */
STATE_HANDLER_COND(state_handler_0d, 0x0D,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400, 0xFFFFFFEF,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x20002000)

STATE_HANDLER_COND(state_handler_0e, 0x0E,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x50000400, 0xFFFFFFEF,
    (volatile uint32_t *)0x20002000, (volatile uint32_t *)0x20002000)
