#include "batteryware.h"

/*
 * ===== Shared BMS context (SRAM) =====
 *
 * The OEM firmware keeps the BMS state machine context in a small set
 * of SRAM cells. Every state_timer_* handler dispatches off the same
 * cells; a few also touch the over-current monitoring cells (cur,
 * oc_tmr). Addresses are resolved from the OEM literal pool (see the
 * comment above each timer for the pool offset within its function).
 *
 * Naming note: two of these were mislabelled in an earlier pass.
 *   - 0x20002C44 is the central protection/fault-flags register
 *     (declared `g_fault_flags` in fuel_gauge.c / batteryware.h). It
 *     was called `s_bms_aux`; renamed `s_fault_flags`. The OEM reads it
 *     as a halfword in these handlers (ldrh), hence uint16_t.
 *   - 0x2000286C is a *separate* protection-status word (zeroed by
 *     bms_init; read for the OVP/UVP dispatch bits) — NOT the central
 *     fault register. It was called `s_bms_fault`; renamed
 *     `s_prot_status`.
 *
 * The thresholds 0x4E1F (19999) and 0x4A37 (18999) live in the literal
 * pool as immediates, not pointers.
 */

/* Core dispatch cells — used by every state_timer_* */
static volatile uint8_t  * const s_bms_state   = (volatile uint8_t  *)0x2000289C;
static volatile uint8_t  * const s_bms_flags   = (volatile uint8_t  *)0x20002C80;
static volatile uint32_t * const s_bms_cfg     = (volatile uint32_t *)0x20002C00; /* central status/control reg; aka `s_status` in the unique handlers */
static volatile uint16_t * const s_prot_status = (volatile uint16_t *)0x2000286C; /* OVP/UVP protection-status bits */
static volatile uint16_t * const s_fault_flags = (volatile uint16_t *)0x20002C44; /* central fault register (== g_fault_flags), low half */

/* Over-current monitoring — used by 0c/0d/05/charge_a/charge_b */
static volatile uint32_t * const s_bms_cur     = (volatile uint32_t *)0x200028C8;
static volatile uint16_t * const s_bms_oc_tmr  = (volatile uint16_t *)0x20002C0C;

/* Immediate thresholds (literal-pool constants, NOT pointers) */
#define BMS_OC_DISCHARGE_THRESHOLD 19999u   /* 0x4E1F */
#define BMS_OC_CHARGE_THRESHOLD    18999u   /* 0x4A37 */

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
 * Standard pattern but the bms_configure arg depends on an over-current
 * comparison against a constant threshold (OEM: cmp r3,thr; bls →cfg2):
 *   if (*cmp_ptr > threshold) bms_configure(0) else bms_configure(2)
 */
#define STATE_HANDLER_COND(name, state, status_reg, gpio_base, mask_val, cmp_ptr, threshold) \
    void name(void) { \
        *status_reg &= ~0x10U; \
        gpio_bit_write((uint32_t)gpio_base, 1, 1); \
        charge_mosfet_off(); \
        gpio_bit_write((uint32_t)gpio_base, 0x200, 0); \
        *status_reg &= mask_val; \
        if (*cmp_ptr > (threshold)) { \
            bms_configure(0); \
        } else { \
            bms_configure(2); \
        } \
        bms_set_state(state); \
    }

/*
 * Instantiate the 17 macro handlers. Constants are taken from each
 * handler's own OEM literal pool, verified to be identical across the
 * set: status register 0x20002C00 (= s_bms_cfg), GPIO base 0x50000400,
 * and AND-mask 0xFFFFF7FF (clear bit 11). The COND handlers compare the
 * over-current cell s_bms_cur (0x200028C8) against 19999.
 *
 * (An earlier pass used 0x20002000 / mask 0xFFFFFFEF and a self-
 * comparison in the COND handlers — all three were wrong.)
 */
#define BMS_GPIO ((volatile uint32_t *)0x50000400)
#define BMS_HANDLER_MASK 0xFFFFF7FFu   /* clear bit 11 */

/* ---- Pattern 1: Standard ---- */
STATE_HANDLER_STD(state_handler_0b, 0x0B, s_bms_cfg, BMS_GPIO, BMS_HANDLER_MASK)
STATE_HANDLER_STD(state_handler_0c, 0x0C, s_bms_cfg, BMS_GPIO, BMS_HANDLER_MASK)
STATE_HANDLER_STD(state_handler_12, 0x12, s_bms_cfg, BMS_GPIO, BMS_HANDLER_MASK)
STATE_HANDLER_STD(state_handler_13, 0x13, s_bms_cfg, BMS_GPIO, BMS_HANDLER_MASK)
STATE_HANDLER_STD(state_handler_0f, 0x0F, s_bms_cfg, BMS_GPIO, BMS_HANDLER_MASK)
STATE_HANDLER_STD(state_handler_10, 0x10, s_bms_cfg, BMS_GPIO, BMS_HANDLER_MASK)
STATE_HANDLER_STD(state_handler_11, 0x11, s_bms_cfg, BMS_GPIO, BMS_HANDLER_MASK)
STATE_HANDLER_STD(state_handler_14, 0x14, s_bms_cfg, BMS_GPIO, BMS_HANDLER_MASK)
STATE_HANDLER_STD(state_handler_15, 0x15, s_bms_cfg, BMS_GPIO, BMS_HANDLER_MASK)
STATE_HANDLER_STD(state_handler_16, 0x16, s_bms_cfg, BMS_GPIO, BMS_HANDLER_MASK)

/* ---- Pattern 2: MOSFET-on (OR 0x800, config 1) ---- */
STATE_HANDLER_MOSFET_ON(state_handler_07, 0x07, s_bms_cfg, BMS_GPIO)
STATE_HANDLER_MOSFET_ON(state_handler_08, 0x08, s_bms_cfg, BMS_GPIO)

/* ---- Pattern 3: Dual-MOSFET (config 2) ---- */
STATE_HANDLER_DUAL(state_handler_09, 0x09, s_bms_cfg, BMS_GPIO, BMS_HANDLER_MASK)
STATE_HANDLER_DUAL(state_handler_0a, 0x0A, s_bms_cfg, BMS_GPIO, BMS_HANDLER_MASK)

/* ---- Pattern 4: Inverted start (OR 0x40, config 2) ---- */
STATE_HANDLER_INV(state_handler_02, 0x02, s_bms_cfg, BMS_GPIO, BMS_HANDLER_MASK)

/* ---- Pattern 5: Conditional config (over-current threshold) ---- */
STATE_HANDLER_COND(state_handler_0d, 0x0D, s_bms_cfg, BMS_GPIO, BMS_HANDLER_MASK,
    s_bms_cur, BMS_OC_DISCHARGE_THRESHOLD)
