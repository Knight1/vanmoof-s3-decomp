/* main.c — OEM `main` at flash address 0x080042D6 (size ≈ 484 B).
 *
 * Speculative pre-decomp version lives in `main.c.bak`.
 *
 * Shape of the OEM main:
 *   1. Copy 0xC0 bytes of .data from flash 0x08004828 to SRAM 0x20000000.
 *   2. Enable SYSCFG (RCC_APB2ENR bit 0) and set NVIC priority via
 *      `FUN_080052E8(3)`.
 *   3. `cpsie i` to enable IRQs.
 *   4. Three init thunks (`FUN_0800428E`, `FUN_080041C6`, `FUN_080040B2`)
 *      bring up the rest of the peripherals (TIM / ADC / etc., not yet
 *      decomp'd).
 *   5. `uart1_init(9600)`, then RCC AHBENR bit 6 (likely CRC clock).
 *   6. A boot-time hash/check pair: `FUN_08005D40` over the .data at
 *      `*(uint32_t*)0x20000148` for 100,000 bytes, then `FUN_08004048`
 *      applies the result to a status field.
 *   7. A two-stage `FUN_080033CC` check that latches the state byte
 *      at `0x200000FC` to 1 or 2 and gates the operating mode at
 *      `0x20000115` and `0x2000013B`.
 *   8. Infinite super-loop:
 *       a. `r4 = FUN_080035BE()` — returns a small index used by the
 *          round-robin switch below.
 *       b. `modbus_rx_poll()` — drain UART RX scratch into PDU buffer
 *          and possibly dispatch.
 *       c. `FUN_080036D4(G_5A_TARGET)` — periodic per-byte action.
 *       d. If a SysTick fired (tick_a != tick_b), increment `tick_a`
 *          and run the round-robin switch in `r4`. Cases 0..6 each
 *          stamp a different task-ID into `0x20000118` and may call
 *          `FUN_08003608` / `FUN_080033E2` depending on flag bytes.
 *          (Full case-by-case breakdown is in
 *           `docs/progress.md` under the main() entry; not all
 *           helpers are decomp'd yet.)
 *       e. Post-switch: if the 5C-busy latch at `0x2000013C` is set,
 *          wait 0x32 ticks then fire `FUN_080031E6` (the 5C consumer
 *          stubbed in modbus_dispatch.c).
 *       f. If mode (`0x20000139`) == 0 and `tick_b - prev_tick_b ==
 *          2000`, roll over the tick counters.
 *       g. Snapshot `tick_b` for the next iteration.
 *
 * Many of the helpers above are still `FUN_xxxxxxxx` in Ghidra. To keep
 * the link clean while the decomp is in progress, this file defines
 * each unresolved helper as a `for (;;)` trap stub. As each helper
 * lands in its proper module, the corresponding stub here is deleted
 * and replaced with an `extern` to the real symbol. The trap shape is
 * deliberately diagnostic: any unintended call lands in a tight loop
 * a debugger can observe.
 */

#include "uart.h"
#include "modbus.h"
#include "util.h"
#include "gpio.h"
#include "mm32f031.h"
#include <stdint.h>

/* ---- raw RAM globals (literal-pool resolved) ---------------------- */

#define G_SRAM_BASE         ((void *)0x20000000u)
#define DATA_INIT_SRC       ((const void *)0x08004828u)
#define DATA_INIT_LEN       0xC0u

#define G_HASH_SEED_PTR     (*(volatile uint32_t *)0x20000148u)
#define G_HASH_LEN          100000u  /* 0x186A0 — matches OEM literal */

#define G_STATE_FC          (*(volatile uint8_t  *)0x200000FCu)
#define G_STATE_115         (*(volatile uint8_t  *)0x20000115u)
#define G_STATE_13B         (*(volatile uint8_t  *)0x2000013Bu)

#define G_TICK_A            (*(volatile uint32_t *)0x20000104u)
#define G_TICK_B            (*(volatile uint32_t *)0x200000D0u)
#define G_TICK_PREV_B       (*(volatile uint32_t *)0x20000108u)
#define G_TICK_D4           (*(volatile uint32_t *)0x200000D4u)

