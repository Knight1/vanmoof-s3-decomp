/* hal_stubs.S — weak fallbacks for the TI SimpleLink SDK / ROM
 * symbols bleware's C sources declare `extern`.
 *
 * Until we vendor the SimpleLink SDK 3.40 (jeandudey mirror — see
 * `reference_ti_simplelink_sdk.md`), provide minimal `.weak` stubs
 * so the build links. Each stub is marked weak so the real BSP body
 * overrides it once the SDK is in place.
 *
 * Pattern matches shifterboot/src/hal_stubs.S — same idea, different
 * MCU.
 */

    .syntax unified
    .cpu    cortex-m4
    .thumb

/* ===== TI-RTOS BIOS_start =====
 *
 * OEM at ROM 0x1002EAA4. Never returns under normal operation
 * (enters the scheduler). For the skeleton, trap with `b .`.
 */
    .section .text.BIOS_start, "ax", %progbits
    .weak    BIOS_start
    .type    BIOS_start, %function
    .thumb_func
BIOS_start:
1:  b       1b
    .size   BIOS_start, .-BIOS_start

/* ===== Helpers cmd_extflash_verify depends on =====
 *
 * Weak no-ops so the skeleton links. Each gets replaced as the OEM
 * body lands in its own .c file.
 */

#define WEAK_NOOP(name) \
    .section .text.##name, "ax", %progbits ; \
    .weak    name ; \
    .type    name, %function ; \
    .thumb_func ; \
name: ; \
    movs    r0, #0 ; \
    bx      lr ; \
    .size   name, .-name

    WEAK_NOOP(monitor_log)
    WEAK_NOOP(monitor_print_help_line)
    WEAK_NOOP(memcpy)
    WEAK_NOOP(memset)
/* `monitor_alloc` and `monitor_free` are now real strong symbols in
 * src/ti_rtos_heap.c. Earlier WEAK_NOOP entries removed. */
    WEAK_NOOP(nvs_open)
    WEAK_NOOP(extflash_open)
    WEAK_NOOP(extflash_close)
    WEAK_NOOP(extflash_retry_backoff)
    WEAK_NOOP(extflash_get_chip_info)
    WEAK_NOOP(extflash_sw_wp_enabled)
    WEAK_NOOP(extflash_block_wp_enabled)

/* ===== Monitor command helper fallbacks ===== */
    WEAK_NOOP(monitor_command_matches)
    WEAK_NOOP(monitor_sscanf)
    WEAK_NOOP(monitor_sleep)
    WEAK_NOOP(firmware_update_start)
    WEAK_NOOP(print_firmware_info)
    WEAK_NOOP(log_block_count)
    WEAK_NOOP(log_format_block)
    WEAK_NOOP(monitor_strlen)
    WEAK_NOOP(log_submit)
    WEAK_NOOP(monitor_snprintf)
    WEAK_NOOP(packfs_open)
    WEAK_NOOP(packfs_next)
    WEAK_NOOP(packfs_close)
    WEAK_NOOP(format_size)
    WEAK_NOOP(extflash_erase)
    WEAK_NOOP(extflash_read)
    WEAK_NOOP(extflash_sector_write)
    WEAK_NOOP(crc32_le)
    WEAK_NOOP(memcmp)
    WEAK_NOOP(block_dispatch_queue_post)

/* RAM globals owned by the M-Key cache (OEM struct around
 * `DAT_000195FC = 0x2000A3DC`). Defined here as weak BSS bytes so the
 * skeleton links; the real RAM lives in xs3_gatt_backoffice.c. */
    .section .bss.g_mkey_working_buffer, "aw", %nobits
    .weak    g_mkey_working_buffer
    .type    g_mkey_working_buffer, %object
    .size    g_mkey_working_buffer, 4
g_mkey_working_buffer:
    .skip    4

    .section .bss.g_mkey_owning_struct_field30, "aw", %nobits
    .weak    g_mkey_owning_struct_field30
    .type    g_mkey_owning_struct_field30, %object
    .size    g_mkey_owning_struct_field30, 4
