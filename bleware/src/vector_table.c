/* vector_table.c — Cortex-M4 vector table for bleware.
 *
 * Pinned at flash 0x00000090 by the linker (linker_cc2642r1.ld places
 * the `.isr_vector` section right after the OAD header).
 *
 * Slot 0 = initial MSP = 0x20013A00 (matches the OEM's pool word that
 * Reset_Handler reads to seed SP). Slot 1 = Reset_Handler. Slots 2..14
 * default to TI's SimpleLink ROM handlers at 0x1002FB38 (general trap)
 * and 0x1002FB10 (PendSV ROM stub). Slot 15 = SysTick = 0xFFFFFFFF
 * (unused — bleware 1.4.01 doesn't enable SysTick from the application
 * side; TI-RTOS's clock module owns timing).
 *
 * Slots 16..47 = peripheral IRQs. As decomp progresses we'll replace
 * the ROM-default entries with the per-driver handlers actually
 * installed in the OEM image. */

#include <stdint.h>

extern uint32_t _stack_top;   /* linker symbol — for documentation only;
                                  the actual slot 0 value is hardcoded
                                  to match the OEM. */
extern void Reset_Handler(void);

/* TI SimpleLink ROM trap stubs at 0x1002FB38 (general) and 0x1002FB10
 * (PendSV). These are the "do nothing useful" handlers TI ships in ROM
 * for unused vectors. */
#define ROM_TRAP_HANDLER        ((void (*)(void))0x1002FB39u)  /* Thumb-set */
#define ROM_PENDSV_HANDLER      ((void (*)(void))0x1002FB11u)
#define VECTOR_UNUSED           ((void (*)(void))0xFFFFFFFFu)

/* Initial MSP from the OEM vector table (vec[0]). */
#define INITIAL_MSP             ((void (*)(void))0x20013A00u)

__attribute__((section(".isr_vector"), used))
void (*const g_vectors[48])(void) = {
    /* 0  */ INITIAL_MSP,              /* initial MSP */
    /* 1  */ Reset_Handler,            /* Reset */
    /* 2  */ ROM_TRAP_HANDLER,         /* NMI */
    /* 3  */ ROM_TRAP_HANDLER,         /* HardFault */
    /* 4  */ ROM_TRAP_HANDLER,         /* MemManage */
    /* 5  */ ROM_TRAP_HANDLER,         /* BusFault */
    /* 6  */ ROM_TRAP_HANDLER,         /* UsageFault */
    /* 7  */ ROM_TRAP_HANDLER,         /* reserved */
    /* 8  */ ROM_TRAP_HANDLER,         /* reserved */
    /* 9  */ ROM_TRAP_HANDLER,         /* reserved */
    /* 10 */ ROM_TRAP_HANDLER,         /* reserved */
    /* 11 */ ROM_TRAP_HANDLER,         /* SVCall */
    /* 12 */ ROM_TRAP_HANDLER,         /* DebugMon */
    /* 13 */ ROM_TRAP_HANDLER,         /* reserved */
    /* 14 */ ROM_PENDSV_HANDLER,       /* PendSV (TI-RTOS owns this) */
    /* 15 */ VECTOR_UNUSED,            /* SysTick (unused) */

    /* IRQ 0..31 — peripheral interrupts. All currently default to the
     * ROM trap handler; per-driver entries land as the decomp identifies
     * each one. */
    /* 16 */ ROM_TRAP_HANDLER, /* IRQ 0  */
    /* 17 */ ROM_TRAP_HANDLER, /* IRQ 1  */
    /* 18 */ ROM_TRAP_HANDLER, /* IRQ 2  */
    /* 19 */ ROM_TRAP_HANDLER, /* IRQ 3  */
    /* 20 */ ROM_TRAP_HANDLER, /* IRQ 4  */
    /* 21 */ ROM_TRAP_HANDLER, /* IRQ 5  */
    /* 22 */ ROM_TRAP_HANDLER, /* IRQ 6  */
    /* 23 */ ROM_TRAP_HANDLER, /* IRQ 7  */
    /* 24 */ ROM_TRAP_HANDLER, /* IRQ 8  */
    /* 25 */ ROM_TRAP_HANDLER, /* IRQ 9  */
    /* 26 */ ROM_TRAP_HANDLER, /* IRQ 10 */
    /* 27 */ ROM_TRAP_HANDLER, /* IRQ 11 */
    /* 28 */ ROM_TRAP_HANDLER, /* IRQ 12 */
    /* 29 */ ROM_TRAP_HANDLER, /* IRQ 13 */
    /* 30 */ ROM_TRAP_HANDLER, /* IRQ 14 */
    /* 31 */ ROM_TRAP_HANDLER, /* IRQ 15 */
    /* 32 */ ROM_TRAP_HANDLER, /* IRQ 16 */
    /* 33 */ ROM_TRAP_HANDLER, /* IRQ 17 */
    /* 34 */ ROM_TRAP_HANDLER, /* IRQ 18 */
    /* 35 */ ROM_TRAP_HANDLER, /* IRQ 19 */
    /* 36 */ ROM_TRAP_HANDLER, /* IRQ 20 */
    /* 37 */ ROM_TRAP_HANDLER, /* IRQ 21 */
    /* 38 */ ROM_TRAP_HANDLER, /* IRQ 22 */
    /* 39 */ ROM_TRAP_HANDLER, /* IRQ 23 */
    /* 40 */ ROM_TRAP_HANDLER, /* IRQ 24 */
    /* 41 */ ROM_TRAP_HANDLER, /* IRQ 25 */
    /* 42 */ ROM_TRAP_HANDLER, /* IRQ 26 */
    /* 43 */ ROM_TRAP_HANDLER, /* IRQ 27 */
    /* 44 */ ROM_TRAP_HANDLER, /* IRQ 28 */
    /* 45 */ ROM_TRAP_HANDLER, /* IRQ 29 */
    /* 46 */ ROM_TRAP_HANDLER, /* IRQ 30 */
    /* 47 */ ROM_TRAP_HANDLER, /* IRQ 31 */
};