#define G_5A_TARGET         (*(volatile uint8_t  *)0x200000EAu)
#define G_MOTION_REACHED    (*(volatile uint8_t  *)0x2000013Au) /* 1 = position-sensor latched motion-complete */
#define G_TASK_ID           (*(volatile uint8_t  *)0x20000118u)
#define G_FLAG_13E          (*(volatile uint8_t  *)0x2000013Eu)
#define G_FLAG_13D          (*(volatile uint8_t  *)0x2000013Du)
#define G_FLAG_117          (*(volatile uint8_t  *)0x20000117u)
#define G_FLAG_116          (*(volatile uint8_t  *)0x20000116u)

#define G_5C_BUSY           (*(volatile uint8_t  *)0x2000013Cu)
#define G_5C_DEADLINE_BASE  (*(volatile uint32_t *)0x20000110u)
#define G_5C_LATCH_BYTE     (*(volatile uint8_t  *)0x20000131u)  /* 0x20000130 + 1 */
#define G_MOTOR_RUNNING     (*(volatile uint8_t  *)0x20000139u)  /* 1 = H-bridge driving, 0 = braked/idle */
#define G_MOTOR_RUN_START   (*(volatile uint32_t *)0x2000010Cu)  /* G_TICK_B at the moment the motor was last energised */
#define G_MOTOR_RUN_LATCH   (*(volatile uint8_t  *)0x20000130u)  /* 1 once G_MOTOR_RUN_START has been captured this run */

/* Drive-direction byte. Written by the round-robin task helpers
 * (`sched_task_alpha/beta/extra`, `FUN_08003538`) when they queue a
 * shift, valued `0xF0` for forward or `0x0F` for reverse so it mirrors
 * the H-bridge mask. Consumed by `pos_encoder_tick` to know which way
 * to tick `G_STATE_115` (the gear-position counter) on each PA0 edge.
 * Cleared by `state_flags_reset` once the task finishes.
 */
#define G_DRIVE_DIR         (*(volatile uint8_t  *)0x20000114u)

#define RX_TICK_ROLLOVER    2000u   /* OEM literal: movs r1,#0x7d; lsls r1,#4 → 0x7D0 = 2000 */
#define SCHED_5C_WAIT_TICKS 0x32u   /* ~50 ticks before firing the 5C consumer */

/* RCC bits used by main(). */
#define RCC_APB2_SYSCFGEN   (1u << 0)
#define RCC_AHB_CRC_BIT     (1u << 6)

/* ---- forward decls + trap stubs for not-yet-decomp'd OEM helpers --- */

/* Each trap stub corresponds to a real OEM function whose decomp
 * hasn't landed yet. Comment after each is the OEM address. */

#define TRAP_VOID()  do { for (;;) { /* unimplemented */ } } while (0)
#define TRAP_RET(x)  do { for (;;) { /* unimplemented */ } __builtin_unreachable(); } while (0)

static void boot_init_periphs_a(void)            { TRAP_VOID(); } /* OEM @ 0x0800428E (72 B) */
static void boot_init_periphs_b(void)            { TRAP_VOID(); } /* OEM @ 0x080041C6 (88 B) */
static void boot_init_periphs_c(void)            { TRAP_VOID(); } /* OEM @ 0x080040B2 (126 B) */
static void set_nvic_priority(int p)             { (void)p; TRAP_VOID(); } /* OEM @ 0x080052E8 (24 B) */

static int  boot_hash(const void *p, uint32_t n) { (void)p; (void)n; TRAP_RET(0); } /* OEM @ 0x08005D40 (44 B) */
static void boot_apply_hash(uint16_t v, int t)   { (void)v; (void)t; TRAP_VOID(); } /* OEM @ 0x08004048 (96 B) */

/* OEM @ 0x080033CC and 0x0800325C are `input_pa1` / `input_pa0` in
 * gpio.c — thin wrappers over `gpio_idr_test`. Calls below go direct
 * to those instead of through a sched_ alias. */

/* OEM @ 0x080035BE (74 B). Return the current state-machine value at
 * G_STATE_FC, clamped to 6 (the highest valid round-robin case). The
 * OEM source is a cascaded compare/return chain — clipped to its
 * observable semantics. */