g_mkey_owning_struct_field30:
    .skip    4

    .section .bss.g_mkey_owning_struct_field34, "aw", %nobits
    .weak    g_mkey_owning_struct_field34
    .type    g_mkey_owning_struct_field34, %object
    .size    g_mkey_owning_struct_field34, 4
g_mkey_owning_struct_field34:
    .skip    4

    .section .bss.g_mkey_owning_struct_field3C, "aw", %nobits
    .weak    g_mkey_owning_struct_field3C
    .type    g_mkey_owning_struct_field3C, %object
    .size    g_mkey_owning_struct_field3C, 4
g_mkey_owning_struct_field3C:
    .skip    4
    WEAK_NOOP(ble_connection_count)
    WEAK_NOOP(ble_connection_present)
    WEAK_NOOP(ble_connection_is_rider_app)
    WEAK_NOOP(ble_connection_addr)
    WEAK_NOOP(ble_connection_params)
    WEAK_NOOP(ble_device_address)

/* ===== Backoffice GATT message handler — sub-command helpers =====
 *
 * Decoded plate-comments live in src/provisioning.c. Each gets its own
 * real .c body as it lands. The current weak no-ops let the skeleton
 * link with provisioning.o present. */
    WEAK_NOOP(mkey_record_write_slot126)
    WEAK_NOOP(secrets_delete_by_key)
    WEAK_NOOP(backoffice_ack_noop)
    WEAK_NOOP(module_forward_async)
    WEAK_NOOP(secrets_sector_erase)
    WEAK_NOOP(module_forward_sync)
    WEAK_NOOP(ble_connection_is_active)
    WEAK_NOOP(ble_connection_touch)
    WEAK_NOOP(backoffice_on_success_hook)
    WEAK_NOOP(gatt_notify_channel)
    WEAK_NOOP(crc16_modbus)

/* ===== OAD helpers (src/oad.c) ===== */
    WEAK_NOOP(oad_state_lock)
    WEAK_NOOP(bleware_control_event_post)
    WEAK_NOOP(oad_session_close)
    WEAK_NOOP(extflash_erase_range)
    WEAK_NOOP(extflash_write)
    WEAK_NOOP(ti_semaphore_post)
/* `FUN_00024740` is now strong as `mfg_key_ecb_decrypt_chunks` in
 * src/auth.c.
 * `state_machine_post` is now strong in src/state_machine.c;
 * `secrets_count_valid_in_keys_range` is now strong in src/secrets.c. */
/* Inner publish helper used by state_machine_post (FUN_000219A2). */
    WEAK_NOOP(task_queue_publish_envelope)
/* Firmware abort routine (FUN_0001F7F8), tail-called on alloc failure. */
    WEAK_NOOP(firmware_abort)

/* OAD state struct (single instance, RAM-resident). Weak BSS so the
 * skeleton links; real RAM address baked by the OEM C runtime. */
    .section .bss.g_oad_state, "aw", %nobits
    .weak    g_oad_state
    .type    g_oad_state, %object
    .size    g_oad_state, 0x68
g_oad_state:
    .skip    0x68

/* TI-RTOS tick-period word — OEM `PTR_DAT_0000BA50`. */
    .section .bss.g_ti_tick_period_us, "aw", %nobits
    .weak    g_ti_tick_period_us
    .type    g_ti_tick_period_us, %object
    .size    g_ti_tick_period_us, 4
g_ti_tick_period_us:
    .skip    4

/* ===== extflash.c helpers ===== */
    WEAK_NOOP(extflash_cs_assert)
    WEAK_NOOP(extflash_cs_deassert)
    WEAK_NOOP(extflash_spi_tx)
    WEAK_NOOP(extflash_spi_rx)
    WEAK_NOOP(extflash_wait_wip_clear)
    WEAK_NOOP(extflash_write_enable)

/* Singleton state struct (OEM `DAT_00016AFC`). 16-byte placeholder so
 * the skeleton links; the real RAM is populated by the extflash
 * driver init that runs early in the Bluetooth task. */
    .section .bss.g_extflash_state, "aw", %nobits
    .weak    g_extflash_state
    .type    g_extflash_state, %object
    .size    g_extflash_state, 0x10
g_extflash_state:
    .skip    0x10

