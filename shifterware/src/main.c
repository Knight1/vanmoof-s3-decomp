/* main.c — OEM `main` at flash address 0x080042D6 (size ≈ 484 B).
 *
 * Speculative pre-decomp version lives in `main.c.bak`.
 *
 * Shape of the OEM main:
 *   1. Copy 0xC0 bytes of .data from flash 0x08004828 to SRAM 0x20000000.
 *   2. Enable SYSCFG (RCC_APB2ENR bit 0) and call
 *      `syscfg_set_mem_mode(3)` to slam the MEM_MODE field of
 *      `SYSCFG_CFGR1` (boot memory remap; value 3 is MM32-specific).
 *   3. `cpsie i` to enable IRQs.
 *   4. Three init thunks (`FUN_0800428E`, `FUN_080041C6`, `FUN_080040B2`)
 *      bring up the rest of the peripherals (TIM / ADC / etc., not yet
 *      decomp'd).
 *   5. `uart1_init(9600)`, then RCC AHBENR bit 6 (likely CRC clock).
 *   6. TIM2 1 kHz periodic-tick init. The OEM reads HCLK from
 *      `*(uint32_t*)0x20000148`, computes `HCLK/100000 - 1` (via the
 *      Cortex-M0 softdiv `__aeabi_uidiv` at OEM @ 0x08005D40), then
 *      calls `tim2_init_periodic` (OEM @ 0x08004048) with that
 *      prescaler and ARR = 99 — a 1 ms (1 kHz) update IRQ.
 *      (The pre-decomp model speculated this was a "boot-time hash
 *      check"; once the two functions were translated it became clear
 *      that 0x08005D40 is just `uidiv` and 0x08004048 is a TIM2
 *      configure — see commit history.)
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
#include "flash_store.h"
#include "hal.h"
#include "timer.h"
#include "mm32f031.h"
#include <stdint.h>

/* ---- raw RAM globals (literal-pool resolved) ---------------------- */

#define G_SRAM_BASE         ((void *)0x20000000u)
#define DATA_INIT_SRC       ((const void *)0x08004828u)
#define DATA_INIT_LEN       0xC0u

/* OEM literal at flash 0x20000148 holds the **HCLK frequency in Hz**,
 * not a hash seed as the speculative pre-decomp model assumed.
 * Written once during boot (the writer is not yet localised — the
 * candidates are `boot_init_systick` at `0x0800428E` and
 * `boot_init_gpio` at `0x080041C6`, both of which run before the
 * first read by `main`'s TIM2 prescaler computation; whichever
 * stamps it does so via a literal of the actual HCLK value), then
 * read by `main` (to derive the TIM2 prescaler at `HCLK/100000-1`)
 * and by `boot_init_systick` (which divides by 1000 for the
 * SysTick reload, not the "hashes for 100,000" the speculative
 * decomp guessed). */
#define G_HCLK_HZ           (*(volatile uint32_t *)0x20000148u)
#define TIM2_TICK_BASE_HZ   100000u  /* OEM divisor: HCLK/100000 = 100 kHz timer base */
#define TIM2_TICK_PERIOD    99u      /* TIM2 ARR — IRQ every 100 base ticks = 1 ms */

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

#define G_COUNTER           (*(volatile uint32_t *)0x200000F8u)  /* cmd-0x14 monotonic counter; reset by `sched_idle_reset` */
#define G_SHIFT_ATTEMPT_CTR (*(volatile int32_t  *)0x2000011Cu)  /* per-shift retry counter; cleared on home-reset, incremented in `sched_task_beta` post-motion; ≥3 demotes G_STATE_FC to 6 */
#define G_SETTLE_TICK_BASE  (*(volatile uint32_t *)0x20000134u)  /* tick snapshot when settling started; latched by `sched_alpha_match_3538` on first call */
#define G_SETTLE_ARMED      (*(volatile uint8_t  *)0x20000138u)  /* 1 once `G_SETTLE_TICK_BASE` is valid; cleared when the settling completes */
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