static uint8_t sched_pick_task(void)
{
    const uint8_t v = G_STATE_FC;
    return v > 6u ? 6u : v;
}
/* OEM @ 0x08003288 (28 B). Decode the drive-direction byte at
 * `G_DRIVE_DIR` (valued `0xF0` forward, `0x0F` reverse, anything else
 * idle) into a small tri-state index (0 / 1 / 2). Used exclusively
 * by `pos_encoder_tick` below. */
static uint32_t drive_dir_code(void)
{
    const uint8_t v = G_DRIVE_DIR;
    if (v == 0xF0u) return 0u;
    if (v == 0x0Fu) return 1u;
    return 2u;
}

/* OEM @ 0x08003272 (22 B). Detect a level change on PA0 since the
 * mirror byte `G_STATE_13B` (`0x2000013B`) was last refreshed.
 * `main()` seeds the mirror once before entering the super-loop;
 * thereafter `pos_encoder_tick` is responsible for re-syncing it on
 * each detected edge. */
static bool pa0_changed(void)
{
    return (uint8_t)input_pa0() != G_STATE_13B;
}

/* OEM @ 0x080032A4 (86 B). Position-encoder tick: when PA0 toggles,
 * advance the gear-position counter `G_STATE_115` in whichever
 * direction the active task has queued via `G_DRIVE_DIR`. The
 * direction byte mirrors the H-bridge mask (`0xF0` → decrement,
 * `0x0F` → increment); any other value leaves the counter untouched
 * but the mirror at `G_STATE_13B` is still re-synced so the next
 * edge is timed from the new level. The motor-run latch is also
 * cleared on a counted edge so `motor_h_bridge_set` re-snapshots
 * `G_MOTOR_RUN_START` from this tick onward — each detected edge
 * effectively resets the stall-timeout window. Called from
 * `motor_h_bridge_set` immediately after the forward/reverse half
 * of the bridge is energised. */
static void pos_encoder_tick(void)
{
    const uint32_t dir = drive_dir_code();
    if (!pa0_changed()) return;

    int8_t gear = (int8_t)G_STATE_115;
    if (dir == 0u) {
        gear = gear - 1;
        G_MOTOR_RUN_LATCH = 0u;
    } else if (dir == 1u) {
        gear = gear + 1;
        G_MOTOR_RUN_LATCH = 0u;
    }
    G_STATE_115  = (uint8_t)gear;
    G_STATE_13B  = (uint8_t)input_pa0();
}

/* OEM @ 0x080032FA (210 B). H-bridge mask driver.
 *
 * Three bridge configurations are recognised — anything else is a
 * no-op for the GPIOs:
 *   `0x0F` → PA9 LOW,  PA10 HIGH — drive "reverse" half
 *   `0xF0` → PA9 HIGH, PA10 LOW  — drive "forward" half
 *   `0xFF` → PA9 HIGH, PA10 HIGH — brake (both halves high)
 *
 * The drive cases additionally kick `motor_aux_kick` (advances the
 * gear-position counter) and set `G_MOTOR_RUNNING = 1`. The brake
 * case clears `G_MOTOR_RUNNING = 0`.
 *
 * Once the motor is running, every call enforces a stall timeout:
 * `G_TICK_B` is snapshotted into `G_MOTOR_RUN_START` on the first
 * call after energising, then on subsequent calls the elapsed
 * tick count is compared against a per-task limit (200 ticks for
 * round-robin task #2, 2000 ticks otherwise). Exceeding the limit
 * sets `G_MOTION_REACHED = 1`, which `motor_drive_step` will see
 * on the next iteration and use to brake + latch arrival.
 */