STATE_HANDLER_COND(state_handler_0e, 0x0E, s_bms_cfg, BMS_GPIO, BMS_HANDLER_MASK,
    s_bms_cur, BMS_OC_DISCHARGE_THRESHOLD)

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
    volatile uint16_t * const s_counter1 = (volatile uint16_t *)0x20002C46;
    volatile uint16_t * const s_counter2 = (volatile uint16_t *)0x20002C06;
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

    /* Clear fault-register bits 8 and 9 (halfword ops on 0x20002C44). */
    *s_fault_flags &= 0xFEFFU;
    *s_fault_flags &= 0xFDFFU;

    if (((*s_fault_flags & 1) == 0) && (((*s_fault_flags >> 1) & 1) == 0)) {
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
 * **Secondary-protection fuse trigger.** When the protection is *not* the
 * bit-11 class (s_prot_status bit 11 clear) AND a hard over-current is latched
 * (g_fault_flags bit 6 FAULT_DISCHARGE_OC or bit 7 FAULT_CHARGE_OC) AND the
 * update-busy flag (s_bms_cfg bit 15) is clear, it drives **GPIOB PB7
 * (0x80) HIGH** — energizing the heater element of the on-board secondary
 * fuse. This is a one-shot, last-resort permanent pack disconnect: no code
 * path ever drives PB7 low again (it is only cleared once, in the boot GPIO
 * init), so once the AFE's FETs can't break the over-current ("MOS Failure"),
 * the heater melts the fuse and severs the pack for good. See docs/hardware.md.
 *
 * It then forces everything off (clear s_bms_cfg bit 4; PB1 high; charge MOSFET
 * off via charge_mosfet_off + PB9 low; clear bit 11) and calls bms_configure(0),
 * then dispatches the resulting protection state:
 *   - bit 11 clear AND bits 6+7 clear → state 0x18
 *   - bit 11 clear AND (bit 6 or bit 7 set) → state 0x17
 *   - bit 11 set → state 0x19
 */
void state_handler_17_19(void)
{
    volatile uint32_t * const s_status = (volatile uint32_t *)0x20002C00;

    if (((*s_prot_status >> 11) & 1) == 0) {
        if ((((*s_fault_flags >> 6) & 1) != 0) || (((*s_fault_flags >> 7) & 1) != 0)) {
            if (((*s_status >> 15) & 1) == 0) {
                /* PB7 HIGH — fire the secondary-protection fuse heater (one-shot,
                 * never cleared at runtime). Permanent pack disconnect. */
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

    if (((*s_prot_status >> 11) & 1) == 0) {
        if ((((*s_fault_flags >> 6) & 1) == 0) && (((*s_fault_flags >> 7) & 1) == 0)) {
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
    volatile uint8_t  * const s_flags      = (volatile uint8_t  *)0x20002C80; /* uint8 dispatch byte (ldrb/strb) */
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
    /* Same dispatch flags byte as state_flags_handler: 0x20002C80 (ldrb). */
    volatile uint8_t  * const s_flags  = (volatile uint8_t  *)0x20002C80;
    volatile uint32_t * const s_magic  = (volatile uint32_t *)0x20002C10;
    volatile uint32_t * const s_timer  = (volatile uint32_t *)0x20002C74;
    volatile uint32_t * const s_timer2 = (volatile uint32_t *)0x20002AC8;

    extern void cell_balance_update(void); /* FUN_08000880 (bit 0) */
    extern void fg_scan(void);             /* FUN_0800325c */
    extern void memcmp_verify(char *actual, uint32_t len, char *expected);

    fg_scan();

    if ((*s_flags & 1) != 0) {
        *s_flags &= ~1U;
        cell_balance_update();
    }
    if (((*s_flags >> 1) & 1) != 0) {
        *s_flags &= ~2U;
    }
    if (((*s_flags >> 2) & 1) != 0) {
        *s_flags &= ~4U;
        veneer_11f68();
        veneer_11f88();
        veneer_11f18();
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

        /* Shipping/EEPROM-snapshot gate: timer2 > 9, status bit 12 set,
         * and discharge current within the OC threshold. */
        if ((*s_timer2 > 9) &&
            (((*s_bms_cfg >> 12) & 1) != 0) &&
            (*s_bms_cur <= BMS_OC_DISCHARGE_THRESHOLD)) {
            memcmp_verify((char *)0x08080C00, 0x80, (char *)0x200029A8);
            if (button_entry_check()) {
                gpio_bit_write(0x50000400, 0x1000, 0);
                while (1) { }  /* bootloader mode */
            }
            *s_timer2 = 0;
        }
    }
}

/* ---- State timer handlers (periodic ISR-driven dispatch) ----
 * Each is registered as a periodic callback for a specific BMS state.
 * Pattern: fg_scan() → check flags → dispatch state_handler → fault check → response chain → watchdog
 */

void state_timer_0b(void)
{
    extern void fg_scan(void);
    fg_scan();

    /* Literal pool @ 0x080011C4..0x080011D4: s_bms_state, s_bms_flags,
     * s_bms_cfg, s_prot_status, s_fault_flags (all in the shared BMS context). */

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) != 0)        { state_handler_07(); return; }
            if (((*s_prot_status >> 5) & 1) == 0) { state_handler_01(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        extern void cell_balance_update(void);
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check();   /* state 0b: no charge-OC check */
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    veneer_11f08(0);
}

void state_timer_0d(void)
{
    extern void fg_scan(void);
    fg_scan();

    /* Literal pool @ 0x08001D20..0x08001D3C: 5 shared BMS cells +
     * s_bms_cur, the immediate threshold 0x4E1F, and s_bms_oc_tmr. */

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) != 0)        { state_handler_07(); return; }
            if (((*s_prot_status >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_prot_status >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_prot_status >> 7) & 1) == 0) { state_handler_01(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        extern void cell_balance_update(void);
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check(); fg_charge_oc_check();
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        if (BMS_OC_DISCHARGE_THRESHOLD < *s_bms_cur) {
            uint16_t cnt = (uint16_t)(*s_bms_oc_tmr + 1);
            *s_bms_oc_tmr = cnt;
            if (cnt > 9) { *s_bms_oc_tmr = 0; state_handler_02(); return; }
        } else {
            *s_bms_oc_tmr = 0;
        }
        extern void veneer_11f68(void);
        extern void veneer_11f88(void);
        extern void veneer_11f18(void);
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    extern void veneer_11f08(int arg);
    veneer_11f08(0);
}

void state_timer_0e(void)
{
    extern void fg_scan(void);
    fg_scan();

    /* Literal pool @ 0x08001F30..0x08001F4C: 5 shared BMS cells + 3 pointers.
     * Cell-stats struct base @ 0x20002588 (s_cell, byte[]) and the bigger
     * struct @ 0x200028D0 (s_cellv, byte[]); charge timer @ 0x20002A94. */
    volatile uint8_t  *s_cell  = (volatile uint8_t  *)0x20002588;
    volatile uint8_t  *s_cellv = (volatile uint8_t  *)0x200028D0;
    volatile uint16_t *s_tmr   = (volatile uint16_t *)0x20002A94;

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) != 0)        { state_handler_07(); return; }
            if (((*s_prot_status >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_prot_status >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        extern void cell_balance_update(void);
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check(); fg_charge_oc_check();
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        if ((s_cellv[0x82] < s_cell[1]) || (s_cellv[0x82] < s_cell[2])) {
            *s_tmr = 0;
        } else {
            uint16_t cnt = (uint16_t)(*s_tmr + 1);
            *s_tmr = cnt;
            if (((*(volatile uint16_t *)(s_cellv + 0x84) / 100) & 0xFFFF) <= cnt) {
                state_handler_01(); return;
            }
        }
        extern void veneer_11f68(void);
        extern void veneer_11f88(void);
        extern void veneer_11f18(void);
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    extern void veneer_11f08(int arg);
    veneer_11f08(0);
}

void state_timer_14(void)
{
    extern void fg_scan(void);
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11f18(void);
    extern void veneer_11f08(int arg);
    fg_scan();

    /* Literal pool @ 0x08002120..0x0800213C: 5 shared BMS cells + cell-stats
     * struct base @ 0x20002588, cellv struct base @ 0x200028D0, tmr @ 0x20002A52. */
    volatile uint8_t  *s_cell  = (volatile uint8_t  *)0x20002588;
    volatile uint8_t  *s_cellv = (volatile uint8_t  *)0x200028D0;
    volatile uint16_t *s_tmr   = (volatile uint16_t *)0x20002A52;

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) != 0)        { state_handler_07(); return; }
            if (((*s_prot_status >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_prot_status >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check(); fg_charge_oc_check();
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        if ((s_cellv[0x8A] <= s_cell[1]) && (s_cellv[0x8A] <= s_cell[2])) {
            uint16_t cnt = (uint16_t)(*s_tmr + 1);
            *s_tmr = cnt;
            if (((*(volatile uint16_t *)(s_cellv + 0x8C) / 100) & 0xFFFF) <= cnt) {
                state_handler_01(); return;
            }
        }
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    veneer_11f08(0);
}

/*
 * BMS state machine dispatch — central state transition engine.
 *
 * Called whenever the BMS state needs to change. Zeroes all state
 * counters (20 SRAM words), reads FEDL5236 register 10 (status),
 * clears bit 9 errors, and dispatches the new state handler:
 *
 *   state < 0x1A (0–25):
 *     Calls handler from jump table at 0x080175E4 via
 *     indirect call through (*table[state])().
 *
 *   state > 6 AND state-7 < 0x13:
 *     Calls handler from secondary jump table at 0x0801764C.
 *
 *   state > 6 AND state < 0x1A (other handlers):
 *     Builds a 0x38-byte telemetry packet with:
 *       - FEDL5236 status bytes
 *       - Charge/discharge current
 *       - Cell voltages (10 × uint16 from 0x20002880)
 *       - State-of-charge percentage
 *       - Charge/discharge status flags
 *       - Sequence counter (rolling, capped at 64999)
 *     Stores the packet via memcmp_verify to EEPROM/SPI
 *     (ring buffer at 0x08080200 or 0x08080E00 depending on position).
 *
 * After dispatch: clears bit 15 in status register, calls fg_clear_status,
 * and prints an I²C message.
 */
void bms_set_state(uint8_t state)
{
    /* Telemetry/EEPROM context block @ 0x200029A8 — field offsets below are
     * byte offsets from this base. `s_state` (0x20002B58) is the live BMS
     * state byte; `s_prev` records the previous state. */
    volatile uint8_t * const ctx        = (volatile uint8_t *)0x200029A8;
    volatile uint8_t * const s_state    = (volatile uint8_t *)0x20002B58;
    volatile uint8_t * const s_prev     = (volatile uint8_t *)0x2000299C;
    volatile uint8_t * const s_state_cp = (volatile uint8_t *)0x20002C48;

    *s_prev  = *s_state;
    *s_state = state;

    /* Zero the 20 per-transition counters (note the mixed access widths —
     * several targets are not 4-byte aligned and are written as halfwords or
     * bytes; a word write there would fault on Cortex-M0+). */
    *(volatile uint32_t *)0x20002AC8 = 0;
    *(volatile uint16_t *)0x20002C44 = 0;
    *(volatile uint16_t *)0x20002A80 = 0;
    *(volatile uint16_t *)0x20002A94 = 0;
    *(volatile uint16_t *)0x20002A8C = 0;
    *(volatile uint16_t *)0x20002A52 = 0;
    *(volatile uint16_t *)0x20002ABA = 0;
    *(volatile uint16_t *)0x20002B08 = 0;
    *(volatile uint16_t *)0x20002A44 = 0;
    *(volatile uint16_t *)0x20002A4E = 0;
    *(volatile uint16_t *)0x20002B5C = 0;
    *(volatile uint16_t *)0x20002ACE = 0;
    *(volatile uint16_t *)0x20002A2E = 0;
    *(volatile uint16_t *)0x20002B5A = 0;
    *(volatile uint16_t *)0x20002AC4 = 0;
    *(volatile uint16_t *)0x200029A4 = 0;
    *(volatile uint8_t  *)0x2000287B = 0;
    *(volatile uint8_t  *)0x20002BFC = 0;
    *(volatile uint16_t *)0x20002C70 = 0;
    *(volatile uint16_t *)0x20002C04 = 0;

    *s_state_cp = *s_state;

    /* Read FEDL5236 status register (reg 10, 2 bytes); on success copy the
     * two status bytes into the 0x20002820 status word. */
    if (smbus_read(10, 2) != 0) {
        volatile uint8_t * const rx = (volatile uint8_t *)0x20002B84;
        *(volatile uint8_t *)0x20002820       = rx[2];
        *(volatile uint8_t *)(0x20002820 + 1) = rx[3];
    }

    /* Bit 9 set => clear it, reset both fuel-gauge alert masks. */
    if (((*s_bms_cfg >> 9) & 1) != 0) {
        *s_bms_cfg &= 0xFFFFFDFFu;
        smbus_write_reg(10, 0, 0xFF);
        smbus_write_reg(0x0B, 0, 0xFF);
        *(volatile uint8_t *)0x20002590 = 0;
        *(volatile uint8_t *)0x2000287C = 0;
    }

    /* Dispatch only while bit 15 ("update busy") is clear. */
    if (((*s_bms_cfg >> 15) & 1) == 0) {
        if (*s_state != 6) {
            memcmp_verify((char *)0x08080001, 1, (char *)s_state_cp);
        }

        /* Primary per-state entry action (jump table @ 0x080175E4). Each case
         * bumps a per-state counter and persists a few bytes to EEPROM. */
        if (state <= 0x19) {
            switch (state) {
            case 1:
                *s_prot_status &= 0xFEFFu;
                *s_prot_status &= 0xFDFFu;
                break;
            case 2:
                *(volatile uint16_t *)0x2000258C = 0;
                break;
            case 3:
                *(volatile uint8_t *)0x20002A84 = 0;
                break;
            case 7:
                *(volatile uint16_t *)(ctx + 0x14) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x14) + 1);
                memcmp_verify((char *)0x08080C14, 2, (char *)0x20002BBC);
                break;
            case 8:
                *(volatile uint16_t *)(ctx + 0x16) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x16) + 1);
                memcmp_verify((char *)0x08080C16, 2, (char *)0x20002BBE);
                break;
            case 9:
                *(volatile uint8_t  *)(ctx + 0x36) = 0;
                *(volatile uint32_t *)(ctx + 0x24) = 0;
                *(volatile uint16_t *)(ctx + 0x18) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x18) + 1);
                memcmp_verify((char *)0x08080C18, 2, (char *)0x20002BC0);
                break;
            case 10:
                *(volatile uint8_t  *)(ctx + 0x36) = 0;
                *(volatile uint32_t *)(ctx + 0x24) = 0;
                *(volatile uint16_t *)(ctx + 0x1A) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x1A) + 1);
                memcmp_verify((char *)0x08080C1A, 2, (char *)0x20002BC2);
                break;
            case 11:
                *(volatile uint8_t *)0x20002A84 = (uint8_t)(*(volatile uint8_t *)0x20002A84 + 1);
                *(volatile uint16_t *)(ctx + 0x10) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x10) + 1);
                memcmp_verify((char *)0x08080C10, 2, (char *)0x20002BB8);
                break;
            case 12:
                *(volatile uint8_t *)0x20002A84 = (uint8_t)(*(volatile uint8_t *)0x20002A84 + 1);
                *(volatile uint16_t *)(ctx + 0x12) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x12) + 1);
                memcmp_verify((char *)0x08080C12, 2, (char *)0x20002BBA);
                break;
            case 13:
                *(volatile uint16_t *)(ctx + 0x0C) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x0C) + 1);
                memcmp_verify((char *)0x08080C0C, 2, (char *)0x20002BB4);
                break;
            case 14:
                *(volatile uint16_t *)(ctx + 0x0E) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x0E) + 1);
                memcmp_verify((char *)0x08080C0E, 2, (char *)0x20002BB6);
                break;
            case 15:
                *s_prot_status &= 0xFEFFu;
                *s_prot_status &= 0xFDFFu;
                *(volatile uint8_t *)0x20002C49 = (uint8_t)(*(volatile uint8_t *)0x20002C49 + 1);
                *(volatile uint16_t *)(ctx + 0x1C) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x1C) + 1);
                memcmp_verify((char *)0x08080C1C, 2, (char *)0x20002BC4);
                break;
            case 16:
                *s_prot_status &= 0xFEFFu;
                *s_prot_status &= 0xFDFFu;
                *(volatile uint8_t *)0x20002C49 = (uint8_t)(*(volatile uint8_t *)0x20002C49 + 1);
                *(volatile uint16_t *)(ctx + 0x1E) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x1E) + 1);
                memcmp_verify((char *)0x08080C1E, 2, (char *)0x20002BC6);
                break;
            case 17:
                *(volatile uint8_t *)0x20002C73 = (uint8_t)(*(volatile uint8_t *)0x20002C73 + 1);
                *(volatile uint16_t *)(ctx + 0x22) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x22) + 1);
                memcmp_verify((char *)0x08080C22, 2, (char *)0x20002BCA);
                break;
            case 18:
                *(volatile uint16_t *)(ctx + 0x08) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x08) + 1);
                memcmp_verify((char *)0x08080C08, 2, (char *)0x20002BB0);
                break;
            case 19:
                *(volatile uint16_t *)(ctx + 0x0A) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x0A) + 1);
                memcmp_verify((char *)0x08080C0A, 2, (char *)0x20002BB2);
                break;
            case 20:
                *(volatile uint16_t *)(ctx + 0x04) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x04) + 1);
                memcmp_verify((char *)0x08080C04, 2, (char *)0x20002BAC);
                break;
            case 21:
                *(volatile uint16_t *)(ctx + 0x06) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x06) + 1);
                memcmp_verify((char *)0x08080C06, 2, (char *)0x20002BAE);
                break;
            case 22:
                *(volatile uint16_t *)(ctx + 0x20) = (uint16_t)(*(volatile uint16_t *)(ctx + 0x20) + 1);
                memcmp_verify((char *)0x08080C20, 2, (char *)(ctx + 0x20));
                break;
            case 23:
            case 24:
            case 25:
                memcmp_verify((char *)0x08080C38, 1, (char *)0x200029E0);
                break;
            default:
                /* states 0, 4, 5, 6 — no entry action */
                break;
            }
        }

        if (state > 6) {
            /* Secondary table (@ 0x0801764C) selects a per-state telemetry
             * tag word; states beyond the table use tag 0. */
            uint16_t tag;
            if ((uint8_t)(state - 7) < 0x13) {
                switch (state) {
                case 7:  tag = 0x0080; break;
                case 8:  tag = 0x0040; break;
                case 9:  tag = 0x0020; break;
                case 10: tag = 0x0010; break;
                case 11: tag = 0x0200; break;
                case 12: tag = 0x0100; break;
                case 13: tag = 0x0800; break;
                case 14: tag = 0x0400; break;
                case 15: tag = 0x0008; break;
                case 16: tag = 0x0004; break;
                case 17: tag = 0x0001; break;
                case 18: tag = 0x2000; break;
                case 19: tag = 0x1000; break;
                case 20: tag = 0x8000; break;
                case 21: tag = 0x4000; break;
                case 22: tag = 0x0002; break;
                case 23: tag = 0xFFFF; break;
                case 24: tag = 0x00C0; break;
                default: tag = 0x0030; break; /* 25 */
                }
            } else {
                tag = 0;
            }

            if (state > 6 && state <= 0x19) {
                /* Build a 0x38-byte telemetry record and persist it. */
                volatile uint8_t * const pkt   = (volatile uint8_t *)0x20002AD0;
                volatile uint8_t * const cellb = (volatile uint8_t *)0x20002588;
                volatile uint16_t * const seqp = (volatile uint16_t *)(ctx + 0x3E);

                *(volatile uint16_t *)(pkt + 0x02) = tag;
                pkt[0x2A] = cellb[1];
                pkt[0x2B] = cellb[2];
                pkt[0x2C] = cellb[0];
                *(volatile uint16_t *)(pkt + 0x1A) = *(volatile uint16_t *)0x2000281C;
                *(volatile uint32_t *)(pkt + 0x1C) = *(volatile uint32_t *)0x200028C0;
                *(volatile uint32_t *)(pkt + 0x20) = *(volatile uint32_t *)(ctx + 0x28);
                *(volatile uint32_t *)(pkt + 0x24) = *(volatile uint32_t *)(ctx + 0x2C);
                pkt[0x28] = *(volatile uint8_t *)(ctx + 0x36);
                pkt[0x29] = *(volatile uint8_t *)(ctx + 0x37);
                *(volatile uint16_t *)(pkt + 0x04) = *(volatile uint16_t *)(ctx + 0x34);
                *(volatile uint16_t *)(pkt + 0x2E) = *(volatile uint16_t *)0x20002820;
                *(volatile uint16_t *)(pkt + 0x30) = *(volatile uint16_t *)0x200027F0;
                *(volatile uint16_t *)(pkt + 0x32) = fg_charge_status();
                *(volatile uint16_t *)(pkt + 0x34) = (uint16_t)fg_status_flag_get();
                *(volatile uint16_t *)(pkt + 0x36) = (uint16_t)fg_status_flag2_get();

                for (uint8_t i = 0; i < 10; i++) {
                    *(volatile uint16_t *)(pkt + i * 2 + 6) =
                        *(volatile uint16_t *)(0x20002880 + i * 2);
                }

                *(volatile uint16_t *)pkt = (uint16_t)(*seqp + 1);
                memcmp_verify((char *)0x08080C80, 0x38, (char *)pkt);

                /* Two interleaved 50-entry ring buffers keyed on sequence%100. */
                uint16_t rem = (uint16_t)(*seqp % 100);
                if (rem <= 0x31) {
                    memcmp_verify((char *)(0x08080200 + (uint32_t)rem * 0x38), 0x38, (char *)pkt);
                } else {
                    memcmp_verify((char *)(0x08080E00 + (uint32_t)(rem - 0x32) * 0x38), 0x38, (char *)pkt);
                }

                *seqp = (uint16_t)(*seqp + 1);
                if (*seqp > 0xFDE7) {
                    *seqp = 0;
                }
                memcmp_verify((char *)0x08080C3E, 2, (char *)seqp);
            }
        }
    }

    /* Cleanup: clear bit 15, clear fuel-gauge status, flag, log. */
    *s_bms_cfg &= 0xFFFF7FFFu;
    fg_clear_status();
    *(volatile uint8_t *)(0x200028D0 + 5) = 1;
    /* "PreDischargeLockCounter = %d" (src/strings.c s_predischg_lock_ctr,
     * OEM flash 0x08017188). OEM also passes *(uint8_t*)0x20002C49 as the %d
     * argument; the project's uart_printf prototype is the format pointer only. */
    uart_printf((uint8_t *)s_predischg_lock_ctr);
}