/* ===== Monitor command helpers (cmd_log/cmd_os/cmd_audio/...) ===== */
    WEAK_NOOP(log_region_erase)
    WEAK_NOOP(log_writer_restart)
    WEAK_NOOP(rtos_mem_get_stats)
    WEAK_NOOP(monitor_key_wait_with_timeout)
    WEAK_NOOP(monitor_yield_ticks)
    WEAK_NOOP(snv_free_space_query)
    WEAK_NOOP(snv_compact)
    WEAK_NOOP(system_state_save)
    WEAK_NOOP(system_power_down)
    WEAK_NOOP(system_software_reset)
    WEAK_NOOP(ble_gap_terminate_link)
    WEAK_NOOP(ymodem_receive)
    WEAK_NOOP(pack_ingest_start)
    WEAK_NOOP(pack_upload_finalize)
    WEAK_NOOP(audio_clip_dump_one)
    WEAK_NOOP(audio_player_play)
    WEAK_NOOP(audio_player_stop_or_pause)
/* `module_publish_command`, `module_forward_async`,
 * `module_publish_sync_with_timeout`, `ble_authenticated_connection_count`,
 * `ble_activity_led_pulse`, `ssp_signal_fetch`, `ssp_queue_publish_frame`,
 * `ssp_publish_fetch_frame` are real strong symbols in
 * src/protocols/ssp.c — no weak stub needed. */

    WEAK_NOOP(ti_mailbox_post)
    WEAK_NOOP(ssp_frame_alloc)
    WEAK_NOOP(gpio_write)
/* `ble_connection_get_session_key` is now a real strong symbol in
 * src/ble_connection.c (was a weak stub here and again at line 399). */

/* SSP master state struct (singleton, RAM 0x20003104). */
    .section .bss.g_ssp_master, "aw", %nobits
    .weak    g_ssp_master
    .type    g_ssp_master, %object
    .size    g_ssp_master, 0x24
g_ssp_master:
    .skip    0x24

/* Per-module reply-state array. The OEM allocates 6+ records of
 * 0x7C bytes each; reserve a few here so module-index math doesn't
 * trip the linker. */
    .section .bss.g_ssp_modules, "aw", %nobits
    .weak    g_ssp_modules
    .type    g_ssp_modules, %object
    .size    g_ssp_modules, 0x300
g_ssp_modules:
    .skip    0x300

    .section .bss.g_ssp_sequence, "aw", %nobits
    .weak    g_ssp_sequence
    .type    g_ssp_sequence, %object
    .size    g_ssp_sequence, 1
g_ssp_sequence:
    .skip    1

    .section .bss.g_bleware_gpio_ctx, "aw", %nobits
    .weak    g_bleware_gpio_ctx
    .type    g_bleware_gpio_ctx, %object
    .size    g_bleware_gpio_ctx, 4
g_bleware_gpio_ctx:
    .skip    4

/* YModem target-offset pointer — OEM `*DAT_0000CC70`, a u32 in RAM
 * that cmd_audio_upload writes into before invoking ymodem_receive. */
    .section .bss.g_ymodem_target_offset, "aw", %nobits
    .weak    g_ymodem_target_offset
    .type    g_ymodem_target_offset, %object
    .size    g_ymodem_target_offset, 4
g_ymodem_target_offset:
    .skip    4

/* ===== gatt_svc_5560.c + gatt_service_shims.c helpers ===== */
    WEAK_NOOP(svc_5560_char_uuid_to_index)
    WEAK_NOOP(svc_5510_char_uuid_to_index)
    WEAK_NOOP(svc_5520_char_uuid_to_index)
    WEAK_NOOP(svc_5530_char_uuid_to_index)
    WEAK_NOOP(svc_5570_char_uuid_to_index)
    WEAK_NOOP(svc_5590_char_uuid_to_index)
    WEAK_NOOP(svc_55a0_char_uuid_to_index)
    WEAK_NOOP(svc_55c0_char_uuid_to_index)
    WEAK_NOOP(vanmoof_ssp_uuid_to_index)
    WEAK_NOOP(cccd_write_validate)

/* Per-service mailbox callback-slot pointers (one global per service;
 * three are shared at the same RAM address: 0x5510/0x5520/0x55A0). */
