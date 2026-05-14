#include "exception.h"

/* The OEM emits three independent 2-byte `b .` trap loops (Thumb
 * 0xE7FE) — one entered from NMI, one from HardFault, and one
 * routed by every other vector slot. We preserve the three distinct
 * symbols so the vector table keeps three independent targets;
 * collapsing them into one would change the emitted byte sequence
 * and break the binary diff. */

__attribute__((naked, noreturn)) void NMI_Handler(void)
{
    __asm__ volatile ("b .");
}

__attribute__((naked, noreturn)) void HardFault_Handler(void)
{
    __asm__ volatile ("b .");
}

__attribute__((naked, noreturn)) void Default_Handler(void)
{
    __asm__ volatile ("b .");
}