/*
 * Master BMS state machine — main periodic dispatch (OEM FUN_08002194).
 *
 * Runs every tick. Order of business:
 *   1. fg_scan() refreshes cell measurements.
 *   2. If a forced state is pending, run state_handler_11 and return.
 *   3. On the "scan-complete" flag (bit 0 of 0x20002C80): if protection bit 5
 *      of s_bms_cfg is set, dispatch the highest-priority protection handler
 *      (07/08/09/0a/0d/0e/17_19) and return; otherwise balance cells.
 *   4. The "service" flag (bit 2) gates the rest of the tick; if clear, run the
 *      sub-loop hook and return early.
 *   5. Run the fuel-gauge OVP/threshold/alert/OC checks, then dispatch any
 *      latched fault handler (14/15/16/17_19).
 *   6. Charge/discharge MOSFET timing for both packs (when discharging above
 *      19999 with cfg bit 8), or relax to the idle config otherwise.
 *   7. OVP recovery dispatch (states 0x0B/0x0C) keyed on s_prot_status.
 *   8. Pre-charge / MOSFET balance state machine keyed on pack voltage vs the
 *      configured upper window and the over-current cell.
 *
 * Note: the OEM reaches its epilogue via `bl` to the function tail from each
 * early-exit point, i.e. these are genuine returns (not no-op call sites).
 */