static void motor_h_bridge_set(uint8_t mask)
{
    void *const gpioa = (void *)0x48000000u;
    if (mask == 0x0Fu) {
        gpio_brr_write (gpioa, 1u << 9);   /* PA9 LOW  */
        gpio_bsrr_write(gpioa, 1u << 10);  /* PA10 HIGH */
        pos_encoder_tick();
        G_MOTOR_RUNNING = 1u;
    } else if (mask == 0xF0u) {
        gpio_bsrr_write(gpioa, 1u << 9);   /* PA9 HIGH */
        gpio_brr_write (gpioa, 1u << 10);  /* PA10 LOW  */
        pos_encoder_tick();
        G_MOTOR_RUNNING = 1u;
    } else if (mask == 0xFFu) {
        gpio_bsrr_write(gpioa, 1u << 9);   /* PA9 HIGH */
        gpio_bsrr_write(gpioa, 1u << 10);  /* PA10 HIGH (brake) */
        G_MOTOR_RUNNING = 0u;
    }

    if (G_MOTOR_RUNNING == 1u) {
        if (G_MOTOR_RUN_LATCH == 0u) {
            G_MOTOR_RUN_START = G_TICK_B;
            G_MOTOR_RUN_LATCH = 1u;
        }
        const uint32_t elapsed = G_TICK_B - G_MOTOR_RUN_START;
        const uint32_t limit   = (G_TASK_ID == 2u) ? 199u : 1999u;
        if (elapsed > limit) {
            G_MOTION_REACHED = 1u;
        }
    }
}

/* OEM @ 0x0800315E (26 B). Clear the shared per-task flag bytes once
 * the active state-task is done. Called by `motor_drive_step` at
 * arrival, `cmd_5c_consume` at end of its 3-byte register write, and
 * the three round-robin task helpers (`sched_task_alpha`,
 * `sched_task_beta`, `FUN_08003538`) at their tails. Bytes cleared:
 * `G_DRIVE_DIR`, `G_MOTION_REACHED`, `G_FLAG_13D`, `G_FLAG_13E`, and
 * both bytes of the motor-run latch / 5C latch pair at
 * `0x20000130`/`0x20000131`. */
static void state_flags_reset(void)
{
    G_DRIVE_DIR      = 0u;
    G_MOTION_REACHED = 0u;
    G_FLAG_13D       = 0u;
    G_FLAG_13E       = 0u;
    G_5C_LATCH_BYTE   = 0u;
    G_MOTOR_RUN_LATCH = 0u;
}

/* OEM @ 0x080036D4 (74 B). Per-iteration motor servoing step.
 *
 * The inbound shift command (`G_5A_TARGET`, set by Modbus cmd 0x5A)
 * tells us which direction to drive — 0 = forward bank, 1 = reverse
 * bank, anything else = leave the bridge as-is. Then we poll the
 * "motion-reached" latch from the position sensor: as soon as it
 * fires and we haven't already arrived (`G_5A_TARGET != 2`), we
 * brake the motor (mask 0xFF), latch arrived, demote the running
 * state-task if it was 2 → 1, and emit a motion-done report.
 */
static void motor_drive_step(uint8_t target)
{
    if (target == 0u) {
        motor_h_bridge_set(0xF0u);
    } else if (target == 1u) {
        motor_h_bridge_set(0x0Fu);
    }

    if (G_MOTION_REACHED == 1u && G_5A_TARGET != 2u) {
        G_5A_TARGET = 2u;
        motor_h_bridge_set(0xFFu);
        if (G_TASK_ID == 2u) {
            G_STATE_FC = 1u;
        }
        state_flags_reset();
    }
}
static void    sched_default_post(void)          { TRAP_VOID(); } /* OEM @ 0x080036BA (26 B) */
static void    sched_task_alpha(void)            { TRAP_VOID(); } /* OEM @ 0x08003608 (178 B) */
static void    sched_task_beta(void)             { TRAP_VOID(); } /* OEM @ 0x080033E2 (196 B) */
static void    sched_task_extra(void)            { TRAP_VOID(); } /* OEM @ 0x080034A6 (60 B) */
static void    sched_5c_consume(void)            { TRAP_VOID(); } /* OEM @ 0x080031E6 (118 B) — also stubbed in modbus_dispatch.c */

/* ---- round-robin task dispatch --------------------------------------
 *
 * The OEM emits this as a GCC `__gnu_thumb1_case_uqi` jump table on
 * `task`. Cases 0..6 each set `G_TASK_ID` to a constant and may call
 * `sched_task_alpha` / `sched_task_beta` depending on per-case flag
 * bytes. Case 0 (`task == 0`) is the bus-quiescent default — it sets
 * no task ID, runs `sched_default_post`, and skips straight to the
 * loop epilogue. Cases ≥ 7 are out of range and unhandled.
 */