/* ---- boot bring-up + scheduler tasks (all OEM-confirmed) ----------- *
 *
 * Every helper that was previously trap-stubbed has been translated.
 * Per-function OEM addresses live in each function's comment. */

/* OEM @ 0x0800428E (72 B). Classic CMSIS `SysTick_Config(HCLK/1000)`
 * pattern with a guard for HCLK > 1.024 GHz (which won't happen on
 * this part, but the OEM emits the check anyway). Bracketed by two
 * `NVIC_SetPriority(SysTick_IRQn, _)` calls — priority 3 (lowest on
 * Cortex-M0) during the LOAD/VAL/CTRL writes, then 0 (highest) once
 * the timer is running, so SysTick can't preempt anything mid-config
 * but immediately reaches the front of the queue afterwards. */
static void boot_init_systick(void)
{
    const uint32_t ticks_per_ms = G_HCLK_HZ / 1000u;
    if (ticks_per_ms - 1u > 0x00FFFFFFu) {       /* SysTick LOAD is 24-bit */
        for (;;) { /* unsupported HCLK */ }
    }
    SYSTICK->LOAD = ticks_per_ms - 1u;
    nvic_set_priority(-1, 3);                    /* SysTick_IRQn = -1 */
    SYSTICK->VAL  = 0u;
    SYSTICK->CTRL = 0x7u;                        /* CLKSRC | TICKINT | ENABLE */
    nvic_set_priority(-1, 0);
}

/* OEM @ 0x080041C6 (88 B). Boot-time GPIO pin map. Enables GPIOA +
 * GPIOB clocks together (`rcc_ahben_bits(0x60000, 1)` — AHBENR bits
 * 17 and 18), then drives the four pins we care about from
 * hardware.md:
 *   PA9, PA10 → output 50 MHz push-pull (motor H-bridge halves), then
 *               BRR them both low so the bridge is parked off at boot.
 *   PA0, PA1  → input floating (the position-sensor lines).
 *
 * USART1's PB6/PB7 are configured later by `uart1_init`, so this
 * function doesn't touch them. The `b 0x08004268` past the literal
 * pool in the asm is just a compiler-generated jump over an embedded
 * literal pool block — the function is logically contiguous. */
static void boot_init_gpio(void)
{
    rcc_ahben_bits(0x60000u, 1);                 /* IOPAEN | IOPBEN (AHBENR bits 17, 18) */

    void *const gpioa = (void *)0x48000000u;

    /* PA9, PA10: output 50 MHz push-pull (CRH nibble 0x3). */
    gpio_pin_cfg_t cfg = { .pin_mask = 0x0600u, .mode_extra = 3u, .flags = 0x10u };
    gpio_pin_configure(gpioa, &cfg);

    /* PA0, PA1: input floating (CRL nibble 0x4). */
    cfg.pin_mask   = 0x0003u;
    cfg.mode_extra = 3u;
    cfg.flags      = 0x04u;
    gpio_pin_configure(gpioa, &cfg);

    gpio_brr_write(gpioa, 0x0200u);              /* PA9 low */
    gpio_brr_write(gpioa, 0x0400u);              /* PA10 low */
}
/* OEM @ 0x080052E8 (24 B). Read-modify-write the `MEM_MODE` field
 * (bits 0..1) of `SYSCFG_CFGR1` at `0x40010000`. On the STM32F0
 * family this selects boot-time memory remap (Flash / System
 * memory / SRAM); the MM32F031 silicon may extend the encoding —
 * the OEM passes `3`, which is "reserved" per ST docs but
 * presumably has a vendor-specific meaning here. Caller (only one)
 * is `main`'s boot prologue, right after gating the SYSCFG clock on
 * in `RCC->APB2ENR`. */
