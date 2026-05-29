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
    volatile uint32_t * const s_status    = (volatile uint32_t *)0x20002C00;
    volatile uint32_t * const s_precharge = (volatile uint32_t *)0x2000286C;

    if (((*s_precharge >> 11) & 1) == 0) {
        if ((((*g_fault_flags >> 6) & 1) != 0) || (((*g_fault_flags >> 7) & 1) != 0)) {
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

/* ---- State timer handlers (periodic ISR-driven dispatch) ----
 * Each is registered as a periodic callback for a specific BMS state.
 * Pattern: fg_scan() → check flags → dispatch state_handler → fault check → response chain → watchdog
 */

void state_timer_0b(void)
{
    extern void fg_scan(void);
    fg_scan();
    volatile uint8_t *s_state = (volatile uint8_t *)0x200011C4;
    volatile uint32_t *s_flags = (volatile uint32_t *)0x200011C8;
    volatile uint32_t *s_cfg   = (volatile uint32_t *)0x200011CC;
    volatile uint32_t *s_fault = (volatile uint32_t *)0x200011D0;
    volatile uint32_t *s_aux   = (volatile uint32_t *)0x200011D4;

    if (*s_state != 0) { state_handler_11(); return; }

    if ((*s_flags & 1) != 0) {
        *s_flags &= ~1U;
        if (((*s_cfg >> 5) & 1) != 0) {
            *s_cfg &= ~0x20U;
            if (((*s_fault >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_fault & 1) != 0) { state_handler_07(); return; }
            if (((*s_fault >> 5) & 1) == 0) { state_handler_01(); return; }
            if (((*s_fault >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        extern void cell_balance_update(void);
        cell_balance_update();
    }

    if (((*s_flags >> 2) & 1) != 0) {
        *s_flags &= ~4U;
        fg_alert_monitor();
        fg_discharge_oc_check();
        if ((*s_aux != 0) &&
            ((((*s_aux >> 5) & 1) || ((*s_aux >> 6) & 1) || ((*s_aux >> 7) & 1)))) {
            state_handler_17_19(); return;
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

void state_timer_0c(void)
{
    extern void fg_scan(void);
    fg_scan();
    volatile uint8_t *s_state = (volatile uint8_t *)0x200013B8;
    volatile uint32_t *s_flags = (volatile uint32_t *)0x200013BC;
    volatile uint32_t *s_cfg   = (volatile uint32_t *)0x200013C0;
    volatile uint32_t *s_fault = (volatile uint32_t *)0x200013C4;
    volatile uint32_t *s_aux   = (volatile uint32_t *)0x200013C8;
    volatile uint32_t *s_cnt   = (volatile uint32_t *)0x200013CC;
    volatile uint32_t *s_thr   = (volatile uint32_t *)0x200013D0;
    volatile uint32_t *s_tmr   = (volatile uint32_t *)0x200013DC;
    volatile uint8_t  *s_cell1 = (volatile uint8_t  *)0x200013D4;
    volatile uint8_t  *s_cellv = (volatile uint8_t  *)0x200013D8;

    if (*s_state != 0) { state_handler_11(); return; }

    if ((*s_flags & 1) != 0) {
        *s_flags &= ~1U;
        if (((*s_cfg >> 5) & 1) != 0) {
            *s_cfg &= ~0x20U;
            if (((*s_fault >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_fault & 1) != 0) { state_handler_07(); return; }
            if (((*s_fault >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_fault >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_fault >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        extern void cell_balance_update(void);
        cell_balance_update();
    }

    if (((*s_flags >> 2) & 1) != 0) {
        *s_flags &= ~4U;
        fg_alert_monitor();
        fg_discharge_oc_check();
        if ((*s_aux != 0) &&
            ((((*s_aux >> 5) & 1) || ((*s_aux >> 6) & 1) || ((*s_aux >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        if (*s_cnt <= *s_thr) { state_handler_01(); return; }
        if ((s_cellv[0x72] < s_cell1[1]) || (s_cellv[0x72] < s_cell1[2])) {
            *s_tmr = 0;
        } else {
            uint16_t cnt = (uint16_t)(*s_tmr + 1);
            *s_tmr = cnt;
            if (((*(volatile uint16_t *)(s_cellv + 0x74) / 100) & 0xFFFF) <= cnt) {
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

void state_timer_12(void)
{
    extern void fg_scan(void);
    fg_scan();
    volatile uint8_t *s_state = (volatile uint8_t *)0x200015C0;
    volatile uint32_t *s_flags = (volatile uint32_t *)0x200015C4;
    volatile uint32_t *s_cfg   = (volatile uint32_t *)0x200015C8;
    volatile uint32_t *s_fault = (volatile uint32_t *)0x200015CC;
    volatile uint32_t *s_aux   = (volatile uint32_t *)0x200015D0;
    volatile uint32_t *s_cnt   = (volatile uint32_t *)0x200015D4;
    volatile uint32_t *s_thr   = (volatile uint32_t *)0x200015D8;
    volatile uint32_t *s_tmr   = (volatile uint32_t *)0x200015E4;
    volatile uint8_t  *s_cell1 = (volatile uint8_t  *)0x200015DC;
    volatile uint8_t  *s_cellv = (volatile uint8_t  *)0x200015E0;

    if (*s_state != 0) { state_handler_11(); return; }

    if ((*s_flags & 1) != 0) {
        *s_flags &= ~1U;
        if (((*s_cfg >> 5) & 1) != 0) {
            *s_cfg &= ~0x20U;
            if (((*s_fault >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_fault & 1) != 0) { state_handler_07(); return; }
            if (((*s_fault >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_fault >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_fault >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        extern void cell_balance_update(void);
        cell_balance_update();
    }

    if (((*s_flags >> 2) & 1) != 0) {
        *s_flags &= ~4U;
        fg_alert_monitor();
        fg_discharge_oc_check();
        if ((*s_aux != 0) &&
            ((((*s_aux >> 5) & 1) || ((*s_aux >> 6) & 1) || ((*s_aux >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        if (*s_cnt <= *s_thr) { state_handler_01(); return; }
        if ((s_cell1[1] < s_cellv[0x7A]) || (s_cell1[2] < s_cellv[0x7A])) {
            *s_tmr = 0;
        } else {
            uint16_t cnt = (uint16_t)(*s_tmr + 1);
            *s_tmr = cnt;
            if (((*(volatile uint16_t *)(s_cellv + 0x7C) / 100) & 0xFFFF) <= cnt) {
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

void state_timer_13(void)
{
    extern void fg_scan(void);
    fg_scan();
    volatile uint8_t *s_state = (volatile uint8_t *)0x20001870;
    volatile uint32_t *s_flags = (volatile uint32_t *)0x20001874;
    volatile uint32_t *s_cfg   = (volatile uint32_t *)0x20001878;
    volatile uint32_t *s_fault = (volatile uint32_t *)0x2000187C;
    volatile uint32_t *s_aux   = (volatile uint32_t *)0x20001880;
    volatile uint32_t *s_cnt   = (volatile uint32_t *)0x20001884;
    volatile uint32_t *s_thr   = (volatile uint32_t *)0x20001888;
    volatile uint32_t *s_cur   = (volatile uint32_t *)0x2000188C;
    volatile uint32_t *s_gpio  = (volatile uint32_t *)0x50000400;

    if (*s_state != 0) { state_handler_11(); return; }

    if ((*s_flags & 1) != 0) {
        *s_flags &= ~1U;
        if (((*s_cfg >> 5) & 1) != 0) {
            *s_cfg &= ~0x20U;
            if (((*s_fault >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_fault & 1) != 0) { state_handler_07(); return; }
            if (((*s_fault >> 5) & 1) != 0) { state_handler_0c(); return; }
            if (((*s_fault >> 4) & 1) != 0) { state_handler_0b(); return; }
            if (((*s_fault >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        extern void cell_balance_update(void);
        cell_balance_update();
    }

    if (((*s_flags >> 2) & 1) != 0) {
        *s_flags &= ~4U;
        fg_uvp1_check(); fg_uvp2_check(); fg_ovp1_check(); fg_ovp2_check();
        fg_threshold_check(); fg_alert_monitor();

        if (*s_aux != 0) {
            if ((*s_aux & 1) != 0) { state_handler_12(); return; }
            if (((*s_aux >> 1) & 1) != 0) { state_handler_13(); return; }
            if (((*s_aux >> 2) & 1) != 0) { state_handler_14(); return; }
            if (((*s_aux >> 3) & 1) != 0) { state_handler_15(); return; }
            if (((*s_aux >> 4) & 1) != 0) { state_handler_16(); return; }
            if ((((*s_aux >> 5) & 1) || ((*s_aux >> 6) & 1) || ((*s_aux >> 7) & 1))) {
                state_handler_17_19(); return;
            }
        }

        if (*s_cnt <= *s_thr) {
            if (((*s_fault >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_fault >> 2) & 1) != 0) { state_handler_09(); return; }
            state_handler_01(); return;
        }

        if (*s_cur < *(volatile uint16_t *)((uint8_t *)s_cur - 0x18 + 0x16)) {
            if (((*s_cfg >> 6) & 1) != 0) { *s_cfg &= ~0x40U; gpio_bit_write((uint32_t)s_gpio, 0x200, 0); }
        } else if (((*s_cfg >> 6) & 1) == 0) {
            *s_cfg |= 0x40;
            gpio_bit_write((uint32_t)s_gpio, 0x200, 1);
        }

        extern void veneer_11f68(void);
        extern void veneer_11ee8(void);
        extern void veneer_11f18(void);
        veneer_11f68(); veneer_11ee8(); veneer_11f18();
        fg_watchdog_kick();
    }
    extern void veneer_11f08(int arg);
    veneer_11f08(1);
}

void state_timer_15_a(void)
{
    extern void fg_scan(void);
    fg_scan();
    volatile uint8_t *s_state = (volatile uint8_t *)0x20001B24;
    volatile uint32_t *s_flags = (volatile uint32_t *)0x20001B28;
    volatile uint32_t *s_cfg   = (volatile uint32_t *)0x20001B2C;
    volatile uint32_t *s_fault = (volatile uint32_t *)0x20001B30;
    volatile uint32_t *s_aux   = (volatile uint32_t *)0x20001B34;
    volatile uint32_t *s_cur   = (volatile uint32_t *)0x20001B38;
    volatile uint32_t *s_thr   = (volatile uint32_t *)0x20001B3C;
    volatile uint32_t *s_tmr   = (volatile uint32_t *)0x20001B40;

    if (*s_state != 0) { state_handler_11(); return; }

    if ((*s_flags & 1) != 0) {
        *s_flags &= ~1U;
        if (((*s_cfg >> 5) & 1) != 0) {
            *s_cfg &= ~0x20U;
            if (((*s_fault >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_fault & 1) != 0) { state_handler_07(); return; }
            if (((*s_fault >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_fault >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_fault >> 6) & 1) == 0) { state_handler_01(); return; }
            if (((*s_fault >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        extern void cell_balance_update(void);
        cell_balance_update();
    }

    if (((*s_flags >> 2) & 1) != 0) {
        *s_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check(); fg_charge_oc_check();
        if ((*s_aux != 0) &&
            ((((*s_aux >> 5) & 1) || ((*s_aux >> 6) & 1) || ((*s_aux >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        if (*s_thr < *s_cur) {
            uint16_t cnt = (uint16_t)(*s_tmr + 1);
            *s_tmr = cnt;
            if (cnt > 9) { *s_tmr = 0; state_handler_02(); return; }
        } else { *s_tmr = 0; }
        extern void veneer_11f68(void);
        extern void veneer_11f88(void);
        extern void veneer_11f18(void);
        veneer_11f68(); veneer_11f88(); veneer_11f18();
        fg_watchdog_kick();
    }
    extern void veneer_11f08(int arg);
    veneer_11f08(0);
}

void state_timer_0d(void)
{
    extern void fg_scan(void);
    fg_scan();
    volatile uint8_t *s_state = (volatile uint8_t *)0x20001D20;
    volatile uint32_t *s_flags = (volatile uint32_t *)0x20001D24;
    volatile uint32_t *s_cfg   = (volatile uint32_t *)0x20001D28;
    volatile uint32_t *s_fault = (volatile uint32_t *)0x20001D2C;
    volatile uint32_t *s_aux   = (volatile uint32_t *)0x20001D30;
    volatile uint32_t *s_cur   = (volatile uint32_t *)0x20001D34;
    volatile uint32_t *s_thr   = (volatile uint32_t *)0x20001D38;
    volatile uint32_t *s_tmr   = (volatile uint32_t *)0x20001D3C;

    if (*s_state != 0) { state_handler_11(); return; }

    if ((*s_flags & 1) != 0) {
        *s_flags &= ~1U;
        if (((*s_cfg >> 5) & 1) != 0) {
            *s_cfg &= ~0x20U;
            if (((*s_fault >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_fault & 1) != 0) { state_handler_07(); return; }
            if (((*s_fault >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_fault >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_fault >> 7) & 1) == 0) { state_handler_01(); return; }
            if (((*s_fault >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        extern void cell_balance_update(void);
        cell_balance_update();
    }

    if (((*s_flags >> 2) & 1) != 0) {
        *s_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check(); fg_charge_oc_check();
        if ((*s_aux != 0) &&
            ((((*s_aux >> 5) & 1) || ((*s_aux >> 6) & 1) || ((*s_aux >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        if (*s_thr < *s_cur) {
            uint16_t cnt = (uint16_t)(*s_tmr + 1);
            *s_tmr = cnt;
            if (cnt > 9) { *s_tmr = 0; state_handler_02(); return; }
        } else { *s_tmr = 0; }
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
    volatile uint8_t *s_state = (volatile uint8_t *)0x20001F30;
    volatile uint32_t *s_flags = (volatile uint32_t *)0x20001F34;
    volatile uint32_t *s_cfg   = (volatile uint32_t *)0x20001F38;
    volatile uint32_t *s_fault = (volatile uint32_t *)0x20001F3C;
    volatile uint32_t *s_aux   = (volatile uint32_t *)0x20001F40;
    volatile uint32_t *s_tmr   = (volatile uint32_t *)0x20001F4C;
    volatile uint8_t  *s_cell  = (volatile uint8_t  *)0x20001F44;
    volatile uint8_t  *s_cellv = (volatile uint8_t  *)0x20001F48;

    if (*s_state != 0) { state_handler_11(); return; }

    if ((*s_flags & 1) != 0) {
        *s_flags &= ~1U;
        if (((*s_cfg >> 5) & 1) != 0) {
            *s_cfg &= ~0x20U;
            if (((*s_fault >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_fault & 1) != 0) { state_handler_07(); return; }
            if (((*s_fault >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_fault >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_fault >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        extern void cell_balance_update(void);
        cell_balance_update();
    }

    if (((*s_flags >> 2) & 1) != 0) {
        *s_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check(); fg_charge_oc_check();
        if ((*s_aux != 0) &&
            ((((*s_aux >> 5) & 1) || ((*s_aux >> 6) & 1) || ((*s_aux >> 7) & 1)))) {
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
    fg_scan();
    volatile uint8_t *s_state = (volatile uint8_t *)0x20002120;
    volatile uint32_t *s_flags = (volatile uint32_t *)0x20002124;
    volatile uint32_t *s_cfg   = (volatile uint32_t *)0x20002128;
    volatile uint32_t *s_fault = (volatile uint32_t *)0x2000212C;
    volatile uint32_t *s_aux   = (volatile uint32_t *)0x20002130;
    volatile uint32_t *s_tmr   = (volatile uint32_t *)0x2000213C;
    volatile uint8_t  *s_cell  = (volatile uint8_t  *)0x20002134;
    volatile uint8_t  *s_cellv = (volatile uint8_t  *)0x20002138;

    if (*s_state != 0) { state_handler_11(); return; }

    if ((*s_flags & 1) != 0) {
        *s_flags &= ~1U;
        if (((*s_cfg >> 5) & 1) != 0) {
            *s_cfg &= ~0x20U;
            if (((*s_fault >> 1) & 1) != 0) { state_handler_08(); return; }
            if ((*s_fault & 1) != 0) { state_handler_07(); return; }
            if (((*s_fault >> 3) & 1) != 0) { state_handler_0a(); return; }
            if (((*s_fault >> 2) & 1) != 0) { state_handler_09(); return; }
            if (((*s_fault >> 11) & 1) != 0) { state_handler_17_19(); return; }
        }
        extern void cell_balance_update(void);
        cell_balance_update();
    }

    if (((*s_flags >> 2) & 1) != 0) {
        *s_flags &= ~4U;
        fg_alert_monitor(); fg_discharge_oc_check(); fg_charge_oc_check();
        if ((*s_aux != 0) &&
            ((((*s_aux >> 5) & 1) || ((*s_aux >> 6) & 1) || ((*s_aux >> 7) & 1)))) {
            state_handler_17_19(); return;
        }
        if ((s_cellv[0x8A] <= s_cell[1]) && (s_cellv[0x8A] <= s_cell[2])) {
            uint16_t cnt = (uint16_t)(*s_tmr + 1);
            *s_tmr = cnt;
            if (((*(volatile uint16_t *)(s_cellv + 0x8C) / 100) & 0xFFFF) <= cnt) {
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

/*
 * Master BMS state machine — main periodic dispatch.
 */
void bms_state_machine(void)
{
    extern void fg_scan(void);
    fg_scan();
    volatile uint8_t *s_state = (volatile uint8_t *)0x20002488;
    volatile uint32_t *s_flags = (volatile uint32_t *)0x2000248C;
    volatile uint32_t *s_cfg   = (volatile uint32_t *)0x20002490;
    volatile uint32_t *s_fault = (volatile uint32_t *)0x20002494;
    volatile uint32_t *s_aux   = (volatile uint32_t *)0x20002498;

    if (*s_state != 0) { state_handler_11(); nop_2bac(); }

    if ((*s_flags & 1) != 0) {
        *s_flags &= ~1U;
        if (((*s_cfg >> 5) & 1) != 0) {
            *s_cfg &= ~0x20U;
            if (((*s_fault >> 1) & 1) != 0) { state_handler_08(); nop_2bac(); }
            if ((*s_fault & 1) != 0)      { state_handler_07(); nop_2bac(); }
            if (((*s_fault >> 3) & 1) != 0) { state_handler_0a(); nop_2bac(); }
            if (((*s_fault >> 2) & 1) != 0) { state_handler_09(); nop_2bac(); }
            if (((*s_fault >> 7) & 1) != 0) { state_handler_0e(); nop_2bac(); }
            if (((*s_fault >> 6) & 1) != 0) { state_handler_0d(); nop_2bac(); }
            if (((*s_fault >> 11) & 1) != 0){ state_handler_17_19(); nop_2bac(); }
        }
        extern void cell_balance_update(void);
        cell_balance_update();
    }

    if (((*s_flags >> 2) & 1) == 0) { nop_2ba6(); }
    *s_flags &= ~4U;

    fg_ovp1_check(); fg_ovp2_check(); fg_threshold_check();
    fg_alert_monitor(); fg_discharge_oc_check();

    if (*s_aux != 0) {
        if (((*s_aux >> 2) & 1) != 0) { state_handler_14(); nop_2bac(); }
        if (((*s_aux >> 3) & 1) != 0) { state_handler_15(); nop_2bac(); }
        if (((*s_aux >> 4) & 1) != 0) { state_handler_16(); nop_2bac(); }
        if ((((*s_aux >> 5) & 1) || ((*s_aux >> 6) & 1) || ((*s_aux >> 7) & 1))) {
            state_handler_17_19(); nop_2bac();
        }
    }

    /* response chain and charge/discharge MOSFET control */
    extern void veneer_11f68(void);
    extern void veneer_11f88(void);
    extern void veneer_11ee8(void);
    veneer_11f68(); veneer_11f88(); veneer_11ee8();

    /* charge/discharge voltage threshold checks and MOSFET toggling */
    volatile uint32_t *s_cur   = (volatile uint32_t *)0x2000249C;
    volatile uint32_t *s_thr   = (volatile uint32_t *)0x200024A0;
    volatile uint32_t *s_cell1 = (volatile uint32_t *)0x200024A8;
    volatile uint32_t *s_cellv = (volatile uint32_t *)0x200024AC;
    volatile uint32_t *s_tmr_a = (volatile uint32_t *)0x200024B0;
    volatile uint32_t *s_tmr_b = (volatile uint32_t *)0x200024A4;
    volatile uint32_t *s_gpio  = (volatile uint32_t *)0x50000400;

    if ((*s_thr < *s_cur) && (((*s_cfg >> 8) & 1) != 0)) {
        if ((*s_aux & 1) == 0) {
            *s_tmr_b = 0;
            if (*(volatile uint8_t *)(s_cell1 + 1/4) < *(volatile uint8_t *)((uint8_t *)s_cellv + 0x6E) &&
                *(volatile uint8_t *)(s_cell1 + 2/4) < *(volatile uint8_t *)((uint8_t *)s_cellv + 0x6E)) {
                *s_tmr_a = 0;
            } else {
                uint16_t cnt = *(volatile uint16_t *)s_tmr_a + 1;
                *(volatile uint16_t *)s_tmr_a = cnt;
                if (((*(volatile uint16_t *)((uint8_t *)s_cellv + 0x70) / 100) & 0xFFFF) <= cnt) {
                    charge_mosfet_set(false); discharge_mosfet_set(false);
                    bms_set_state(0x12);
                    volatile uint32_t *s_cfg2 = (volatile uint32_t *)0x200024B8;
                    *s_cfg2 = 3;
                    *s_aux |= 1;
                    gpio_bit_write((uint32_t)s_gpio, 1, 1);
                    *s_cfg &= ~0x10U;
                    *s_tmr_a = 0;
                }
            }
        }
    }

    extern void veneer_11f18(void);
    veneer_11f18();
    fg_watchdog_kick();
    extern void veneer_11f08(int arg);
    veneer_11f08(1);
}
