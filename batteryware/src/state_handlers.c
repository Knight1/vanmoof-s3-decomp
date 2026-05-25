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

/* ===== Remaining state handlers with unique patterns ===== */

/*
 * State handler 01: entry state for normal boot.
 *
 * Turns off GPIO pin 0x01, turns on charge MOSFET, turns off GPIO 0x200.
 * Selects bms_configure argument based on status register fields:
 *   - bit 3 clear → arg=1 or 3 depending on bit 12
 *   - bit 11 clear → arg=0 or 2 depending on bit 12
 *   - otherwise → arg=1 or 3 depending on bit 12
 */
void state_handler_01(void)
{
    volatile uint32_t * const s_status = (volatile uint32_t *)0x20002C00;
    uint8_t cfg;

    gpio_bit_write(0x50000400, 1, 0);
    charge_mosfet_on();
    gpio_bit_write(0x50000400, 0x200, 0);

    if (((*s_status >> 3) & 1) == 0) {
        *s_status |= 0x800;
        if (((*s_status >> 12) & 1) == 0) {
            cfg = 1;
        } else {
            cfg = 3;
        }
    } else if (((*s_status >> 11) & 1) == 0) {
        if (((*s_status >> 12) & 1) == 0) {
            cfg = 0;
        } else {
            cfg = 2;
        }
    } else if (((*s_status >> 12) & 1) == 0) {
        cfg = 1;
    } else {
        cfg = 3;
    }

    bms_configure(cfg);
    bms_set_state(1);
}

/*
 * State handler 03 init: initialization for charge state.
 *
 * Clears multiple GPIO pins (1, 0x200), masks out bit 6, bit 7,
 * and bit 8 from the status register, zeroes two SRAM counters,
 * sets a byte to 0xFF, then ORs 0x800 into status.
 * Reads g_fault_flags to determine BMS config argument:
 *   - if bits 0 and 1 are clear AND bit 12 == 0: arg = 1
 *   - if bits 0 and 1 are clear AND bit 12 == 1: arg = 3
 *   - otherwise: arg = 1
 * Turns off charge MOSFET, transitions to state 3.
 */
void state_handler_03_init(void)
{
    volatile uint32_t * const s_status   = (volatile uint32_t *)0x20002C00;
    volatile uint32_t * const s_counter1 = (volatile uint32_t *)0x20002C46;
    volatile uint32_t * const s_counter2 = (volatile uint32_t *)0x20002C06;
    volatile uint8_t  * const s_cfg_byte = (volatile uint8_t  *)0x20002BFE;
    uint8_t cfg;

    gpio_bit_write(0x50000400, 1, 0);
    gpio_bit_write(0x50000400, 0x200, 0);
    *s_status &= ~0x40U;
    *s_status &= 0xFFFFFEFF;
    *s_status &= ~0x80U;
    *s_counter1 = 0;
    *s_counter2 = 0;
    *s_cfg_byte = 0xFF;
    *s_status |= 0x800;

    if (((*g_fault_flags & 1) == 0) && (((*g_fault_flags & 3) >> 1) == 0)) {
        if (((*s_status >> 12) & 1) == 0) {
            cfg = 1;
        } else {
            cfg = 3;
        }
    } else {
        cfg = 1;
    }

    bms_configure(cfg);
    charge_mosfet_off();
    bms_set_state(3);
}

/*
 * State handler 17/18/19: power-on OVP/UVP protection dispatch.
 *
 * If pre-discharge bit 11 is clear AND (bit 6 or bit 7 is set) AND
 * bit 15 is clear: turns on GPIO pin 0x80 (OVP/UVP override).
 * Clears bit 4 in status register.
 * Turns on GPIO 1, turns off charge MOSFET, turns off GPIO 0x200.
 * Masks out bit 11 (0x800) from status.
 * Calls bms_configure(0), then dispatches:
 *   - bit 11 clear AND bits 6+7 clear → state 0x18
 *   - bit 11 clear AND (bit 6 or bit 7 set) → state 0x17
 *   - bit 11 set → state 0x19
 */
void state_handler_17_19(void)
{
    volatile uint32_t * const s_status    = (volatile uint32_t *)0x20002C00;
    volatile uint32_t * const s_precharge = (volatile uint32_t *)0x2000286C;

    if (((*s_precharge >> 11) & 1) == 0) {
        if ((((*g_fault_flags >> 6) & 1) != 0) || (((*g_fault_flags >> 7) & 1) != 0)) {
            if (((*s_status >> 15) & 1) == 0) {
                gpio_bit_write(0x50000400, 0x80, 1);
            }
        }
    }

    *s_status &= ~0x10U;
    gpio_bit_write(0x50000400, 1, 1);
    charge_mosfet_off();
    gpio_bit_write(0x50000400, 0x200, 0);
    *s_status &= 0xFFFFF7FF;
    bms_configure(0);

    if (((*s_precharge >> 11) & 1) == 0) {
        if ((((*g_fault_flags >> 6) & 1) == 0) && (((*g_fault_flags >> 7) & 1) == 0)) {
            bms_set_state(0x18);
        } else {
            bms_set_state(0x17);
        }
    } else {
        bms_set_state(0x19);
    }
}

