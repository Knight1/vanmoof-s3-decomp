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

/* External-SPI-flash secrets store @ flash 0x005A000 (4 KB sector,
 * 128 records × 32 B, payload + CRC-32). See src/secrets.c. */
int      secrets_record_read(int index, void *out_record);
int      secrets_record_write_verify(int index, const void *record);
int      secrets_find_by_key(uint32_t key, void *out_record);
int      secrets_count_free_slots(void);
int      secrets_upsert_keyed_record(const void *record_24);
int      secrets_upsert_keyed_batch(const void *records, unsigned int count);
uint32_t secrets_ensure_mid_record(void);

/* Backoffice provisioning (src/provisioning.c). See xs3_gatt_backoffice.c
 * (OEM source path embedded at flash 0x00004140). */
uint8_t *manufacturing_key_get_or_init_default(void);
int      secrets_provisioning_apply_bulk(const uint8_t *pkt,
                                         uint8_t       *ble_addr_out,
                                         uint32_t       payload_len);

/* Allocation helpers — TI-RTOS heap (OEM `monitor_alloc/free`). */
void  *monitor_alloc(unsigned int size);
void   monitor_free(void *p);

/* TI-RTOS BIOS_start (ROM thunk). */
void  BIOS_start(void) __attribute__((noreturn));

#endif /* BLEWARE_BLEWARE_H */
