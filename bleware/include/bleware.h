/* bleware.h — top-level forward declarations for cross-TU calls.
 *
 * The decomp grows this file as more functions land. For now: the
 * boot-chain symbols (`Reset_Handler`, `cinit_walker`, `main`,
 * `SetupTrimDevice`) and the few helpers they call. */

#ifndef BLEWARE_BLEWARE_H
#define BLEWARE_BLEWARE_H

#include <stdint.h>

/* Startup chain. */
void  Reset_Handler(void) __attribute__((noreturn));
int   main(int argc, char **argv);
void  _exit(int code) __attribute__((noreturn));

/* TI CGT runtime — `_auto_init_table` analogue (the cinit + auto-init
 * walker that runs between Reset_Handler and `main`). */
void  cinit_walker(void);

/* TI driverlib helpers used by Reset_Handler. */
void     SetupTrimDevice(void);
void     bim_chip_assert_supported(void);
int32_t  bim_chip_family(void);
int32_t  bim_chip_hw_revision(void);
void     bim_setup_after_cold_reset_cfg1(uint32_t fcfg1_rev);
void     bim_setup_adi_step(uint32_t target_code);

/* TI BIOS-init helpers main calls. */
void  tirtos_modules_init(void);

/* Task creation. */
void  create_bluetoothtask(void);
void  bluetoothtask_main(void);

/* Monitor (debug console). */
int   cmd_help(int verb, void *p2, void *p3, uint32_t p4);
int   monitor_dispatch_loop(const char *user_input);

/* TI-RTOS BIOS_start (ROM thunk). */
void  BIOS_start(void) __attribute__((noreturn));

#endif /* BLEWARE_BLEWARE_H */
