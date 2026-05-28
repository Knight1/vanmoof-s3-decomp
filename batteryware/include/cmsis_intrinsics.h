/*
 * CMSIS intrinsics for Cortex-M0+.
 *
 * The OEM firmware uses the ARM CMSIS intrinsics (__DSB, __ISB, __DMB,
 * __NOP, __disable_irq, __enable_irq, __get_PRIMASK, __set_PRIMASK).
 * Rather than pull in CMSIS-CORE, we provide the small subset we need
 * here as `static inline` wrappers around the corresponding inline
 * assembly. Definitions follow CMSIS-CORE exactly (operand encodings
 * and clobber lists), so swapping in the upstream header would
 * compile identically.
 */

#ifndef CMSIS_INTRINSICS_H
#define CMSIS_INTRINSICS_H

#include <stdint.h>

/* Data Synchronisation Barrier */
static inline void __DSB(void)
{
    __asm__ volatile ("dsb 0xF" : : : "memory");
}

/* Instruction Synchronisation Barrier */
static inline void __ISB(void)
{
    __asm__ volatile ("isb 0xF" : : : "memory");
}

/* Data Memory Barrier */
static inline void __DMB(void)
{
    __asm__ volatile ("dmb 0xF" : : : "memory");
}

/* No-operation. */
static inline void __NOP(void)
{
    __asm__ volatile ("nop");
}

/* Read PRIMASK (0 = IRQs enabled, 1 = IRQs masked). */
static inline uint32_t __get_PRIMASK(void)
{
    uint32_t result;
    __asm__ volatile ("mrs %0, PRIMASK" : "=r" (result) :: "memory");
    return result;
}

static inline void __set_PRIMASK(uint32_t priMask)
{
    __asm__ volatile ("msr PRIMASK, %0" : : "r" (priMask) : "memory");
}

static inline void __disable_irq(void)
{
    __asm__ volatile ("cpsid i" : : : "memory");
}

static inline void __enable_irq(void)
{
    __asm__ volatile ("cpsie i" : : : "memory");
}

#endif /* CMSIS_INTRINSICS_H */
