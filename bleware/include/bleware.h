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

/* Log-block count (src/log_gatt.c). Returns the number of 16-byte
 * blocks available between head and tail in the 128 KB circular log
 * buffer (0x20000 wrap, rounded up). Returns 0 on semaphore timeout.
 * OEM @ 0x00020338 / OEM @ 0x000273DC. */
uint32_t log_block_count_get(void);
uint8_t  log_total_size_byte(void);

/* External SPI NOR-flash driver (src/extflash.c). Sector size 4 KB.
 * `extflash_erase_range` aligns down and erases every 4 KB sector
 * intersecting [addr, addr+len). Returns 1 on success, 0 on failure. */
int      extflash_erase_range(uint32_t addr, uint32_t len);    /* FUN_00016A50 */
int      extflash_write(uint32_t addr, uint32_t len, const void *src); /* FUN_00015B9C */
int      extflash_read (uint32_t addr, uint32_t len, void       *dst); /* FUN_0001C5A4 */
struct extflash_chip_info;
const struct extflash_chip_info *extflash_get_chip_info(void);         /* FUN_000273D0 */

/* Per-service TI BLE-stack callback shims for svc 0x5560
 * (src/gatt_svc_5560.c). Registered with GATTServApp_RegisterService;
 * delegate to the central dispatchers. */
uint8_t svc_5560_read_attr_cb (uint32_t conn, void *attr, uint8_t *out_buf,
                               uint16_t *out_len, uint16_t offset,
                               uint16_t max_len, uint8_t op_byte);  /* FUN_0001E310 */
uint8_t svc_5560_write_attr_cb(uint32_t conn, void *attr, uint8_t *value,
                               uint16_t len, uint16_t offset);      /* FUN_00019A80 */

/* The 8 remaining per-service shim pairs (one for each non-backoffice
 * service except 0x5560, which lives in its own TU). All defined in
 * src/gatt_service_shims.c via a shared core + DEFINE_GATT_SHIM_PAIR
 * macro. Signatures match svc_5560_*_attr_cb. */
#define DECL_SHIM_PAIR(svc)                                                    \
    uint8_t svc_##svc##_read_attr_cb (uint32_t, void *, uint8_t *, uint16_t *, \
                                      uint16_t, uint16_t, uint8_t);            \
    uint8_t svc_##svc##_write_attr_cb(uint32_t, void *, uint8_t *, uint16_t,   \
                                      uint16_t)

DECL_SHIM_PAIR(5510);
DECL_SHIM_PAIR(5520);
DECL_SHIM_PAIR(5530);
DECL_SHIM_PAIR(5540);
DECL_SHIM_PAIR(5570);
DECL_SHIM_PAIR(5590);
DECL_SHIM_PAIR(55a0);
DECL_SHIM_PAIR(55c0);

#undef DECL_SHIM_PAIR

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

/* Central GATT write dispatcher (src/gatt_write.c). Reached via the
 * central vtable at RAM 0x20005A30+0 (no static xref — the populator
 * runs through register-indirect addressing). Dispatches the per-svc,
 * per-char write into the right module handler after running the
 * crypto-flag gates and the runtime-permission check. The argument
 * is a per-write event struct assembled by the BLE-stack shim. */
struct gatt_write_event;
int      xs3_gatt_process_write_event(struct gatt_write_event *evt); /* FUN_00004DB0 */

/* CRC-32/zlib (src/crc32.c). Reflected, polynomial 0xEDB88320.
 * No final XOR. OEM at 0x00025198. */
uint32_t crc32_le(uint32_t seed, const void *buf, uint32_t len);

/* Allocation helpers — TI-RTOS heap (OEM `monitor_alloc/free`). */
void  *monitor_alloc(unsigned int size);
void   monitor_free(void *p);

/* TI-RTOS BIOS_start (ROM thunk). */
void  BIOS_start(void) __attribute__((noreturn));