static void syscfg_set_mem_mode(uint32_t mode)
{
    volatile uint32_t *const cfgr1 = (volatile uint32_t *)0x40010000u;
    *cfgr1 = (*cfgr1 & 0xFFFFFFFCu) | (mode & 0x3u);
}

/* What main.c previously called `boot_hash` (FUN_08005D40) is in fact
 * `__aeabi_uidiv` — the Cortex-M0 unsigned 32-bit softdiv (see
 * src/runtime.c). The OEM uses it here to compute the TIM2 prescaler
 * (`HCLK / 100000`) and earlier inside `boot_init_systick`
 * (`0x0800428E`) to compute `HCLK / 1000`. Both call sites are now plain `/`
 * operators in C; GCC emits the runtime call automatically.
 *
 * `boot_apply_hash` (FUN_08004048) is `tim2_init_periodic` (see
 * src/timer.c). */

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
 * arrival, `flash_settings_commit` at end of its 3-byte register write, and
 * the three round-robin task helpers (`sched_task_alpha`,
 * `sched_task_beta`, `FUN_08003538`) at their tails. Bytes cleared:
 * `G_DRIVE_DIR`, `G_MOTION_REACHED`, `G_FLAG_13D`, `G_FLAG_13E`, and
 * both bytes of the motor-run latch / 5C latch pair at
 * `0x20000130`/`0x20000131`. */
void state_flags_reset(void)
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
/* OEM @ 0x080036BA (26 B). The case-0 epilogue of `sched_run_task`,
 * fired the iteration after `G_STATE_FC` lands at 0. Resets the
 * application state machine back to its initial "home" configuration
 * — promotes `G_STATE_FC` out of state 0, zeroes the cmd-0x14 counter,
 * latches the gear-position counter back to home (1), then commits
 * the snapshot to the settings flash page via `flash_settings_commit`.
 * Only caller is main's `sched_run_task` (case 0). */
static void sched_idle_reset(void)
{
    G_STATE_FC  = 1u;
    G_COUNTER   = 0u;
    G_STATE_115 = 1u;
    flash_settings_commit();
}
/* OEM @ 0x080034A6 (60 B). The "extra" round-robin task body. Run
 * from case 1 of `sched_run_task` after `G_FLAG_116` is updated to
 * the new target (lifted from `G_FLAG_117`). Decides which way the
 * H-bridge needs to drive by comparing the *current* gear position
 * (`G_STATE_115`) to the *target* gear (`G_FLAG_116`):
 *
 *   current < target → `G_DRIVE_DIR = 0x0F` (pos_encoder_tick will
 *                       increment `G_STATE_115` on each PA0 edge)
 *   current > target → `G_DRIVE_DIR = 0xF0` (decrement on each edge)
 *   equal           → `G_DRIVE_DIR = 0` (no drive — already at target)
 *
 * The two driving branches also raise `G_FLAG_13E` so the next
 * iteration of case 1 picks `sched_task_alpha` instead of starting
 * another `sched_task_extra` cycle. */
/* Non-static so `modbus_dispatch_pdu`'s cmd=0x02 (gear-write) handler
 * can call it from a different TU. The OEM has them in one compilation
 * unit; our split into main.c + modbus_dispatch.c requires the
 * external linkage. */
void sched_task_extra(void)
{
    if (G_STATE_115 < G_FLAG_116) {
        G_DRIVE_DIR = 0x0Fu;
        G_FLAG_13E  = 1u;
    } else if (G_FLAG_116 < G_STATE_115) {
        G_DRIVE_DIR = 0xF0u;
        G_FLAG_13E  = 1u;
    } else {
        G_DRIVE_DIR = 0u;
    }
}