#define WEAK_PTR(name) \
    .section .bss.##name, "aw", %nobits ; \
    .weak    name ; \
    .type    name, %object ; \
    .size    name, 4 ; \
name: ; \
    .skip    4

    WEAK_PTR(g_svc_5510_mailbox_callback)
    WEAK_PTR(g_svc_5520_mailbox_callback)
    WEAK_PTR(g_svc_5530_mailbox_callback)
    WEAK_PTR(g_svc_5540_mailbox_callback)
    WEAK_PTR(g_svc_5570_mailbox_callback)
    WEAK_PTR(g_svc_5590_mailbox_callback)
    WEAK_PTR(g_svc_55a0_mailbox_callback)
    WEAK_PTR(g_svc_55c0_mailbox_callback)

/* Central-dispatch vtable pointer — OEM `*DAT_0001E370`. The actual
 * struct lives at RAM 0x20003F84 and is populated by GATT-init code
 * we haven't yet decoded. Weak BSS placeholder so the skeleton links. */
    .section .bss.g_gatt_dispatch, "aw", %nobits
    .weak    g_gatt_dispatch
    .type    g_gatt_dispatch, %object
    .size    g_gatt_dispatch, 4
g_gatt_dispatch:
    .skip    4

/* ===== log_emit.c + icall_runtime.c + ti_rtos_kernel.c helpers =====
 *
 * `icall_caller_entity`, `icall_cs_enter`, `icall_cs_exit`, and
 * `bios_abort` are now real strong symbols (see src/icall_runtime.c,
 * src/ti_rtos_kernel.c). The remaining ICall / BIOS helpers stay as
 * weak no-ops until their own decomp passes land. */
    WEAK_NOOP(icall_send_service_msg)
    WEAK_NOOP(icall_wait_match)
    WEAK_NOOP(icall_free_message)
    WEAK_NOOP(bios_get_thread_state)
    WEAK_NOOP(log_reply_match_pred)

/* ===== icall_runtime.c + ti_rtos_kernel.c ROM-thunk pass-throughs ===== */
    WEAK_NOOP(ti_task_self)
    WEAK_NOOP(bios_hwi_disable)
    WEAK_NOOP(bios_task_disable)
    WEAK_NOOP(bios_hwi_restore)
    WEAK_NOOP(bios_task_restore)
    WEAK_NOOP(abort_continuation)

/* ===== gatt_write.c helpers ===== */
/* `FUN_00021884` (→ `ssp_relay_u32`), `FUN_00023204` (→ `ssp_relay_u16`)
 * now strong in src/ssp_relay.c; `FUN_00026cc0` (→ `timekeeper_submit_epoch`)
 * now strong in src/timekeeper.c.
 * `auth_derive_session_key` is now strong in src/auth.c;
 * `backoffice_auth_session_init` in src/ble_connection.c. */
/* `timekeeper_apply_request` (FUN_00020B18) is now strong in
 * src/timekeeper.c. Its leaf helpers remain stubbed: */
    WEAK_NOOP(sysclock_snapshot)            /* FUN_000236E8 */
    WEAK_NOOP(timekeeper_read_request)      /* FUN_0001EAF8 */

/* Heap-resident timekeeper shared state + tick-modulus constant. */
    .section .bss.g_timekeeper_state, "aw", %nobits
    .weak    g_timekeeper_state
    .type    g_timekeeper_state, %object
    .size    g_timekeeper_state, 4
g_timekeeper_state:
    .zero    4

    .section .bss.g_timekeeper_tick_modulus, "aw", %nobits
    .weak    g_timekeeper_tick_modulus
    .type    g_timekeeper_tick_modulus, %object
    .size    g_timekeeper_tick_modulus, 4
g_timekeeper_tick_modulus:
    .zero    4
/* `ble_connection_set_session_key`, `indicate_seq_peek`,
 * `indicate_seq_advance`, and `ble_connection_get_session_key` are
 * now real strong symbols in src/ble_connection.c (earlier WEAK_NOOP
 * for `ble_connection_get_session_key` was at line 293 — see below).
 */

/* ===== ble_connection.c + lcg_random.c helpers =====
 * `ti_semaphore_pend` / `ti_semaphore_post` already declared as
 * WEAK_NOOPs earlier (lines 142, 472). No need to re-declare. */