/* Integer-payload SSP relay helpers (src/ssp_relay.c). Both pulse
 * the BLE activity LED first (if any authenticated conn), then post
 * a single little-endian frame onto the inter-module bus. */
void  ssp_relay_u32(uint16_t cmd_id, uint32_t value);  /* 0x00021884 */
void  ssp_relay_u16(uint16_t cmd_id, uint16_t value);  /* 0x00023204 */

/* Inter-module bus idle check (src/protocols/ssp.c). Returns 0 if
 * the bus is idle, 1 if a transaction is in-flight. OEM @ 0x00026594. */
int   module_bus_is_idle(void);

/* Bleware timekeeper subsystem (src/timekeeper.c). */
void     sysclock_snapshot(uint32_t out_clock[3]);                 /* 0x000236E8 */
void     timekeeper_submit_epoch(uint32_t epoch);                  /* 0x00026CC0 */
int      timekeeper_apply_request(const uint32_t request[3]);      /* 0x00020B18 */
uint64_t timekeeper_read_be(void);                                 /* 0x00027448 */

/* State-machine notify primitive (src/state_machine.c). Wraps a small
 * payload in an envelope and posts it as kind 0x32 onto the
 * bluetoothtask user-message queue. OEM @ 0x00017C6C. */
void  state_machine_post(uint32_t state_id, const void *payload, uint16_t len);

/* Envelope alloc + queue post for the bluetoothtask's user-message
 * queue (src/bluetoothtask_post.c). Allocates an 8-byte {kind, ptr}
 * envelope and hands it to task_queue_enqueue_and_signal. Returns 0
 * on success, 0x13 on alloc failure. OEM @ 0x00023CC8. */
int   task_queue_publish_envelope(uint32_t kind, const void *payload,
                                  uint16_t len, uint32_t tag);

/* Secrets-store population accessor (src/secrets.c). Counts valid
 * UKEY records in slots [0, 123] — used as the "device is provisioned"
 * gate by `auth_derive_session_key`. OEM @ 0x00025680. */
int   secrets_count_valid_in_keys_range(void);

/* Session-key derivation + manufacturing-ECB helpers (src/auth.c). */
void *auth_derive_session_key(uint32_t client_key_id);            /* 0x00018B1C */
void  mfg_key_ecb_decrypt_chunks(uint8_t       *dst,
                                 const uint8_t *src,
                                 uint32_t       total_len);       /* 0x00024740 */

/* Pin a session key for a backoffice-authenticated connection
 * (src/ble_connection.c). OEM @ 0x0001A218. */
int   backoffice_auth_session_init(uint16_t conn, const void *session_key);

/* Per-connection state accessors (src/ble_connection.c). The BLE
 * stack tracks each active connection in a 0x7C-byte record at
 * `g_ble_connection_table[conn]`. Each accessor takes the per-record
 * semaphore, sanity-checks the conn handle, performs the read/write,
 * and posts the semaphore. */
int      indicate_seq_peek(uint16_t conn, uint16_t *out_seq);     /* 0x00022970 */
int      indicate_seq_advance(uint16_t conn);                     /* 0x00023114 */
int      ble_connection_get_session_key(uint32_t conn);           /* 0x00023DCC */
int      ble_connection_set_session_key(uint32_t conn, const void *key_16); /* 0x000231C8 */
int      ble_conn_state_byte(uint32_t conn, uint8_t *out_byte);    /* 0x000228B0 */

/* ATT MTU clamp — reads the negotiated MTU for a connection and
 * writes it back via *len_inout. OEM @ 0x000229B0. */
int      att_mtu_clamp(uint32_t conn, uint16_t *len_inout);

/* BLE connection introspection (src/ble_connection.c). Used by cmd_ble_info. */
int      ble_connection_count(int unused);
int      ble_connection_present(int index);
void     ble_connection_addr(int index, uint8_t *dst);
void     ble_connection_params(int index, uint16_t *interval,
                               uint16_t *latency, uint16_t *timeout);
int      ble_connection_is_rider_app(int index);
uint8_t *ble_device_address(int addr_type);