static void sched_run_task(uint8_t task)
{
    switch (task) {
    case 0u:
        sched_default_post();
        return;

    case 1u:
        /* Both alpha and beta gated by per-state flags; whichever
         * fires we then advance to the extra task and latch a "ready"
         * byte (G_FLAG_13E). */
        G_TASK_ID = 7u;
        if (G_FLAG_13E == 1u) {
            sched_task_alpha();
            /* fall through to extra-task epilogue */
        } else if (G_FLAG_13D == 1u) {
            sched_task_beta();
            return;
        }
        /* The "extra" branch: if alpha ran or both flags are clear and
         * the latched byte at G_FLAG_117 differs from a snapshot at
         * G_FLAG_116, run sched_task_extra and re-latch G_FLAG_13E. */
        if (G_FLAG_117 != G_FLAG_116 && G_TASK_ID != 0u) {
            G_FLAG_116 = G_FLAG_117;
            sched_task_extra();
            G_FLAG_13E = 1u;
        }
        return;

    case 2u:
        G_TASK_ID = 2u;
        G_5A_TARGET = 1u;
        return;

    case 3u:  /* missing case */
        return;

    case 4u:
        G_TASK_ID = 4u;
        if (G_FLAG_13D == 1u) sched_task_beta();
        return;

    case 5u:
        G_TASK_ID = 5u;
        if (G_FLAG_13D == 1u) sched_task_beta();
        return;

    case 6u:
        G_TASK_ID = 8u;
        return;

    default:
        return;
    }
}

/* ---- main ----------------------------------------------------------- */

int main(void)
{
    /* .data init (copy from flash to SRAM). */
    memcpy(G_SRAM_BASE, DATA_INIT_SRC, DATA_INIT_LEN);

    /* RCC + NVIC + IRQs */
    RCC->APB2ENR |= RCC_APB2_SYSCFGEN;
    set_nvic_priority(3);
    __asm__ volatile ("cpsie i");

    /* Per-peripheral bring-up. */
    boot_init_periphs_a();
    boot_init_periphs_b();
    uart1_init(9600u);
    RCC->AHBENR |= RCC_AHB_CRC_BIT;
    boot_init_periphs_c();

    /* Boot-time integrity check. */
    {
        const int h = boot_hash((const void *)G_HASH_SEED_PTR, G_HASH_LEN);
        boot_apply_hash((uint16_t)((unsigned)h - 1u), 99);
    }

    /* Pre-loop state-machine sync. PA1 gates a 2→1 demotion and the
     * latch-into-2 path; PA0's level is mirrored into G_STATE_13B. */
    if (G_STATE_FC == 2u && input_pa1()) {
        G_STATE_FC = 1u;
    }
    if (G_STATE_115 != 1u && !input_pa1()) {
        G_STATE_115 = 1u;
        G_STATE_FC  = 2u;
    }
    G_STATE_13B = (uint8_t)input_pa0();

    /* Super-loop. */
    for (;;) {
        const uint8_t task = sched_pick_task();
        modbus_rx_poll();
        motor_drive_step(G_5A_TARGET);

        if (G_TICK_A != G_TICK_B) {
            G_TICK_A = G_TICK_A + 1u;
            sched_run_task(task);
        }

        if (G_5C_BUSY == 1u) {
            if (G_5C_LATCH_BYTE == 0u) {
                G_5C_DEADLINE_BASE = G_TICK_B;
                G_5C_LATCH_BYTE    = 1u;
            }
            if (G_TICK_B - G_5C_DEADLINE_BASE == SCHED_5C_WAIT_TICKS) {
                sched_5c_consume();
                G_5C_LATCH_BYTE = 0u;
            }
        }

        if (G_MOTOR_RUNNING == 0u && (G_TICK_B - G_TICK_PREV_B == RX_TICK_ROLLOVER)) {
            G_TICK_B      = 0u;
            G_TICK_D4     = 0u;
            G_TICK_A      = 0u;
        }
        G_TICK_PREV_B = G_TICK_B;
    }
}