/* OEM @ 0x08003538 (134 B). Per-gear settling-time enforcement called
 * by `sched_task_alpha` once `G_FLAG_116 == G_STATE_115` (position
 * matched). Holds the bridge engaged for a small window so the motor
 * physically settles before braking.
 *
 * First call latches `G_TICK_B` into `G_SETTLE_TICK_BASE` and arms
 * `G_SETTLE_ARMED`. Each subsequent call compares the elapsed time
 * (`G_TICK_B - G_SETTLE_TICK_BASE`) against a per-(gear, direction)
 * threshold; once it passes, brakes the H-bridge, clears
 * `G_DRIVE_DIR` / `G_TASK_ID` / `G_SHIFT_ATTEMPT_CTR` / `G_SETTLE_ARMED`,
 * and tail-calls `state_flags_reset`.
 *
 * Thresholds (at 1 kHz tick → ms):
 *   gear 1: 23 ms (any direction)
 *   gear 2: 30 ms (only when drive_dir ∈ {0x0F, 0xF0})
 *   gear 3 reverse (0x0F): 35 ms; gear 3 forward (0xF0): 25 ms
 *   gear 4: 40 ms (any direction)
 *   anything else: 0 (settle elapses immediately) */
static void sched_alpha_match_3538(uint8_t gear, uint8_t drive_dir)
{
    if (G_SETTLE_ARMED == 0u) {
        G_SETTLE_TICK_BASE = G_TICK_B;
        G_SETTLE_ARMED     = 1u;
    }

    uint32_t threshold = 0u;
    switch (gear) {
    case 1u:
        threshold = 0x17u;                     /* 23 ms */
        break;
    case 2u:
        if (drive_dir == 0x0Fu || drive_dir == 0xF0u) {
            threshold = 0x1Eu;                 /* 30 ms */
        }
        break;
    case 3u:
        if      (drive_dir == 0x0Fu) threshold = 0x23u;  /* 35 ms */
        else if (drive_dir == 0xF0u) threshold = 0x19u;  /* 25 ms */
        break;
    case 4u:
        threshold = 0x28u;                     /* 40 ms */
        break;
    default:
        break;
    }

    if ((G_TICK_B - G_SETTLE_TICK_BASE) >= threshold) {
        motor_h_bridge_set(0xFFu);
        G_DRIVE_DIR         = 0u;
        G_SETTLE_ARMED      = 0u;
        G_TASK_ID           = 0u;
        G_SHIFT_ATTEMPT_CTR = 0;
        state_flags_reset();
    }
}

/* OEM @ 0x08003608 (178 B). The "alpha" round-robin task body. Two
 * mutually-exclusive halves, gated on `sched_pick_task()` (the clamped
 * `G_STATE_FC`):
 *
 *   - state 1: arm the cmd-0x14 follow-up — set `G_FLAG_117 = 1`,
 *     `G_FLAG_13D = 1` (which `sched_run_task` cases 4/5 use to gate
 *     `sched_task_beta`), and clear `G_FLAG_13E`. Returns immediately.
 *
 *   - state 2: the real shifter work. If the previously-seen position
 *     (`G_FLAG_116`) differs from the current gear (`G_STATE_115`),
 *     re-energise the H-bridge in the direction `G_DRIVE_DIR` is set
 *     to (0x0F reverse or 0xF0 forward; anything else: don't touch the
 *     bridge). Then poll `G_MOTION_REACHED`; if the position sensor
 *     (or the stall-timeout fallback) has fired, brake the bridge and
 *     classify by PA1 — PA1 low schedules task 6 and re-homes
 *     `G_STATE_115` to 1; PA1 high schedules task 3. Either path
 *     bumps `G_SHIFT_ATTEMPT_CTR` (shared with `sched_task_beta`);
 *     after 5 attempts the OEM demotes `G_STATE_FC` to 6. Finally,
 *     once `G_FLAG_116 == G_STATE_115`, hand off to the not-yet-
 *     decomp'd post-match helper at `0x08003538` with the current
 *     gear and drive-dir bytes.
 *
 * Note the asymmetry with `sched_task_beta`: beta uses a retry
 * threshold of 3 (`cmp ; blt #3`); alpha uses 5 (`cmp ; blt #5`).
 * Both feed the same counter slot. */
