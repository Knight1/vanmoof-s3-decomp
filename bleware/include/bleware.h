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

/* Backoffice GATT message handler (src/provisioning.c). See
 * xs3_gatt_backoffice.c (OEM source path embedded at flash 0x00004140). */
uint8_t *manufacturing_key_get_or_init_default(void);
int      gatt_handle_backoffice_message_data(uint32_t       conn_idx,
                                             const uint8_t *frag,
                                             unsigned int   frag_len);

/* Sub-command helpers — implementations TBD, currently linked via
 * weak no-op stubs in hal_stubs.S. Each has an OEM address tag in the
 * comment for the eventual real implementation. */
int      mkey_record_write_slot126(const void *rec_24);          /* FUN_000222AC */
int      secrets_delete_by_key(uint32_t key);                    /* FUN_00024D14 */
int      backoffice_ack_noop(void);                              /* FUN_0002774A */
int      module_forward_async(uint32_t cmd_id, uint8_t arg);     /* FUN_00024508 */
int      secrets_sector_erase(void);                             /* FUN_00026C30 */
int      module_forward_sync(uint16_t cmd, const uint8_t *payload,
                             unsigned int len);                  /* FUN_000177E8 */
int      ble_connection_is_active(uint32_t conn_idx);            /* FUN_00023D30 */
void     ble_connection_touch(uint32_t conn_idx);                /* FUN_00023608 */
void     backoffice_on_success_hook(void);                       /* FUN_00022BE8 */
int      gatt_notify_channel(int channel, const void *buf);      /* FUN_0001B538 */
uint32_t crc16_modbus(const uint8_t *buf, int len, uint32_t seed); /* FUN_0002651C */

/* OAD over-the-air firmware update (src/oad.c). Service 0x5510 write
 * handler; called by xs3_gatt_process_write_event for indices 0..2. */
int      oad_gatt_write_handler(uint32_t       conn_handle,
                                int            char_idx,
                                const uint8_t *payload,
                                uint32_t       payload_len);     /* FUN_000267A4 */

/* Log-dispatch GATT handler (src/log_gatt.c). Service 0x55C0 write
 * handler; reads the circular log buffer at ext-flash 0x03FDD000. */
int      log_gatt_write_handler(uint32_t       conn_handle,
                                int            char_idx,
                                const uint8_t *payload,
                                uint32_t       payload_len);     /* FUN_00014910 */

/* External SPI NOR-flash driver (src/extflash.c). Sector size 4 KB.
 * `extflash_erase_range` aligns down and erases every 4 KB sector
 * intersecting [addr, addr+len). Returns 1 on success, 0 on failure. */
int      extflash_erase_range(uint32_t addr, uint32_t len);    /* FUN_00016A50 */

/* Central GATT read dispatcher (src/gatt_read.c). Read-side analogue
 * of xs3_gatt_process_write_event. Called by the TI BLE-stack
 * ReadAttrCB for every char in the 11-service registry. */
int      xs3_gatt_process_read_event(uint32_t  module_idx,
                                     uint32_t  conn_handle,
                                     uint16_t  svc_uuid,
                                     uint16_t  char_idx,
                                     uint8_t  *out_buf,
                                     uint16_t *out_len,
                                     uint8_t   att_opcode);     /* FUN_000061c0 */

/* Allocation helpers — TI-RTOS heap (OEM `monitor_alloc/free`). */
void  *monitor_alloc(unsigned int size);
void   monitor_free(void *p);

/* TI-RTOS BIOS_start (ROM thunk). */
void  BIOS_start(void) __attribute__((noreturn));

#endif /* BLEWARE_BLEWARE_H */
