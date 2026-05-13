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
#define G_TASK_ID           (*(volatile uint8_t  *)0x20000118u)
#define G_FLAG_13E          (*(volatile uint8_t  *)0x2000013Eu)
#define G_FLAG_13D          (*(volatile uint8_t  *)0x2000013Du)
#define G_FLAG_117          (*(volatile uint8_t  *)0x20000117u)
#define G_FLAG_116          (*(volatile uint8_t  *)0x20000116u)

#define G_5C_BUSY           (*(volatile uint8_t  *)0x2000013Cu)
#define G_5C_DEADLINE_BASE  (*(volatile uint32_t *)0x20000110u)
#define G_5C_LATCH_BYTE     (*(volatile uint8_t  *)0x20000131u)  /* 0x20000130 + 1 */
#define G_MODE              (*(volatile uint8_t  *)0x20000139u)

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
static void    sched_pre_task(uint8_t b)         { (void)b; TRAP_VOID(); } /* OEM @ 0x080036D4 (74 B) */
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
        sched_pre_task(G_5A_TARGET);

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

        if (G_MODE == 0u && (G_TICK_B - G_TICK_PREV_B == RX_TICK_ROLLOVER)) {
            G_TICK_B      = 0u;
            G_TICK_D4     = 0u;
            G_TICK_A      = 0u;
        }
        G_TICK_PREV_B = G_TICK_B;
    }
}