static void sched_task_alpha(void)
{
    if (sched_pick_task() == 1u) {
        G_FLAG_117 = 1u;
        G_FLAG_13D = 1u;
        G_FLAG_13E = 0u;
        return;
    }
    if (sched_pick_task() == 2u) {
        if (G_FLAG_116 != G_STATE_115) {
            if (G_DRIVE_DIR == 0x0Fu) {
                motor_h_bridge_set(0x0Fu);
            } else if (G_DRIVE_DIR == 0xF0u) {
                motor_h_bridge_set(0xF0u);
            }
            if (G_MOTION_REACHED == 1u) {
                motor_h_bridge_set(0xFFu);
                if (input_pa1() == 0u) {
                    G_TASK_ID           = 6u;
                    G_STATE_115         = 1u;
                    G_SHIFT_ATTEMPT_CTR = G_SHIFT_ATTEMPT_CTR + 1;
                } else {
                    G_TASK_ID           = 3u;
                    G_SHIFT_ATTEMPT_CTR = G_SHIFT_ATTEMPT_CTR + 1;
                }
                if (G_SHIFT_ATTEMPT_CTR >= 5) {
                    G_STATE_FC = 6u;
                }
                state_flags_reset();
            }
        }
        if (G_FLAG_116 == G_STATE_115) {
            sched_alpha_match_3538(G_STATE_115, G_DRIVE_DIR);
        }
    }
}

/* OEM @ 0x080033E2 (196 B). Round-robin shifter task body, called by
 * `sched_run_task` cases 4 and 5 (and from two more call sites in
 * `main`'s tail, currently inside the gap region we extended back into
 * `main`'s bounds on 2026-05-18). Two halves gated by the PA1 sensor:
 *
 *   - PA1 high: drive forward (G_DRIVE_DIR = 0xF0; H-bridge mask 0xF0).
 *     Then, only if `G_MOTION_REACHED == 1`, branch on the gear-position
 *     counter `G_STATE_115` to schedule the next state and pick the
 *     next task slot:
 *       gear == 4              → G_TASK_ID = 4, G_STATE_FC = 4
 *       gear in {2, 3}         → G_TASK_ID = 5, G_STATE_FC = 5
 *       gear == 1 && PA1 still → G_TASK_ID = 2, G_STATE_FC = 3
 *     Brake the H-bridge (mask 0xFF, G_DRIVE_DIR = 0xFF), bump the
 *     per-shift retry counter `G_SHIFT_ATTEMPT_CTR`, and if it reaches 3
 *     demote `G_STATE_FC` to 6 (a give-up state). Tail-call
 *     `state_flags_reset` to drop the shared task flags.
 *
 *   - PA1 low (home/sensor cleared): latch gear back to 1, brake the
 *     motor, clear drive-dir + task-id + retry counter, raise the
 *     deferred-commit flags `G_FLAG_117` and `G_5C_BUSY`, and arm
 *     `G_STATE_FC = 2` so the next super-loop iteration runs the
 *     follow-up task.
 *
 * The OEM uses a *signed* compare on the retry counter (`cmp r0,#3;
 * blt`), hence `int32_t` for `G_SHIFT_ATTEMPT_CTR`. */
