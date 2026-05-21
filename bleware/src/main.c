/* main.c — bleware application entry.
 *
 * OEM at 0x0001CFEC (~130 B). The 18 B trampoline at flash
 * 0x00026474 reads `argc`/`argv` from the global at flash 0x00026488
 * and tail-calls here; we collapse that into the standard signature.
 *
 * Body sequence (verified against the OEM decompile):
 *   1. board / AON power-mgmt init (touches AON via ROM HAPI)
 *   2. set a kernel-global pointer
 *   3. tirtos_modules_init — 13 sub-init calls; halt on failure
 *   4. two ROM-API indirect calls (slots 0 + 1 of ROM_API_TABLE
 *      at 0x100001D8) — likely OsalLink_init / ICall_init pair
 *   5. Hwi/Swi object construct via FUN_00023A58
 *   6. BLE-stack init helpers (FUN_00024DE0, FUN_00024058)
 *   7. create_bluetoothtask — Task_construct (prio 3, 1.8 KB stack)
 *   8. BIOS_start() — TI-RTOS scheduler (never returns)
 *
 * Skeleton: stub each call as a weak placeholder so the link is
 * clean. The actual bodies land as the decomp progresses; each
 * stub here gets replaced when its corresponding OEM function is
 * decoded.
 */

#include "bleware.h"

#include <stdint.h>

/* Weak stubs for the steps we haven't yet decoded. The skeleton
 * needs them to satisfy the link; the decomp replaces them in turn. */
__attribute__((weak)) void board_pm_init(int argc, char **argv,
                                          uint32_t p3, uint32_t p4)
{
    (void)argc; (void)argv; (void)p3; (void)p4;
}

__attribute__((weak)) void kernel_pointer_setter(void *p)
{
    (void)p;
}

__attribute__((weak)) void tirtos_modules_init(void) { }
__attribute__((weak)) void ble_stack_init_a(void) { }
__attribute__((weak)) void ble_stack_init_b(void) { }
__attribute__((weak)) void create_bluetoothtask(void) { }

int main(int argc, char **argv)
{
    /* 1. Board / power-management init. */
    board_pm_init(argc, argv, 0u, 0u);

    /* 2. Kernel pointer setter — OEM reads a single value from a
     *    global table and stashes it for the kernel. */
    kernel_pointer_setter((void *)0);

    /* 3. TI-RTOS module-init chain (13 sub-init calls in OEM). */
    tirtos_modules_init();

    /* 4. ROM-API indirect calls — placeholder. */

    /* 5..6. BLE-stack init. */
    ble_stack_init_a();
    ble_stack_init_b();

    /* 7. Create the bluetooth task. */
    create_bluetoothtask();

    /* 8. Enter the TI-RTOS scheduler. Never returns under normal ops;
     *    if it does, the Reset_Handler epilogue calls `_exit`. */
    BIOS_start();

    /* Unreachable — keep the standard `int main` signature. */
    /* return 0; */
}