void bms_state_machine(void)
{
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11ee8(void);
    extern void veneer_11f18(void);
    extern void veneer_11f08(int arg);

    volatile uint8_t  * const cfg_blk     = (volatile uint8_t  *)0x200028D0;
    volatile uint8_t  * const cell        = (volatile uint8_t  *)0x20002588;
    volatile uint32_t * const thr1        = (volatile uint32_t *)0x200028A0;
    volatile uint32_t * const thr2        = (volatile uint32_t *)0x20002800;
    volatile uint16_t * const tmr_chg1    = (volatile uint16_t *)0x20002A80;
    volatile uint16_t * const tmr_dis1    = (volatile uint16_t *)0x20002B5C;
    volatile uint16_t * const tmr_chg2    = (volatile uint16_t *)0x20002A8C;
    volatile uint16_t * const tmr_dis2    = (volatile uint16_t *)0x20002ACE;
    volatile uint32_t * const saved       = (volatile uint32_t *)0x20002AC8;
    volatile uint8_t  * const state_dup   = (volatile uint8_t  *)0x20002B58;
    volatile uint8_t  * const mode_flag   = (volatile uint8_t  *)0x20002870;
    volatile uint16_t * const recover_cnt = (volatile uint16_t *)0x20002C06;
    volatile int8_t   * const pre_state   = (volatile int8_t   *)0x20002BFE;
    volatile uint16_t * const pre_cnt     = (volatile uint16_t *)0x20002C46;

    fg_scan();

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1u;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20u;
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) != 0)        { state_handler_07(); return; }
            if (((*s_prot_status >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_prot_status >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_prot_status >> 7) & 1) != 0) { state_handler_0e(); return; }
            if (((*s_prot_status >> 6) & 1) != 0) { state_handler_0d(); return; }
            if (((*s_prot_status >> 11) & 1) != 0){ state_handler_17_19(); return; }
        }
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) == 0) { veneer_11f08(1); return; }
    *s_bms_flags &= ~4u;

    fg_ovp1_check();
    fg_ovp2_check();
    fg_threshold_check();
    fg_alert_monitor();
    fg_discharge_oc_check();

    if (*s_fault_flags != 0) {
        if (((*s_fault_flags >> 2) & 1) != 0) { state_handler_14(); return; }
        if (((*s_fault_flags >> 3) & 1) != 0) { state_handler_15(); return; }
        if (((*s_fault_flags >> 4) & 1) != 0) { state_handler_16(); return; }
        if (((*s_fault_flags >> 5) & 1) != 0 ||
            ((*s_fault_flags >> 6) & 1) != 0 ||
            ((*s_fault_flags >> 7) & 1) != 0) { state_handler_17_19(); return; }
    }

    veneer_11f68();
    veneer_11f88();
    veneer_11ee8();

    if (*s_bms_cur > 19999 && ((*s_bms_cfg >> 8) & 1) != 0) {
        /* Pack 1 */
        if ((*s_fault_flags & 1) == 0) {
            *tmr_dis1 = 0;
            if (cell[1] < cfg_blk[0x6E] && cell[2] < cfg_blk[0x6E]) {
                *tmr_chg1 = 0;
            } else {
                uint16_t c = (uint16_t)(*tmr_chg1 + 1);
                *tmr_chg1 = c;
                if ((uint16_t)(*(volatile uint16_t *)(cfg_blk + 0x70) / 100) <= c) {
                    uint32_t sv = *saved;
                    charge_mosfet_set(false);
                    discharge_mosfet_set(false);
                    bms_set_state(0x12);
                    *state_dup = 3;
                    *s_fault_flags |= 1;
                    gpio_bit_write(0x50000400, 1, 1);
                    *s_bms_cfg &= ~0x10u;
                    *tmr_chg1 = 0;
                    *saved = sv;
                }
            }
        } else {
            *tmr_chg1 = 0;
            if (cfg_blk[0x72] < cell[1] || cfg_blk[0x72] < cell[2]) {
                *tmr_dis1 = 0;
                gpio_bit_write(0x50000400, 1, 1);
            } else {
                uint16_t c = (uint16_t)(*tmr_dis1 + 1);
                *tmr_dis1 = c;
                if ((uint16_t)(*(volatile uint16_t *)(cfg_blk + 0x74) / 100) <= c) {
                    charge_mosfet_set(true);
                    discharge_mosfet_set(true);
                    *s_fault_flags &= ~1u;
                    gpio_bit_write(0x50000400, 1, 0);
                    *tmr_dis1 = 0;
                }
            }
        }
        /* Pack 2 */
        if (((*s_fault_flags >> 1) & 1) == 0) {
            *tmr_dis2 = 0;
            if (cfg_blk[0x76] < cell[1] && cfg_blk[0x76] < cell[2]) {
                *tmr_chg2 = 0;
            } else {
                uint16_t c = (uint16_t)(*tmr_chg2 + 1);
                *tmr_chg2 = c;
                if ((uint16_t)(*(volatile uint16_t *)(cfg_blk + 0x78) / 100) <= c) {
                    uint32_t sv = *saved;
                    charge_mosfet_set(false);
                    discharge_mosfet_set(false);
                    bms_set_state(0x13);
                    *state_dup = 3;
                    *s_fault_flags |= 2;
                    gpio_bit_write(0x50000400, 1, 1);
                    *s_bms_cfg &= ~0x10u;
                    *tmr_chg2 = 0;
                    *saved = sv;
                }
            }
        } else {
            *tmr_chg2 = 0;
            if (cell[1] < cfg_blk[0x7A] || cell[2] < cfg_blk[0x7A]) {
                *tmr_dis2 = 0;
                gpio_bit_write(0x50000400, 1, 1);
            } else {
                uint16_t c = (uint16_t)(*tmr_dis2 + 1);
                *tmr_dis2 = c;
                if ((uint16_t)(*(volatile uint16_t *)(cfg_blk + 0x7C) / 100) <= c) {
                    charge_mosfet_set(true);
                    discharge_mosfet_set(true);
                    *s_fault_flags &= ~2u;
                    gpio_bit_write(0x50000400, 1, 0);
                    *tmr_dis2 = 0;
                }
            }
        }
    } else if ((*mode_flag & 2) != 2) {
        if ((*s_fault_flags & 1) != 0 || ((*s_fault_flags >> 1) & 1) != 0) {
            *s_fault_flags &= ~1u;
            *s_fault_flags &= ~2u;
            gpio_bit_write(0x50000400, 1, 0);
        }
        *tmr_chg1 = 0;
        *tmr_dis1 = 0;
        *tmr_chg2 = 0;
        *tmr_dis2 = 0;
        *mode_flag = (uint8_t)((((*s_bms_cfg >> 12) & 1) == 0) ? 1 : 3);
        bms_configure(*mode_flag);
    }

    /* OVP recovery dispatch (latched protection bits 4/5 of s_prot_status). */
    if (((*s_prot_status >> 5) & 1) == 0) {
        if (((*s_prot_status >> 4) & 1) == 0) {
            /* Clear the charge-disable latch after a 50-tick recovery hold. */
            if (((*s_bms_cfg >> 7) & 1) != 0) {
                uint16_t c = (uint16_t)(*recover_cnt + 1);
                *recover_cnt = c;
                if (c > 49) {
                    *recover_cnt = 0;
                    *s_bms_cfg &= ~0x80u;
                    *s_fault_flags &= ~0x100u;
                    *s_fault_flags &= ~0x200u;
                    gpio_bit_write(0x50000400, 1, 0);
                }
            }
        } else {
            uint32_t sv = *saved;
            charge_mosfet_set(false);
            discharge_mosfet_set(false);
            gpio_bit_write(0x50000400, 1, 1);
            bms_set_state(0x0B);
            *state_dup = 3;
            *s_bms_cfg &= ~0x10u;
            *s_prot_status &= ~0x10u;
            *s_fault_flags |= 0x100;
            *saved = sv;
            *s_bms_cfg |= 0x80;
            *recover_cnt = 0;
        }
    } else {
        uint32_t sv = *saved;
        charge_mosfet_set(false);
        discharge_mosfet_set(false);
        gpio_bit_write(0x50000400, 1, 1);
        bms_set_state(0x0C);
        *state_dup = 3;
        *s_bms_cfg &= ~0x10u;
        *s_prot_status &= ~0x20u;
        *s_fault_flags |= 0x200;
        *saved = sv;
        *s_bms_cfg |= 0x80;
        *recover_cnt = 0;
    }

    /* Pre-charge / MOSFET balance state machine. `pre_state`: -1 idle/abort,
     * 1/2 discharge/charge ramp below window, 3/4 hold/precharge at window. */
    if (*thr1 < *(volatile uint16_t *)(cfg_blk + 0x16)) {
        if (*thr2 < *(volatile uint16_t *)(cfg_blk + 0x16) || *s_bms_cur <= 19999) {
            if (19999 < *s_bms_cur) {
                if ((*s_fault_flags & 1) == 0 && ((*s_fault_flags >> 1) & 1) == 0) {
                    if (((*s_bms_cfg >> 12) & 1) == 0 || ((*s_bms_cfg >> 7) & 1) != 0) {
                        charge_mosfet_set(false);
                        discharge_mosfet_set(false);
                    } else if (*pre_state == 1) {
                        if (((*s_bms_cfg >> 6) & 1) == 0) {
                            *s_bms_cfg |= 0x100;
                        }
                    } else {
                        *pre_state = 1;
                        *pre_cnt = 0;
                        discharge_mosfet_set(true);
                        if (((*s_bms_cfg >> 8) & 1) != 0) {
                            charge_mosfet_set(false);
                        }
                    }
                } else {
                    charge_mosfet_set(false);
                    discharge_mosfet_set(false);
                    *pre_state = -1;
                }
            } else {
                if (*pre_state != 0) {
                    *pre_state = 0;
                    *s_bms_cfg &= ~0x100u;
                    *pre_cnt = 0;
                }
                charge_mosfet_set(false);
                if ((*s_fault_flags & 1) == 0 && ((*s_fault_flags >> 1) & 1) == 0) {
                    if (((*s_bms_cfg >> 12) & 1) == 0 || ((*s_bms_cfg >> 7) & 1) != 0) {
                        discharge_mosfet_set(false);
                    } else {
                        discharge_mosfet_set(true);
                    }
                } else {
                    discharge_mosfet_set(false);
                    *pre_state = -1;
                }
            }
        } else if ((*s_fault_flags & 1) == 0 && ((*s_fault_flags >> 1) & 1) == 0) {
            if (((*s_bms_cfg >> 12) & 1) == 0 || ((*s_bms_cfg >> 7) & 1) != 0) {
                charge_mosfet_set(false);
                discharge_mosfet_set(false);
            } else {
                if (*pre_state != 2) {
                    *pre_state = 2;
                    *s_bms_cfg |= 0x100;
                    *pre_cnt = 0;
                }
                charge_mosfet_set(true);
                discharge_mosfet_set(true);
            }
        } else {
            charge_mosfet_set(false);
            discharge_mosfet_set(false);
            *pre_state = -1;
        }
    } else {
        *s_bms_oc_tmr = 0;
        if ((*s_fault_flags & 1) == 0 && ((*s_fault_flags >> 1) & 1) == 0) {
            if (19999 < *s_bms_cur) {
                if (((*s_bms_cfg >> 12) & 1) == 0 || ((*s_bms_cfg >> 7) & 1) != 0) {
                    charge_mosfet_set(false);
                    discharge_mosfet_set(false);
                } else if (*pre_state == 4) {
                    if (((*s_bms_cfg >> 6) & 1) == 0) {
                        uint16_t c = (uint16_t)(*pre_cnt + 1);
                        *pre_cnt = c;
                        if (c > 19) {
                            *pre_cnt = 0;
                            *s_bms_cfg |= 0x100;
                            charge_mosfet_set(true);
                        }
                    } else {
                        uint16_t c = (uint16_t)(*pre_cnt + 1);
                        *pre_cnt = c;
                        if (c > 299) {
                            *pre_cnt = 0;
                            charge_mosfet_set(false);
                        }
                    }
                } else {
                    *pre_state = -1;
                    discharge_mosfet_set(true);
                    uint16_t c = (uint16_t)(*pre_cnt + 1);
                    *pre_cnt = c;
                    if (c > 9) {
                        *pre_state = 4;
                        *pre_cnt = 0;
                        charge_mosfet_set(false);
                    }
                }
            } else {
                if (((*s_bms_cfg >> 12) & 1) == 0 || ((*s_bms_cfg >> 7) & 1) != 0) {
                    charge_mosfet_set(false);
                    discharge_mosfet_set(false);
                } else {
                    if (*pre_state != 3) {
                        *pre_state = 3;
                        *s_bms_cfg &= ~0x100u;
                        *pre_cnt = 0;
                        discharge_mosfet_set(true);
                    }
                    charge_mosfet_set(false);
                }
            }
        } else {
            charge_mosfet_set(false);
            discharge_mosfet_set(false);
            *pre_state = -1;
        }
    }

    veneer_11f18();
    fg_watchdog_kick();
    veneer_11f08(1);
}