static void sched_task_beta(void)
{
    if (input_pa1() == 1u) {
        G_DRIVE_DIR = 0xF0u;
        motor_h_bridge_set(0xF0u);
        if (G_MOTION_REACHED == 1u) {
            if (G_STATE_115 == 4u) {
                G_TASK_ID  = 4u;
                G_STATE_FC = 4u;
            } else if (G_STATE_115 < 2u || G_STATE_115 > 3u) {
                if (G_STATE_115 == 1u && input_pa1() == 1u) {
                    G_TASK_ID  = 2u;
                    G_STATE_FC = 3u;
                }
            } else {
                /* G_STATE_115 ∈ {2, 3} */
                G_TASK_ID  = 5u;
                G_STATE_FC = 5u;
            }

            motor_h_bridge_set(0xFFu);
            G_DRIVE_DIR = 0xFFu;
            G_SHIFT_ATTEMPT_CTR = G_SHIFT_ATTEMPT_CTR + 1;
            if (G_SHIFT_ATTEMPT_CTR >= 3) {
                G_STATE_FC = 6u;
            }
            state_flags_reset();
        }
    }
    if (input_pa1() == 0u) {
        G_STATE_115         = 1u;
        motor_h_bridge_set(0xFFu);
        G_DRIVE_DIR         = 0u;
        G_TASK_ID           = 0u;
        G_SHIFT_ATTEMPT_CTR = 0;
        G_FLAG_117          = 1u;
        G_STATE_FC          = 2u;
        G_5C_BUSY           = 1u;
    }
}

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
        sched_idle_reset();
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
    /* .data init (copy from flash to SRAM). The OEM copies 192 B from
     * flash 0x08004828 to SRAM 0x20000000. Note: a literal-pool scan
     * of the OEM image finds zero references into SRAM 0..0xBF after
     * this copy, so the destination region is effectively dead in the
     * OEM build — the memcpy itself is preserved here only to match
     * the OEM byte sequence. */
    memcpy(G_SRAM_BASE, DATA_INIT_SRC, DATA_INIT_LEN);

    /* `G_HCLK_HZ` at SRAM 0x20000148 must hold the active HCLK in Hz
     * before `boot_init_systick` runs — the OEM bounds-checks
     * `(G_HCLK_HZ / 1000) - 1 <= 0x00FFFFFF` and falls into a
     * `b .`-style infinite loop if the value reads as 0xFFFFFFFF.
     *
     * In the OEM image, neither shifterware nor shifterboot writes
     * this location anywhere — confirmed by a Ghidra cross-binary
     * xref sweep on 2026-05-19 (script:
     * `~/ghidra_scripts/FindWritesToAddress.java`, 0 writes vs 2
     * reads in shifterware, 0 refs in shifterboot). The OEM must
     * rely on some out-of-band initialisation (production-flow
     * programming, SRAM retention on a specific MCU variant, or a
     * write via an indirect path the cross-binary xref scan misses).
     *
     * We can't reproduce that mechanism without hardware access, so
     * for our build we set the value explicitly here. The value
     * matches what `set_sysclock_to_48m` (in `shifterboot/src/clock.c`,
     * derived from `unused_set_sysclock_to_48m` @ shifterware
     * `0x080045B8`) brings the SYSCLK up to. Deviation from the OEM
     * byte sequence: one extra `ldr`/`str` pair. */
    G_HCLK_HZ = 48000000u;

    /* RCC + NVIC + IRQs */
    RCC->APB2ENR |= RCC_APB2_SYSCFGEN;
    syscfg_set_mem_mode(3u);
    __asm__ volatile ("cpsie i");

    /* Per-peripheral bring-up. */
    boot_init_systick();
    boot_init_gpio();
    uart1_init(9600u);
    RCC->AHBENR |= RCC_AHB_CRC_BIT;
    settings_load();

    /* TIM2 1 kHz periodic IRQ. Prescaler = HCLK/100000 - 1, ARR = 99
     * → update event every (psc+1)*(arr+1) HCLK cycles. The `uxth`
     * truncation to 16 bits is the OEM's: TIM2->PSC is a 16-bit
     * register. */
    {
        const uint32_t psc = (G_HCLK_HZ / TIM2_TICK_BASE_HZ) - 1u;
        tim2_init_periodic((uint16_t)psc, TIM2_TICK_PERIOD);
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
                flash_settings_commit();
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