/* Runtime permission mask — reads the 32-bit capability mask from the
 * M-Key struct at RAM 0x2000A3DC offset +0x14, gated on whether the
 * first 0x20 bytes of the struct are non-zero (initialized). Both GATT
 * dispatchers apply it as a gate against the per-char required mask.
 * OEM @ 0x00026050. */
uint32_t runtime_permission_mask(void);

/* 15-bit LCG pseudo-random (src/lcg_random.c). OEM at 0x00023E34. */
uint32_t lcg_random_u15(void);

/* Post a kind-0x04 "disconnect" message to the bluetoothtask's user-
 * message queue (src/bluetoothtask_post.c). The bluetoothtask consumes
 * it on its next event-flag-bit-30 wakeup and either force-disconnects
 * the specific connection or, with conn = 0xFFFD, all connections.
 * Called from the GATT-write dispatcher on auth/perm/pad failures and
 * from the monitor `ble_disconnect` command. Returns 0 on success, -1
 * if either the payload or envelope alloc fails. OEM at 0x00021030. */
int  ble_post_disconnect(uint16_t conn, uint8_t reason);

/* ICall entity-registry lookup (src/icall_runtime.c). Returns the
 * ICall entity index (0..5) that the calling BIOS task is registered
 * under, or 0xff if the task isn't registered or we aren't in a
 * runnable thread. OEM at flash 0x00020C54. */
int  icall_caller_entity(void);

/* Variadic log-emit helper (src/log_emit.c). Dispatches a format-string
 * log line to the TI BLE-stack ICall logger service and synchronously
 * waits for the ack (1000 ms timeout). `service_id` is the ICall
 * service to address — 0x10 in every observed call site. Returns the
 * status word the logger writes back into the first-variadic slot
 * (zero on success). OEM at flash 0x0001AC6C. */
uint32_t log_emit_v(uint32_t service_id, const char *fmt, ...);

/* GAP Host event dispatcher — handles ICall event class 0x91
 * sub-code 0x3E (BLE GAP Host commands). Called by the bluetoothtask
 * event loop. OEM at flash 0x00010B40. */
uint32_t gap_event_91_3e_handler(const void *msg);

/* Firmware info printer (src/print_firmware_info.c). Prints device
 * name, BLE MAC, version, compile date, BIM info, reset reason,
 * and systick. Called by cmd_info_ver. OEM @ 0x000054D8. */
void print_firmware_info(void);

/* Reset reason string (src/reset_reason.c). Returns a static string
 * for the last reset cause. OEM @ 0x000145AC. */
const char *reset_reason_string(void);

/* System helpers (src/system.c). Called by cmd_shutdown / cmd_reset. */
void system_state_save(void);
int  system_power_down(int mode, int flags);
void system_software_reset(void);

/* Firmware update (src/cmd_update.c). OEM @ 0x0000D444. */
int  firmware_update_start(void *params);

/* PACK ingest (src/pack_ingest.c). Called by cmd_pack_upload. */
void pack_ingest_start(void);
void pack_upload_finalize(void);

/* Utility helpers (src/monitor_helpers.c / src/log_stubs.c). */
void     format_size(uint32_t bytes, char *buf, unsigned int bufsz);
uint32_t rtos_mem_get_stats(void *stats_out);
int      snv_compact(uint32_t arg);
int      snv_free_space_query(void);
uint32_t log_block_count(void);
void     log_format_block(uint32_t index, void *out_16B);
void     log_submit(const void *block_16B);

/* Audio stubs (src/audio_stubs.c). */
void audio_clip_dump_one(uint32_t index);
void audio_player_play(uint32_t index);
void audio_player_stop_or_pause(int action);

/* PACK filesystem stubs (src/packfs_stubs.c). */
void *packfs_open(void *params);
int   packfs_next(void *handle, void *entry_out);
void  packfs_close(void *handle);

#endif /* BLEWARE_BLEWARE_H */