/* ===== State timer functions (periodic ISR-driven dispatch) ===== */

/* state_timer_05 (FUN_080063e0): complex state engine with fault checks */
void state_timer_05(void)
{
    extern void fg_scan(void);
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11ee8(void);
    extern void veneer_11f18(void);
    extern void veneer_11f58(uint32_t);
    extern void veneer_11f08(int arg);

    fg_scan();

    /* First literal pool @ 0x08006670..0x080066A4 */
    volatile uint8_t  *s_gpio_flag = (volatile uint8_t  *)0x2000453D;   /* unused here */
    volatile uint8_t  *s_gpio_cnt  = (volatile uint8_t  *)0x2000450C;
    volatile uint8_t  *s_cmp2_base = (volatile uint8_t  *)0x200028D0;
    volatile uint32_t *s_cmp       = (volatile uint32_t *)0x20002800;
    /* s_bms_cur = 0x200028C8 (already shared) used as "cmp3" here */
    volatile uint16_t *s_tmr       = (volatile uint16_t *)0x20002C78;
    volatile uint32_t *s_voltage   = (volatile uint32_t *)0x20002824;
    (void)s_gpio_flag;

    /* Second literal pool @ 0x08006730..0x08006744 — flags re-loaded, plus
     * dst-pointer cell, the immediate 0xAAAA, arg-pointer, and two counters. */
    volatile uint32_t **s_dst_pp  = (volatile uint32_t **)0x20002C10;
    volatile uint32_t  *s_arg     = (volatile uint32_t  *)0x200028C0;
    volatile uint32_t  *s_tmr2    = (volatile uint32_t  *)0x20002C74;
    volatile uint32_t  *s_tmr3    = (volatile uint32_t  *)0x20002AC8;

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) != 0)        { state_handler_07(); return; }
            if (((*s_prot_status >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_prot_status >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_prot_status >> 11) & 1) == 0) {
                if (((*s_prot_status >> 8) & 1) != 0) { state_handler_0f(); return; }
                if (((*s_prot_status >> 9) & 1) != 0) { state_handler_10(); return; }
            }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        cell_balance_update();
    }

    if (((*s_bms_flags >> 1) & 1) != 0) {
        *s_bms_flags &= ~2U;
        if (*s_gpio_cnt == 1) {
            *s_gpio_cnt = 0;
        } else {
            bool b = gpio_bit_read(0x50000000, 0x400);
            if (b) {
                uint8_t cnt = *s_gpio_cnt + 1;
                *s_gpio_cnt = cnt;
                if (cnt > 9) {
                    *s_gpio_cnt = 0;
                    service_uart_init();
                }
            }
        }
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_uvp1_check(); fg_uvp2_check(); fg_ovp1_check(); fg_ovp2_check();
        fg_threshold_check(); fg_alert_monitor();
        if (*s_fault_flags != 0) {
            if ((*s_cmp < *(volatile uint16_t *)(s_cmp2_base + 0x16)) ||
                (*s_bms_cur <= BMS_OC_DISCHARGE_THRESHOLD)) {
                *s_fault_flags &= ~1U;
                *s_fault_flags &= ~2U;
            } else {
                if ((*s_fault_flags & 1) != 0)       { state_handler_12(); return; }
                if (((*s_fault_flags >> 1) & 1) != 0) { state_handler_13(); return; }
            }
            if (((*s_fault_flags >> 2) & 1) != 0) { state_handler_14(); return; }
            if (((*s_fault_flags >> 3) & 1) != 0) { state_handler_15(); return; }
            if (((*s_fault_flags >> 4) & 1) != 0) { state_handler_16(); return; }
            if ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1))) {
                state_handler_17_19(); return;
            }
        }
        if (((*s_bms_cfg >> 10) & 1) != 0) {
            uint16_t cnt = (uint16_t)(*s_tmr + 1);
            *s_tmr = cnt;
            if (cnt > 9) {
                *s_tmr = 0;
                if (*s_voltage <= BMS_OC_CHARGE_THRESHOLD) {
                    *s_prot_status |= 0x200;
                    state_handler_10(); return;
                }
                state_handler_03_init(); return;
            }
        }
        veneer_11f68(); veneer_11f88(); veneer_11ee8();
        veneer_11f18(); fg_watchdog_kick();
    }

    if (((*s_bms_flags >> 3) & 1) != 0) {
        *s_bms_flags &= ~8U;
        **s_dst_pp = 0xAAAA;                /* literal 0xAAAA from pool */
        veneer_11f58(*s_arg);
    }
    if (((*s_bms_flags >> 4) & 1) != 0) {
        *s_bms_flags &= ~0x10U;
    }
    if (((*s_bms_flags >> 5) & 1) != 0) {
        *s_bms_flags &= ~0x20U;
        *s_tmr2 += 1;
        *s_tmr3 += 1;
    }
}

