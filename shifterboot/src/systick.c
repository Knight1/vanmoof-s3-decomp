#include <stdint.h>

#include "systick.h"

/* Cortex-M0 SysTick block (System Control Space @ 0xE000E000, SysTick
 * registers start at 0xE000E010). The OEM materialises the base as the
 * pool word `0xE000E000` and indexes with byte offsets +0x10 / +0x14 /
 * +0x18 — matching the CMSIS `SysTick->CTRL/LOAD/VAL` field offsets. */
#define SCS_BASE       (0xE000E000UL)
#define SYSTICK_CTRL   (*(volatile uint32_t *)(SCS_BASE + 0x10UL))
#define SYSTICK_LOAD   (*(volatile uint32_t *)(SCS_BASE + 0x14UL))
#define SYSTICK_VAL    (*(volatile uint32_t *)(SCS_BASE + 0x18UL))

/* CMSIS Cortex-M0 internal-IRQ ID for SysTick. */
#define SYSTICK_IRQN   (-1)

/* CMSIS `core_cm0.h` inline that the MindMotion BSP compiles out-of-line
 * (vendor-stock at `0x08001404`, 110 B). Sets the priority field of the
 * SHP / NVIC IP register that covers the requested IRQn. */
extern void NVIC_SetPriority(int32_t IRQn, uint32_t priority);

volatile uint32_t g_systick_countdown;

void systick_tick(void)
{
    if (g_systick_countdown != 0u) {
        g_systick_countdown = g_systick_countdown - 1u;
    }
}

void SysTick_Handler(void)
{
    systick_tick();
}

/* OEM @ 0x080014CA (20 B). Single-arg millisecond delay.
 *
 * The OEM body spills the `ms` arg to the stack on entry
 * (`push {r0, lr}; ldr r0, [sp, #0]`) and uses `pop {r3, pc}` on
 * exit — a `-O0`-style "every parameter is stack-resident" pattern;
 * the popped `r3` is discarded. We translate the visible
 * semantics; gcc `-Os` keeps `ms` in a register.
 *
 * The `nop` at PC `0x080014D2` between the store and the spin is a
 * 2-byte alignment pad for the literal-pool fetch that follows. */
void mdelay(uint32_t ms)
{
    g_systick_countdown = ms;
    while (g_systick_countdown != 0u) {
        /* spin until SysTick_Handler ticks the counter down to 0 */
    }
}

/* OEM @ 0x08001472 (60 B). Sole caller is `main` at `0x0800020C` —
 * the systick setup runs early in main's prelude, immediately after
 * clock-tree bring-up but before the Modbus accumulator goes live.
 *
 * Body is CMSIS `SysTick_Config(48000)` inlined, followed by a VanMoof
 * override that bumps the SysTick exception priority from CM0-lowest
 * (3) to highest (0).
 *
 * The reload value `48000 - 1 = 47999` materialised from pool word
 * `0x0000BB80` produces a 1 ms tick at HCLK = 48 MHz — confirming the
 * SYSCLK that `set_sysclock_to_48m` configures and that `rcc_get_clocks_freq`
 * (mainware-side) reports when `RCC->CR` bit 20 is clear.
 *
 * The OEM emits CMSIS SysTick_Config's "ticks too large for the 24-bit
 * reload" failure path as a `nop; b .` trap. With ticks = 48000 the
 * range check is statically true so the trap is dead code at runtime;
 * `-Os` will fold the comparison away, leaving us behaviour-equivalent
 * but a few bytes shorter than the OEM.
 *
 * The double NVIC_SetPriority sequence (3, then 0) is exactly what the
 * OEM emits: SysTick_Config's inline sets the priority to CM0-lowest
 * via `(1U << __NVIC_PRIO_BITS) - 1U == 3`, then the VanMoof override
 * raises it back to 0. */
void boot_init_systick(void)
{
    if ((48000u - 1u) > 0x00FFFFFFu) {
        for (;;) { }
    }
    SYSTICK_LOAD = 48000u - 1u;
    NVIC_SetPriority(SYSTICK_IRQN, 3u);
    SYSTICK_VAL  = 0u;
    SYSTICK_CTRL = 7u;                       /* CLKSOURCE | TICKINT | ENABLE */

    NVIC_SetPriority(SYSTICK_IRQN, 0u);
}