/* Per-connection state table pointer — flash literal 0x000229AC /
 * 0x0002314C / 0x00023200 / 0x00023DFC, all holding RAM 0x20004158.
 * Each entry is 0x7C bytes; N entries depends on the BLE-stack
 * configuration (typically 1..8). Reserve room for 8 here. */
    .section .bss.g_ble_connection_table_storage, "aw", %nobits
    .weak    g_ble_connection_table_storage
    .type    g_ble_connection_table_storage, %object
    .size    g_ble_connection_table_storage, 0x3E0
g_ble_connection_table_storage:
    .skip    0x3E0

    .section .data.g_ble_connection_table, "aw", %progbits
    .weak    g_ble_connection_table
    .type    g_ble_connection_table, %object
    .size    g_ble_connection_table, 4
g_ble_connection_table:
    .word    g_ble_connection_table_storage

/* LCG state + lock pair (slots populated by stack init at runtime). */
    .section .bss.g_lcg_state, "aw", %nobits
    .weak    g_lcg_state
    .type    g_lcg_state, %object
    .size    g_lcg_state, 4
g_lcg_state:
    .skip    4

    .section .data.g_lcg_lock_enter, "aw", %progbits
    .weak    g_lcg_lock_enter
    .type    g_lcg_lock_enter, %object
    .size    g_lcg_lock_enter, 4
g_lcg_lock_enter:
    .word    0

    .section .data.g_lcg_lock_exit, "aw", %progbits
    .weak    g_lcg_lock_exit
    .type    g_lcg_lock_exit, %object
    .size    g_lcg_lock_exit, 4
g_lcg_lock_exit:
    .word    0

/* ===== bluetoothtask_post.c + ti_rtos_msgq.c helpers =====
 *
 * `ble_post_disconnect` is now a real strong symbol in
 * src/bluetoothtask_post.c; `task_queue_enqueue_and_signal` is now
 * a real strong symbol in src/ti_rtos_msgq.c. ROM-thunk pass-throughs
 * remain weak until they get wired to the actual SDK. */
    WEAK_NOOP(ti_queue_put)
    WEAK_NOOP(ti_event_post)

/* ===== ti_rtos_heap.c =====
 *
 * The OEM `g_heap_state` pointer lives at flash 0x0001355C and at
 * 0x00021BC8 (both literal slots hold the same value 0x20004E0C —
 * the heap-state struct base in RAM). Weak BSS placeholder so the
 * skeleton links; real placement comes from a future cinit-walker /
 * heap-init decomp. */
    .section .bss.g_heap_state_storage, "aw", %nobits
    .weak    g_heap_state_storage
    .type    g_heap_state_storage, %object
    .size    g_heap_state_storage, 0x40
g_heap_state_storage:
    .skip    0x40

    .section .data.g_heap_state, "aw", %progbits
    .weak    g_heap_state
    .type    g_heap_state, %object
    .size    g_heap_state, 4
g_heap_state:
    .word    g_heap_state_storage

/* Bluetoothtask user-message queue control struct — pointer global
 * at flash 0x00021078 in the OEM, holding 0x200057C8. Weak BSS
 * placeholder so the skeleton links; real placement comes from a
 * future bluetoothtask-init decomp. */
    .section .bss.g_bluetoothtask_msg_queue_storage, "aw", %nobits
    .weak    g_bluetoothtask_msg_queue_storage
    .type    g_bluetoothtask_msg_queue_storage, %object
    .size    g_bluetoothtask_msg_queue_storage, 12
g_bluetoothtask_msg_queue_storage:
    .skip    12

    .section .data.g_bluetoothtask_msg_queue, "aw", %progbits
    .weak    g_bluetoothtask_msg_queue
    .type    g_bluetoothtask_msg_queue, %object
    .size    g_bluetoothtask_msg_queue, 4
g_bluetoothtask_msg_queue:
    .word    g_bluetoothtask_msg_queue_storage