/* state_timer_03 (FUN_08006810): discharge monitoring */
void state_timer_03(void)
{
    extern void fg_scan(void);
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11f18(void);
    extern void veneer_11f08(int arg);

    fg_scan();

    /* Literal pool @ 0x08006934..0x08006944: 5 shared BMS cells. */

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) == 0)        { state_handler_01(); return; }
            if (((*s_prot_status >> 7) & 1) != 0) { state_handler_0e(); return; }
            if (((*s_prot_status >> 6) & 1) != 0) { state_handler_0d(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check();
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    veneer_11f08(0);
}

/* state_timer_06 (FUN_0800699c): discharge + alert monitoring */
void state_timer_06(void)
{
    extern void fg_scan(void);
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11f18(void);
    extern void veneer_11f08(int arg);

    fg_scan();

    /* Literal pool @ 0x08006AC0..0x08006AD0: only 4 cells
     * (s_bms_state, s_bms_flags, s_bms_cfg, s_prot_status). s_fault_flags is
     * also referenced via DAT_08006ad0 — same 0x20002C44 cell, second
     * load through a separate pool slot. */

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 1) & 1) == 0) {
                if ((*s_prot_status & 1) != 0) {
                    state_handler_07(); return;
                }
                state_handler_01(); return;
            }
            if (((*s_prot_status >> 7) & 1) != 0) { state_handler_0e(); return; }
            if (((*s_prot_status >> 6) & 1) != 0) { state_handler_0d(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check();
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    veneer_11f08(0);
}

/* state_timer_07 (FUN_08006b28): charge + alert monitoring */
void state_timer_07(void)
{
    extern void fg_scan(void);
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11f18(void);
    extern void veneer_11f08(int arg);

    fg_scan();

    /* Literal pool @ 0x08006C4C..0x08006C5C: 5 shared BMS cells. */

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) != 0)        { state_handler_07(); return; }
            if (((*s_prot_status >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_prot_status >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_charge_oc_check();
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    veneer_11f08(0);
}

/* state_timer_08 (FUN_08006cb4): charge + alert (dup) */
void state_timer_08(void)
{
    extern void fg_scan(void);
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11f18(void);
    extern void veneer_11f08(int arg);

    fg_scan();

    /* Literal pool @ 0x08006DD8..0x08006DE8: 5 shared BMS cells. */

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) != 0)        { state_handler_07(); return; }
            if (((*s_prot_status >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_prot_status >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_charge_oc_check();
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    veneer_11f08(0);
}

/* state_timer_09 (FUN_08006e40): discharge + charge monitoring.
 * Note: unlike the other timers, this one does NOT check s_bms_state
 * upfront — the pool starts directly at s_bms_flags. cell_balance_update
 * is also called BEFORE the cfg check, not after. */
void state_timer_09(void)
{
    extern void fg_scan(void);
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11f18(void);
    extern void veneer_11f08(int arg);

    fg_scan();

    /* Literal pool @ 0x08006F58..0x08006F64: 4 cells (no s_state, no cur). */

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        cell_balance_update();
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) != 0)        { state_handler_07(); return; }
            if (((*s_prot_status >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_prot_status >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check(); fg_charge_oc_check();
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    veneer_11f08(0);
}

/* state_timer_10 (FUN_08006fbc): DMA/USART init with threshold config */
void state_timer_10(void)
{
    uint32_t result;
    uint8_t  local_cfg[20];
    uint8_t  i;

    memset_byte_fill(local_cfg, 0, 20);

    /* Enable peripheral clocks via RCC (0x40021000): APB2ENR bit 12 and
     * IOPENR bit 1 (GPIOB), with the usual read-back of IOPENR. */
    *(volatile uint32_t *)(0x40021000 + 0x34) |= 0x1000;
    *(volatile uint32_t *)(0x40021000 + 0x2C) |= 2;
    (void)(*(volatile uint32_t *)(0x40021000 + 0x2C) & 2);

    /* GPIO pin config struct: {Pin=0x38, Mode=2, Pull=0, Speed=3, Alt=0}. */
    local_cfg[0]  = 0x38;
    local_cfg[4]  = 2;
    local_cfg[12] = 3;

    gpio_pin_config((uint32_t *)0x50000400, (gpio_pin_cfg_t *)local_cfg);

    volatile uint32_t *s_ctx = (volatile uint32_t *)0x20002BA4;
    s_ctx[0]  = 0x40013000;   /* USART1 */
    s_ctx[1]  = 0x104;
    s_ctx[2]  = 0;
    s_ctx[3]  = 0;
    s_ctx[4]  = 0;
    s_ctx[5]  = 0;
    s_ctx[6]  = 0x200;
    s_ctx[7]  = 0x10;
    s_ctx[8]  = 0;
    s_ctx[9]  = 0;
    s_ctx[10] = 0;
    s_ctx[11] = 7;

    *(volatile uint32_t *)0x200047DC = 0;
    result = dma_usart_init((int *)s_ctx);
    if (result != 0) {
        system_reset();
    }

    flash_opt_byte_op(0x19, 2);
    nvic_enable_irq_s(0x19);

    for (i = 0; i <= 0x1F; i++) {
        *(volatile uint8_t *)(0x20002B64 + i) = 0xFF;
        *(volatile uint8_t *)(0x20002B84 + i) = 0xFF;
    }
}

/*
 * service_uart_init (FUN_0800AB7C): bring up the USART1 service/debug UART,
 * gated on PA10 still being asserted.
 *
 * Reached from state_timer_05 once PA10 (GPIOA mask 0x400) has been sampled
 * high for >9 consecutive ticks. It re-samples PA10 as a debounce: if the pin
 * is still high it configures USART1, otherwise it just clears the TX-enable
 * flag (s_tx_enabled @ 0x2000453D, shared with uart.c) and returns.
 *
 * Bring-up sequence (PA10 still high):
 *   - RCC->APB2ENR |= USART1EN (bit14); RCC->IOPENR |= IOPAEN (bit0), with the
 *     usual dead read-back of IOPENR preserved for fidelity.
 *   - PA9/PA10 -> AF4 (USART1 TX/RX), very-high speed, no pull.
 *   - Populate the UART handle @ 0x20004488 (== uart.c s_uart_base): instance
 *     = USART1 (0x40013800), baud 9600 (0x2580), mode TX|RX (0xC), and the two
 *     advanced-feature words (handle[9]=0x20, handle[0xF]=0x2000); zero CR1/CR2/
 *     CR3 on the instance first.
 *   - hal_uart_init() (HAL_UART_Init, in uart.c); reset on failure.
 *   - Clear the handle's RX/TX bookkeeping (offsets 0x78/0x7C=0x20, 0x80=0) and
 *     the five ring-buffer index cells, set USART1->CR1 |= RXNEIE (0x20), set
 *     IRQ27 (USART1) priority 0 and enable it, then mark TX enabled.
 */
void service_uart_init(void)
{
    volatile uint32_t * const RCC         = (volatile uint32_t *)0x40021000;
    volatile uint32_t * const s_uart_hnd  = (volatile uint32_t *)0x20004488;
    volatile uint8_t  * const s_tx_enable = (volatile uint8_t  *)0x2000453D;

    if (!gpio_bit_read(0x50000000, 0x400)) {
        *s_tx_enable = 0;
        return;
    }

    gpio_pin_cfg_t cfg;
    memset_byte_fill((uint8_t *)&cfg, 0, sizeof cfg);

    RCC[0x34 / 4] |= 0x4000;               /* APB2ENR: USART1EN */
    RCC[0x2C / 4] |= 0x1;                   /* IOPENR:  IOPAEN   */
    (void)(RCC[0x2C / 4] & 0x1);            /* dead read-back (OEM fidelity) */

    cfg.pin_mask = 0x600;                   /* PA9 (TX) | PA10 (RX) */
    cfg.mode     = GPIO_MODE_AF;            /* 2 */
    cfg.pupd     = 0;
    cfg.speed    = 3;                       /* very-high */
    cfg.af       = 4;                       /* AF4 = USART1 */
    gpio_pin_config((uint32_t *)0x50000000, &cfg);

    s_uart_hnd[0]  = 0x40013800;            /* USART1 instance */
    s_uart_hnd[1]  = 0x2580;                /* baud 9600 */
    s_uart_hnd[2]  = 0;
    s_uart_hnd[3]  = 0;
    s_uart_hnd[4]  = 0;
    s_uart_hnd[5]  = 0xC;                   /* mode TX|RX */
    s_uart_hnd[6]  = 0;
    s_uart_hnd[7]  = 0;
    s_uart_hnd[8]  = 0;
    s_uart_hnd[9]  = 0x20;
    s_uart_hnd[15] = 0x2000;

    *(volatile uint32_t *)(s_uart_hnd[0] + 0) = 0;   /* USART1->CR1 */
    *(volatile uint32_t *)(s_uart_hnd[0] + 4) = 0;   /* USART1->CR2 */
    *(volatile uint32_t *)(s_uart_hnd[0] + 8) = 0;   /* USART1->CR3 */

    *(volatile uint32_t *)0x200047DC = 0;

    if (hal_uart_init((int *)s_uart_hnd) != 0) {   /* HAL_UART_Init */
        system_reset();
    }

    s_uart_hnd[0x80 / 4] = 0;
    s_uart_hnd[0x78 / 4] = 0x20;
    s_uart_hnd[0x7C / 4] = 0x20;

    *(volatile uint16_t *)0x20004542 = 0;
    *(volatile uint16_t *)0x2000453E = 0;
    *(volatile uint16_t *)0x20004540 = 0;
    *(volatile uint16_t *)0x20004544 = 0;
    *(volatile uint16_t *)0x20002C84 = 0;

    *(volatile uint32_t *)(s_uart_hnd[0] + 0) |= 0x20;  /* USART1->CR1 |= RXNEIE */

    flash_opt_byte_op(0x1B, 0);             /* NVIC_SetPriority(USART1_IRQn=27, 0) */
    nvic_enable_irq_s(0x1B);                /* NVIC_EnableIRQ(27) */

    *s_tx_enable = 1;
}

/* state_timer_charge_a (FUN_0800a794): charge monitoring with bootloader entry */
void state_timer_charge_a(void)
{
    extern void fg_scan(void);
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11f18(void);
    extern void veneer_11f08(int arg);
    extern void bootloader_entry(void);

    fg_scan();

    /* Pool @ 0x0800A90C: shared BMS cells + boot timer (0x200029A4) and
     * the stats struct (0x200028D0), whose +0x24 word sets the boot delay. */
    volatile uint16_t *s_boot_tmr = (volatile uint16_t *)0x200029A4;
    volatile uint32_t *s_div      = (volatile uint32_t *)0x200028D0;

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_prot_status >> 2) & 1) == 0) { state_handler_01(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_charge_oc_check();
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        if (BMS_OC_DISCHARGE_THRESHOLD < *s_bms_cur) {
            uint16_t cnt = (uint16_t)(*s_bms_oc_tmr + 1);
            *s_bms_oc_tmr = cnt;
            if (cnt > 9) { *s_bms_oc_tmr = 0; state_handler_02(); return; }
        } else {
            *s_bms_oc_tmr = 0;
        }
        if ((((*s_bms_cfg >> 3) & 1) == 0) || (BMS_OC_DISCHARGE_THRESHOLD < *s_bms_cur)) {
            *s_boot_tmr = 0;
        } else {
            uint16_t cnt = (uint16_t)(*s_boot_tmr + 1);
            *s_boot_tmr = cnt;
            if ((s_div[0x24 / 4] / 100) <= cnt) {
                bootloader_entry();
                return;
            }
        }
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    veneer_11f08(0);
}

/* state_timer_charge_b (FUN_0800a988): charge monitoring (dup) */
void state_timer_charge_b(void)
{
    extern void fg_scan(void);
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11f18(void);
    extern void veneer_11f08(int arg);
    extern void bootloader_entry(void);

    fg_scan();

    /* Same shared cells as charge_a (identical literal pool). */
    volatile uint16_t *s_boot_tmr = (volatile uint16_t *)0x200029A4;
    volatile uint32_t *s_div      = (volatile uint32_t *)0x200028D0;

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 3) & 1) == 0) {
                if (((*s_prot_status >> 2) & 1) != 0) { state_handler_09(); return; }
                state_handler_01(); return;
            }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_charge_oc_check();
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        if (BMS_OC_DISCHARGE_THRESHOLD < *s_bms_cur) {
            uint16_t cnt = (uint16_t)(*s_bms_oc_tmr + 1);
            *s_bms_oc_tmr = cnt;
            if (cnt > 9) { *s_bms_oc_tmr = 0; state_handler_02(); return; }
        } else {
            *s_bms_oc_tmr = 0;
        }
        if ((((*s_bms_cfg >> 3) & 1) == 0) || (BMS_OC_DISCHARGE_THRESHOLD < *s_bms_cur)) {
            *s_boot_tmr = 0;
        } else {
            uint16_t cnt = (uint16_t)(*s_boot_tmr + 1);
            *s_boot_tmr = cnt;
            if ((s_div[0x24 / 4] / 100) <= cnt) {
                bootloader_entry();
                return;
            }
        }
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    veneer_11f08(0);
}

