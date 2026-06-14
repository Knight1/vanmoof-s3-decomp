/* vector_table.c — Cortex-M4 vector table for bleware.
 *
 * Pinned at flash 0x00000090 by the linker (linker_cc2642r1.ld places
 * the `.isr_vector` section right after the OAD header).
 *
 * Slot 0 = initial MSP = 0x20013A00 (matches the OEM's pool word that
 * Reset_Handler reads to seed SP). Slot 1 = Reset_Handler. Slots 2..13
 * default to TI's SimpleLink ROM general-trap handler at 0x1002FB38.
 * Slot 14 = PendSV = 0x1002FB10 (PendSV ROM stub). Slot 15 = SysTick =
 * 0xFFFFFFFF (unused — bleware 1.4.01 doesn't enable SysTick from the
 * application side; TI-RTOS's clock module owns timing).
 *
 * The OEM flash table is exactly 16 system words (0x90..0xCF, 64 bytes):
 * the very next bytes at 0xD0 are application code (FUN_000000d0,
 * 0xD0-0xBB9), not 32 IRQ vector words. On CC2642 the application IRQ
 * vectors live in a relocated (VTOR/RAM) table, so the flash table here
 * carries only the 16 ARM system exceptions. */

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

/* OEM flash table = 16 system words only (verified at flash 0x90: the
 * bytes at 0xD0 are code, not vectors). Words 2..13 are all the ROM
 * general-trap handler (0x1002FB39); word 14 = PendSV ROM stub; word 15
 * = 0xFFFFFFFF. */
__attribute__((section(".isr_vector"), used))
void (*const g_vectors[16])(void) = {
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
};