/*
 * State flags housekeeping handler.
 *
 * Processes a flags byte at 0x20002C80:
 *   - bit 1 set: clears it (handled event)
 *   - bit 3 set: clears it, writes magic 0xAAAA to register, optionally
 *     calls coulomb counter (FUN_08013228) with value from 0x200028C0
 *   - bit 4 set: clears it
 *   - bit 5 set: clears it, increments three counters, and if the
 *     event counter at 0x200025A0 exceeds 0x27, triggers a system event
 *     via veneer_155ec (→ FUN_0800e92d, a subsystem reset/event hook)
 *
 * The 'arg' parameter controls whether the coulomb counter is invoked
 * (only when arg != 0).
 */
void state_flags_handler(uint8_t arg)
{
    volatile uint32_t * const s_flags      = (volatile uint32_t *)0x20002C80;
    volatile uint32_t * const s_magic_reg  = (volatile uint32_t *)0x20002C10;
    volatile uint32_t * const s_charge_in  = (volatile uint32_t *)0x200028C0;
    volatile uint32_t * const s_timer      = (volatile uint32_t *)0x20002C74;
    volatile uint32_t * const s_timer2     = (volatile uint32_t *)0x20002AC8;
    volatile uint16_t * const s_evt_count  = (volatile uint16_t *)0x200025A0;

    if (((*s_flags >> 1) & 1) != 0) {
        *s_flags &= ~2U;
    }

    if (((*s_flags >> 3) & 1) != 0) {
        *s_flags &= ~8U;
        *s_magic_reg = 0xAAAA;
        if (arg != 0) {
            extern void coulomb_counter(uint32_t val);
            coulomb_counter(*s_charge_in);
        }
    }

    if (((*s_flags >> 4) & 1) != 0) {
        *s_flags &= ~0x10U;
    }

    if (((*s_flags >> 5) & 1) != 0) {
        *s_flags &= ~0x20U;
        *s_timer += 1;
        *s_timer2 += 1;
        uint16_t cnt = *s_evt_count + 1;
        *s_evt_count = cnt;
        if (cnt > 0x27) {
            extern void sys_event_hook(void);
            sys_event_hook();
        }
    }
}

/*
 * State flags handler (timer-driven context).
 *
 * Similar to state_flags_handler but driven from a timer ISR.
 * Processes flags at 0x20002C54:
 *   - bit 0: clears it, calls FUN_08000880 (subsystem restart)
 *   - bit 1: clears it
 *   - bit 2: clears it, calls fg_watchdog_kick + veneers (response send chain)
 *   - bit 3: clears it, writes magic 0xAAAA
 *   - bit 4: clears it
 *   - bit 5: clears it, increments two counters. If timeout > 9 and
 *     condition bits set and threshold met: enters shipping mode via
 *     memcmp_verify → button_entry_check → infinite loop.
 */
void state_flags_handler_timer(void)
{
    volatile uint32_t * const s_flags  = (volatile uint32_t *)0x20002C54;
    volatile uint32_t * const s_magic  = (volatile uint32_t *)0x20002C58;
    volatile uint32_t * const s_timer  = (volatile uint32_t *)0x20002C74;
    volatile uint32_t * const s_timer2 = (volatile uint32_t *)0x20002C5C;

    extern void subsys_restart(void);   /* FUN_08000880 */
    extern void resp_send_chain(void);  /* veneer_11f68 */
    extern void resp_send_chain2(void); /* veneer_11f88 */
    extern void resp_send_chain3(void); /* veneer_11f18 */

    extern void flags_scan(void);       /* FUN_0800325c */
    flags_scan();

    if ((*s_flags & 1) != 0) {
        *s_flags &= ~1U;
        subsys_restart();
    }
    if (((*s_flags >> 1) & 1) != 0) {
        *s_flags &= ~2U;
    }
    if (((*s_flags >> 2) & 1) != 0) {
        *s_flags &= ~4U;
        resp_send_chain();
        resp_send_chain2();
        resp_send_chain3();
        fg_watchdog_kick();
    }
    if (((*s_flags >> 3) & 1) != 0) {
        *s_flags &= ~8U;
        *s_magic = 0xAAAA;
    }
    if (((*s_flags >> 4) & 1) != 0) {
        *s_flags &= ~0x10U;
    }
    if (((*s_flags >> 5) & 1) != 0) {
        *s_flags &= ~0x20U;
        *s_timer += 1;
        *s_timer2 += 1;

        volatile uint32_t * const s_thresh = (volatile uint32_t *)0x20002C60;
        volatile uint32_t * const s_cmp1   = (volatile uint32_t *)0x20002C64;

        if ((*s_timer2 > 9) && (((*s_cmp1 >> 3) & 1) != 0) && (*s_thresh <= *(volatile uint32_t *)0x20002C68)) {
            extern void flash_block_update(void);
            flash_block_update();
            if (button_entry_check()) {
                gpio_bit_write(0x50000400, 0x1000, 0);
                while (1) { }  /* bootloader mode */
            }
            *s_timer2 = 0;
        }
    }
}