/*
 * Bootloader / shipping power-down entry check (OEM FUN_080050ac).
 *
 * Forces the charge MOSFET off (PB9 = 0), then polls the FEDL5236 total-
 * voltage register (reg 0x34). The raw reading is scaled (raw * 19536 / 1000,
 * i.e. an mV conversion) and compared against 10000; once the pack has fallen
 * to/below ~10 V it runs the FEDL5236 POWER_DOWN command sequence (reg 0x0C,
 * waiting on the bit-7 handshake) and samples the button on PA11:
 *   pressed (high) -> print "POWER_DOWN_Finish", return true;
 *   released       -> print "POWER_DOWN_Error",  return false.
 * Either poll stage gives up after ~250 ticks (5 ms each) and returns false.
 * `s_bms_flags` bits 4/5 are cleared opportunistically (pending TX/alert acks).
 */
bool button_entry_check(void)
{
    volatile uint8_t * const rx = (volatile uint8_t *)0x20002B84;
    uint32_t scaled;
    uint8_t  ticks;

    gpio_bit_write(0x50000400, 0x200, 0);             /* PB9 = 0 (charge off) */
    uart_printf((uint8_t *)s_chargeon_off_pb9_0);
    delay_ms(100);
    ticks = 0;
    smbus_write_reg(8, 1, 0xFF);
    delay_ms(5);
    smbus_write_reg(8, 0x91, 0xFF);
    uart_tx_flush();

    for (;;) {
        delay_ms(5);
        scaled = 100000u;                              /* fallback when the read fails */
        if (smbus_read(0x34, 2) != 0) {
            scaled = *(volatile uint16_t *)(rx + 2);
            scaled = (scaled * 19536u) / 1000u;
            if (((*s_bms_flags >> 4) & 1) != 0) {
                *s_bms_flags &= ~0x10u;
            }
        }
        if (++ticks > 0xF9) {
            uart_tx_flush();
            return false;
        }
        if (((*s_bms_flags >> 5) & 1) != 0) {
            *s_bms_flags &= ~0x20u;
        }
        uart_resp_handler();
        uart_tx_isr();

        if (scaled <= 10000u) {
            /* Pack drained — issue FEDL5236 power-down and confirm. */
            ticks = 0;
            uart_tx_flush();
            do {
                delay_ms(5);
                if (++ticks > 0xF9) {
                    uart_tx_flush();
                    return false;
                }
                rx[2] = 0;
                smbus_read(0x0C, 1);
                if (((*s_bms_flags >> 5) & 1) != 0) {
                    *s_bms_flags &= ~0x20u;
                }
                uart_resp_handler();
                uart_tx_isr();
            } while ((rx[2] & 0x80) == 0x80);

            delay_ms(5);
            uart_printf((uint8_t *)s_pdown_set_command);
            uart_tx_flush();
            smbus_read_nack(0x0C, 0x10);
            delay_ms(50);
            if (gpio_bit_read(0x50000000, 0x800)) {    /* PA11 */
                uart_printf((uint8_t *)s_pdown_finish);
                uart_tx_flush();
                return true;
            } else {
                uart_printf((uint8_t *)s_pdown_error);
                uart_tx_flush();
                return false;
            }
        }
    }
}

/*
 * state_timer_0c (FUN_0800122c) — state-0x0C periodic tick.
 * Standard timer skeleton; the flag-2 phase runs the discharge over-current
 * path and a discharge-recovery debounce on cell[1]/[2] vs cfg_blk[0x72]
 * (debounce count cfg_blk[0x74]/100, timer @ 0x20002B5C).
 */