/* `module_publish_command`, `ble_authenticated_connection_count`,
 * `runtime_permission_mask`,
 * `block_dispatch_queue_post`, `FUN_00024740`, `ble_connection_get_session_key`,
 * `gatt_config_lookup_item`, `monitor_log`, `module_forward_async`,
 * `oad_gatt_write_handler`, `log_gatt_write_handler`,
 * `gatt_handle_backoffice_message_data`, `FUN_00027448` (=
 * `timekeeper_read_be`), `memcpy` already have weak entries from
 * earlier passes. */

/* ICall entity registry — pointer global at flash 0x00020C9C in the
 * OEM, holding 0x20004E78 (RAM array of 6×12-byte records). Weak BSS
 * placeholder so the skeleton links; real placement comes from a
 * future stack-init decomp. */
    .section .bss.g_icall_entities_storage, "aw", %nobits
    .weak    g_icall_entities_storage
    .type    g_icall_entities_storage, %object
    .size    g_icall_entities_storage, 0x48
g_icall_entities_storage:
    .skip    0x48

    .section .data.g_icall_entities, "aw", %progbits
    .weak    g_icall_entities
    .type    g_icall_entities, %object
    .size    g_icall_entities, 4
g_icall_entities:
    .word    g_icall_entities_storage

/* ===== gatt_read.c helpers =====
 * `ble_connection_get_session_key` now strong in src/ble_connection.c. */
    WEAK_NOOP(gatt_config_lookup_item)
    WEAK_NOOP(runtime_permission_mask)
    WEAK_NOOP(module_publish_sync_with_timeout)
    WEAK_NOOP(module_bus_is_idle)
    WEAK_NOOP(aes128_block_encrypt)
    WEAK_NOOP(gatt_scratch_alloc)
    WEAK_NOOP(gatt_scratch_free)
    WEAK_NOOP(log_block_count_get)
    WEAK_NOOP(log_total_size_byte)
/* `timekeeper_read_be` (FUN_00027448) is now strong in src/timekeeper.c. */
    WEAK_NOOP(ble_conn_state_byte)
    WEAK_NOOP(backoffice_status_u16)
    WEAK_NOOP(ecc_sign_with_factory_key)
    WEAK_NOOP(trng_fill_16)

    .section .rodata.g_ecc_curve_params, "a", %progbits
    .weak    g_ecc_curve_params
    .type    g_ecc_curve_params, %object
    .size    g_ecc_curve_params, 20
g_ecc_curve_params:
    .skip    20

/* ===== log_gatt.c helpers ===== */
    WEAK_NOOP(log_gatt_notify_channel)
    WEAK_NOOP(att_mtu_clamp)
    /* log_read_entry, log_seek_to_timestamp now real in src/log_gatt.c
     * (strong symbols override these weak entries from earlier passes). */
    WEAK_NOOP(monitor_strtol)
    WEAK_NOOP(ti_semaphore_pend)
    WEAK_NOOP(extflash_read)

    .section .bss.g_log_state, "aw", %nobits
    .weak    g_log_state
    .type    g_log_state, %object
    .size    g_log_state, 0x10
g_log_state:
    .skip    0x10

    .section .bss.g_log_control_scratch, "aw", %nobits
    .weak    g_log_control_scratch
    .type    g_log_control_scratch, %object
    .size    g_log_control_scratch, 0x10
g_log_control_scratch:
    .skip    0x10

    .section .bss.g_log_readout_buffer, "aw", %nobits
    .weak    g_log_readout_buffer
    .type    g_log_readout_buffer, %object
    .size    g_log_readout_buffer, 0xF0
g_log_readout_buffer:
    .skip    0xF0

    .section .bss.g_log_last_count, "aw", %nobits
    .weak    g_log_last_count
    .type    g_log_last_count, %object
    .size    g_log_last_count, 1
g_log_last_count:
    .skip    1

/* Backoffice GATT reassembly buffer pointer — OEM `DAT_000041F8`,
 * stored value `0x2000A248`. Weak BSS placeholder so the skeleton
 * links; the real pointer is populated by the GATT-service init. */
    .section .bss.g_backoffice_reassembly, "aw", %nobits
    .weak    g_backoffice_reassembly
    .type    g_backoffice_reassembly, %object
    .size    g_backoffice_reassembly, 4
g_backoffice_reassembly:
    .skip    4