void state_timer_0c(void)
{
    extern void fg_scan(void);
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11f18(void);
    extern void veneer_11f08(int arg);
    fg_scan();

    volatile uint8_t  *s_cell  = (volatile uint8_t  *)0x20002588;
    volatile uint8_t  *cfg_blk = (volatile uint8_t  *)0x200028D0;
    volatile uint16_t *s_tmr   = (volatile uint16_t *)0x20002B5C;

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) != 0)        { state_handler_07(); return; }
            if (((*s_prot_status >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_prot_status >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check();
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        if (*s_bms_cur <= BMS_OC_DISCHARGE_THRESHOLD) { state_handler_01(); return; }
        if ((cfg_blk[0x72] < s_cell[1]) || (cfg_blk[0x72] < s_cell[2])) {
            *s_tmr = 0;
        } else {
            uint16_t cnt = (uint16_t)(*s_tmr + 1);
            *s_tmr = cnt;
            if ((uint16_t)(*(volatile uint16_t *)(cfg_blk + 0x74) / 100) <= cnt) {
                state_handler_01(); return;
            }
        }
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    veneer_11f08(0);
}

/*
 * state_timer_12 (FUN_08001434) — state-0x12 periodic tick. Sibling of 0c;
 * the discharge-recovery debounce is on cell[1]/[2] < cfg_blk[0x7a] (count
 * cfg_blk[0x7c]/100, timer @ 0x20002ACE).
 */
void state_timer_12(void)
{
    extern void fg_scan(void);
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11f18(void);
    extern void veneer_11f08(int arg);
    fg_scan();

    volatile uint8_t  *s_cell  = (volatile uint8_t  *)0x20002588;
    volatile uint8_t  *cfg_blk = (volatile uint8_t  *)0x200028D0;
    volatile uint16_t *s_tmr   = (volatile uint16_t *)0x20002ACE;

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) != 0)        { state_handler_07(); return; }
            if (((*s_prot_status >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_prot_status >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check();
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        if (*s_bms_cur <= BMS_OC_DISCHARGE_THRESHOLD) { state_handler_01(); return; }
        if ((s_cell[1] < cfg_blk[0x7A]) || (s_cell[2] < cfg_blk[0x7A])) {
            *s_tmr = 0;
        } else {
            uint16_t cnt = (uint16_t)(*s_tmr + 1);
            *s_tmr = cnt;
            if ((uint16_t)(*(volatile uint16_t *)(cfg_blk + 0x7C) / 100) <= cnt) {
                state_handler_01(); return;
            }
        }
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    veneer_11f08(0);
}

/*
 * state_timer_13 (FUN_0800163c) — state-0x13 periodic tick (normal/charging).
 * Runs the full fuel-gauge check suite, dispatches latched faults
 * (g_fault_flags bits 0..7 -> handlers 12/13/14/15/16/17_19), and when the
 * pack is above the OC limit (20000) toggles the PB9 charge MOSFET based on
 * the pack voltage vs the cfg_blk[0x16] upper window.
 */
void state_timer_13(void)
{
    extern void fg_scan(void);
    extern void veneer_11f68(void);
    extern void veneer_11ee8(void);
    extern void veneer_11f18(void);
    extern void veneer_11f08(int arg);
    fg_scan();

    volatile uint8_t  *cfg_blk = (volatile uint8_t  *)0x200028D0;
    volatile uint32_t *thr2    = (volatile uint32_t *)0x20002800;

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) != 0)        { state_handler_07(); return; }
            if (((*s_prot_status >> 5) & 1) != 0) { state_handler_0c(); return; }
            if (((*s_prot_status >> 4) & 1) != 0) { state_handler_0b(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_uvp1_check(); fg_uvp2_check(); fg_ovp1_check(); fg_ovp2_check();
        fg_threshold_check(); fg_alert_monitor();
        if (*s_fault_flags != 0) {
            if ((*s_fault_flags & 1) != 0)        { state_handler_12(); return; }
            if (((*s_fault_flags >> 1) & 1) != 0) { state_handler_13(); return; }
            if (((*s_fault_flags >> 2) & 1) != 0) { state_handler_14(); return; }
            if (((*s_fault_flags >> 3) & 1) != 0) { state_handler_15(); return; }
            if (((*s_fault_flags >> 4) & 1) != 0) { state_handler_16(); return; }
            if (((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) ||
                ((*s_fault_flags >> 7) & 1)) { state_handler_17_19(); return; }
        }
        if (*s_bms_cur <= 20000) {
            if (((*s_prot_status >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_prot_status >> 2) & 1) != 0) { state_handler_09(); return; }
            state_handler_01(); return;
        }
        if (*thr2 < *(volatile uint16_t *)(cfg_blk + 0x16)) {
            if (((*s_bms_cfg >> 6) & 1) != 0) {
                *s_bms_cfg &= ~0x40U;
                gpio_bit_write(0x50000400, 0x200, 0);
            }
        } else if (((*s_bms_cfg >> 6) & 1) == 0) {
            *s_bms_cfg |= 0x40;
            gpio_bit_write(0x50000400, 0x200, 1);
        }
        veneer_11f68(); veneer_11ee8(); veneer_11f18();
        fg_watchdog_kick();
    }
    veneer_11f08(1);
}

/*
 * state_timer_15 (FUN_080019b8) — state-0x15 periodic tick. Discharge-OC
 * monitor: when the pack current exceeds 19999 for >9 consecutive ticks it
 * transitions via state_handler_02 (counter @ 0x20002C0C = s_bms_oc_tmr).
 */
void state_timer_15(void)
{
    extern void fg_scan(void);
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11f18(void);
    extern void veneer_11f08(int arg);
    fg_scan();

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) != 0)        { state_handler_07(); return; }
            if (((*s_prot_status >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_prot_status >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_prot_status >> 6) & 1) == 0) { state_handler_01(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check(); fg_charge_oc_check();
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        if (BMS_OC_DISCHARGE_THRESHOLD < *s_bms_cur) {
            uint16_t cnt = (uint16_t)(*s_bms_oc_tmr + 1);
            *s_bms_oc_tmr = cnt;
            if (cnt > 9) {
                *s_bms_oc_tmr = 0;
                state_handler_02();
                return;
            }
        } else {
            *s_bms_oc_tmr = 0;
        }
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    veneer_11f08(0);
}

/*
 * state_timer_0a (FUN_08000f3c) — state-0x0A periodic tick. The simplest of
 * the timer family: protection dispatch (prot bits 1->08, 0->07, bit4 clear
 * ->01, bit11->17_19) then the flag-2 alert/discharge-OC phase with the
 * bit5/6/7 fault escalation. No charge/threshold phase.
 */
void state_timer_0a(void)
{
    extern void fg_scan(void);
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11f18(void);
    extern void veneer_11f08(int arg);
    fg_scan();

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) != 0)        { state_handler_07(); return; }
            if (((*s_prot_status >> 4) & 1) == 0) { state_handler_01(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check();
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    veneer_11f08(0);
}

/*
 * can_transmit (FUN_080055a8) — NOT a CAN routine; the name is a decomp
 * misnomer. This is the state-0x16 periodic timer. Protection dispatch is the
 * standard 08/07/0a/09/17_19 set (with a veneer_11f18 fired right after the
 * cfg-bit5 clear), and the flag-2 phase runs alert + discharge-OC + charge-OC,
 * then a charge-recovery debounce: while s_cell[0] >= cfg_blk[0x92], count up
 * (limit cfg_blk[0x94]/100, timer @ 0x20002ABA) and transition via
 * state_handler_01.
 */
void can_transmit(void)
{
    extern void fg_scan(void);
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11f18(void);
    extern void veneer_11f08(int arg);
    fg_scan();

    volatile uint8_t  *s_cell  = (volatile uint8_t  *)0x20002588;
    volatile uint8_t  *cfg_blk = (volatile uint8_t  *)0x200028D0;
    volatile uint16_t *s_tmr   = (volatile uint16_t *)0x20002ABA;

    if (*s_bms_state != 0) { state_handler_11(); return; }

    if ((*s_bms_flags & 1) != 0) {
        *s_bms_flags &= ~1U;
        if (((*s_bms_cfg >> 5) & 1) != 0) {
            *s_bms_cfg &= ~0x20U;
            veneer_11f18();
            if (((*s_prot_status >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_prot_status & 1) != 0)        { state_handler_07(); return; }
            if (((*s_prot_status >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_prot_status >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_prot_status >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        cell_balance_update();
    }

    if (((*s_bms_flags >> 2) & 1) != 0) {
        *s_bms_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check(); fg_charge_oc_check();
        if ((*s_fault_flags != 0) &&
            ((((*s_fault_flags >> 5) & 1) || ((*s_fault_flags >> 6) & 1) || ((*s_fault_flags >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        if (cfg_blk[0x92] < s_cell[0]) {
            *s_tmr = 0;
        } else {
            uint16_t cnt = (uint16_t)(*s_tmr + 1);
            *s_tmr = cnt;
            if ((uint16_t)(*(volatile uint16_t *)(cfg_blk + 0x94) / 100) <= cnt) {
                state_handler_01(); return;
            }
        }
        veneer_11f68(); veneer_11f88();
        fg_watchdog_kick();
    }
    veneer_11f08(0);
}
