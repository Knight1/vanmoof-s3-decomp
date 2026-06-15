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

    /* WEAK_NOOP(monitor_log) -- now in stubs.c */
    /* WEAK_NOOP(monitor_print_help_line) — now strong in dispatcher.c */
    /* WEAK_NOOP(memcpy) -- now strong in runtime.c */
    /* WEAK_NOOP(memset) -- now strong in runtime.c */
/* `monitor_alloc` and `monitor_free` are now real strong symbols in
 * src/ti_rtos_heap.c. Earlier WEAK_NOOP entries removed. */
    /* WEAK_NOOP(nvs_open) — now strong in gatt_read.c */
    /* WEAK_NOOP(extflash_open) — now strong in extflash.c */
    /* WEAK_NOOP(extflash_close) — now strong in extflash.c */
    /* WEAK_NOOP(extflash_retry_backoff) — now strong in extflash.c */
    /* WEAK_NOOP(extflash_get_chip_info) -- now strong */
    /* WEAK_NOOP(extflash_sw_wp_enabled) — now strong in extflash.c */
    /* WEAK_NOOP(extflash_block_wp_enabled) — now strong in extflash.c */
    /* WEAK_NOOP(att_mtu_clamp) — now strong in ble_connection.c */

/* ===== Monitor command helper fallbacks ===== */
    /* WEAK_NOOP(monitor_command_matches) -- now strong */
    /* WEAK_NOOP(monitor_sscanf) -- now in stubs.c */
    /* WEAK_NOOP(monitor_sleep) -- now in stubs.c */
    /* WEAK_NOOP(firmware_update_start) -- now strong */
    /* WEAK_NOOP(print_firmware_info) -- now in stubs.c */
    /* WEAK_NOOP(log_block_count) -- now strong */
    /* WEAK_NOOP(log_format_block) -- now strong */
    /* WEAK_NOOP(monitor_strlen) -- now in stubs.c */
    /* WEAK_NOOP(log_submit) -- now strong */
    /* WEAK_NOOP(monitor_snprintf) -- now in stubs.c */
    /* WEAK_NOOP(packfs_open) -- now in stubs.c */
    /* WEAK_NOOP(packfs_next) -- now in stubs.c */
    /* WEAK_NOOP(packfs_close) -- now in stubs.c */
    /* WEAK_NOOP(format_size) -- now strong */
    /* WEAK_NOOP(extflash_erase) — now strong in extflash.c (extflash_erase_range) */
    /* WEAK_NOOP(extflash_read) -- already strong */
    /* WEAK_NOOP(extflash_sector_write) — now strong in extflash.c */
    /* WEAK_NOOP(crc32_le) -- already strong */
    /* WEAK_NOOP(memcmp) -- now strong in runtime.c */
    /* WEAK_NOOP(block_dispatch_queue_post) — now strong in auth.c */

/* RAM globals owned by the M-Key cache (OEM struct around
 * `DAT_000195FC = 0x2000A3DC`). Defined here as weak BSS bytes so the
 * skeleton links; the real RAM lives in xs3_gatt_backoffice.c. */
    .section .bss.g_mkey_working_buffer, "aw", %nobits
    .weak    g_mkey_working_buffer
    .type    g_mkey_working_buffer, %object
    .size    g_mkey_working_buffer, 4
g_mkey_working_buffer:
    .skip    4

/* OEM DAT_00026068 — a flash literal holding 0x2000A3DC, the base of
 * the M-Key owned struct. Used by runtime_permission_mask to check if
 * the buffer is zero-filled (uninitialized) or has a valid mask at +0x14. */
    .section .data.g_mkey_ptr, "aw", %progbits
    .weak    g_mkey_ptr
    .type    g_mkey_ptr, %object
    .size    g_mkey_ptr, 4
g_mkey_ptr:
    .word    0x2000A3DC

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
    /* WEAK_NOOP(ble_connection_count) — now strong in ble_connection.c */
    /* WEAK_NOOP(ble_connection_present) — now strong in ble_connection.c */
    /* WEAK_NOOP(ble_connection_addr) — now strong in ble_connection.c */
    /* WEAK_NOOP(ble_connection_params) — now strong in ble_connection.c */
    /* WEAK_NOOP(ble_connection_is_rider_app) — now strong in ble_connection.c */
    /* WEAK_NOOP(ble_device_address) — now strong in ble_connection.c */

/* ===== Backoffice GATT message handler — sub-command helpers =====
 *
 * Decoded plate-comments live in src/provisioning.c. Each gets its own
 * real .c body as it lands. The current weak no-ops let the skeleton
 * link with provisioning.o present. */
    /* WEAK_NOOP(mkey_record_write_slot126) — now strong in provisioning.c */
    /* WEAK_NOOP(backoffice_ack_noop) — now strong in provisioning.c */
    /* WEAK_NOOP(secrets_delete_by_key) — now strong in secrets.c */
    /* WEAK_NOOP(module_forward_async) -- already strong */
    /* WEAK_NOOP(secrets_sector_erase) — now strong in secrets.c */
    /* WEAK_NOOP(module_forward_sync) — now strong in protocols/ssp.c */
    /* WEAK_NOOP(ble_connection_is_active) — now strong in ble_connection.c */
    /* WEAK_NOOP(ble_connection_touch) — now strong in ble_connection.c */
    /* WEAK_NOOP(backoffice_on_success_hook) — now strong in ble_connection.c */
    /* WEAK_NOOP(gatt_notify_channel) — now strong in gatt_notify.c */
    /* WEAK_NOOP(crc16_modbus) — now strong in crc32.c */

/* ===== OAD helpers (src/oad.c) ===== */
/* OAD helpers now strong in src/oad.c. */
    /* WEAK_NOOP(oad_state_lock) */
    /* WEAK_NOOP(bleware_control_event_post) */
    /* WEAK_NOOP(oad_session_close) */
    /* WEAK_NOOP(extflash_erase_range) -- already strong */
    /* WEAK_NOOP(extflash_write) -- already strong */
    /* WEAK_NOOP(ti_semaphore_post) -- now ROM alias */
/* `FUN_00024740` is now strong as `mfg_key_ecb_decrypt_chunks` in
 * src/auth.c.
 * `state_machine_post` is now strong in src/state_machine.c;
 * `secrets_count_valid_in_keys_range` is now strong in src/secrets.c. */
/* Inner publish helper used by state_machine_post. Now strong in
 * bluetoothtask_post.c. */
    /* WEAK_NOOP(task_queue_publish_envelope) — now strong */
/* Firmware abort now strong in provisioning.c. */
    /* WEAK_NOOP(firmware_abort) — now strong */

    /* WEAK_NOOP(FUN_00027766) -- now strong */
    /* WEAK_NOOP(FUN_0002776a) -- now strong */
/* ROM Clock thunks (CONFIRMED via SimpleLink SDK 3.40 cc26x2v2 golden ROM
 * symbol table — NOT Semaphore/Hwi as earlier guessed):
 * thunk_EXT_FUN_1002ecc2 = ti_sysbios_knl_Clock_setTimeout  (0x1002ECC2)
 * thunk_EXT_FUN_1002e2c4 = ti_sysbios_knl_Clock_stop        (0x1002E2C4)
 * thunk_EXT_FUN_1002e9e6 = ti_sysbios_knl_Clock_start       (0x1002E9E6)
 * Used by the clock-reconfigure helper at 0x20098 (decoded as `oad_state_lock`
 * — itself a mislabel) and by clock_arm/clock_disarm/advert_keepalive_pulse.
 * See docs/rom-thunk-audit.md. */
    /* WEAK_NOOP(thunk_FUN_1002ecc2) -- now ROM alias */
    /* WEAK_NOOP(thunk_FUN_1002e2c4) -- now ROM alias */
    /* WEAK_NOOP(thunk_FUN_1002e9e6) -- now ROM alias */
/* FUN_000274e8 — TI SPI driver transfer primitive */
    /* WEAK_NOOP(FUN_000274e8) -- now strong */
    .section .bss.g_oad_state, "aw", %nobits
    .weak    g_oad_state
    .type    g_oad_state, %object
    .size    g_oad_state, 0x68
g_oad_state:
    .skip    0x68

/* OAD state globals used by oad_state_lock and oad_session_close:
 * DAT_000200E8 = pointer to tick period (0x0002BB88), DAT_00025084 =
 * pointer to OAD session struct (0x20009A90), DAT_000265D8 = control
 * event tag literal (0x0002752F). */
    .section .data.g_tick_period_ptr, "aw", %progbits
    .weak    g_tick_period_ptr
    .type    g_tick_period_ptr, %object
    .size    g_tick_period_ptr, 4
g_tick_period_ptr:
    .word    0x0002BB88

    .section .data.g_oad_conn_handle, "aw", %progbits
    .weak    g_oad_conn_handle
    .type    g_oad_conn_handle, %object
    .size    g_oad_conn_handle, 4
g_oad_conn_handle:
    .word    0x20009A90

    .section .data.g_oad_sem_handle, "aw", %progbits
    .weak    g_oad_sem_handle
    .type    g_oad_sem_handle, %object
    .size    g_oad_sem_handle, 4
g_oad_sem_handle:
    .word    0x20009A94

    .section .data.g_ble_control_tag, "aw", %progbits
    .weak    g_ble_control_tag
    .type    g_ble_control_tag, %object
    .size    g_ble_control_tag, 4
g_ble_control_tag:
    .word    0x0002752F

/* Provisioning globals:
 * DAT_000222E8 = "MKEY" tag LE word, DAT_0001F848 = abort flag in RAM */
    .section .data.g_mkey_tag, "aw", %progbits
    .weak    g_mkey_tag
    .type    g_mkey_tag, %object
    .size    g_mkey_tag, 4
g_mkey_tag:
    .word    0x59454B4D   /* "MKEY" little-endian */

    .section .bss.g_abort_flag, "aw", %nobits
    .weak    g_abort_flag
    .type    g_abort_flag, %object
    .size    g_abort_flag, 4
g_abort_flag:
    .skip    4

/* Firmware abort — platform strings referenced by firmware_abort */
    .section .rodata.s_Platform_reset, "a", %progbits
    .weak    s_Platform_reset
    .type    s_Platform_reset, %object
s_Platform_reset:
    .string "Platform reset"

    .section .rodata.s_source_xs3_app_c, "a", %progbits
    .weak    s_source_xs3_app_c
    .type    s_source_xs3_app_c, %object
s_source_xs3_app_c:
    .string "source/xs3_app.c"

/* TI-RTOS tick-period word — OEM `PTR_DAT_0000BA50`. */
    .section .bss.g_ti_tick_period_us, "aw", %nobits
    .weak    g_ti_tick_period_us
    .type    g_ti_tick_period_us, %object
    .size    g_ti_tick_period_us, 4
g_ti_tick_period_us:
    .skip    4

/* ===== extflash.c helpers =====
 * extflash_cs_assert, extflash_cs_deassert, extflash_wait_wip_clear,
 * extflash_write_enable are now strong in extflash.c. */
    /* WEAK_NOOP(extflash_cs_assert) */
    /* WEAK_NOOP(extflash_cs_deassert) */
    /* WEAK_NOOP(extflash_spi_tx) — now strong in extflash.c */
    /* WEAK_NOOP(extflash_spi_rx) — now strong in extflash.c */
    /* WEAK_NOOP(extflash_wait_wip_clear) */
    /* WEAK_NOOP(extflash_write_enable) */
    /* WEAK_NOOP(gpio_write) */

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
    /* WEAK_NOOP(log_region_erase) — now strong in log_gatt.c */
    /* WEAK_NOOP(log_writer_restart) — now strong in log_gatt.c */
    /* WEAK_NOOP(rtos_mem_get_stats) -- now in stubs.c */
    /* WEAK_NOOP(monitor_key_wait_with_timeout) -- now in stubs.c */
    /* WEAK_NOOP(monitor_yield_ticks) -- now in stubs.c */
    /* WEAK_NOOP(snv_free_space_query) -- now in stubs.c */
    /* WEAK_NOOP(snv_compact) -- now in stubs.c */
    /* WEAK_NOOP(system_state_save) -- now in stubs.c */
    /* WEAK_NOOP(system_power_down) -- now in stubs.c */
    /* WEAK_NOOP(system_software_reset) -- now in stubs.c */
    /* WEAK_NOOP(ble_gap_terminate_link) -- ROM thunk, vendored from TI SDK */
    /* WEAK_NOOP(ymodem_receive) -- now in stubs.c */
    /* WEAK_NOOP(pack_ingest_start) -- now in stubs.c */
    /* WEAK_NOOP(pack_upload_finalize) -- now in stubs.c */
    /* WEAK_NOOP(audio_clip_dump_one) -- now in stubs.c */
    /* WEAK_NOOP(audio_player_play) -- now in stubs.c */
    /* WEAK_NOOP(audio_player_stop_or_pause) -- now in stubs.c */
/* `module_publish_command`, `module_forward_async`,
 * `module_publish_sync_with_timeout`, `ble_authenticated_connection_count`,
 * `ble_activity_led_pulse`, `ssp_signal_fetch`, `ssp_queue_publish_frame`,
 * `ssp_publish_fetch_frame` are real strong symbols in
 * src/protocols/ssp.c — no weak stub needed. */

    /* WEAK_NOOP(ti_mailbox_post) -- now ROM alias */
    /* WEAK_NOOP(ssp_frame_alloc) -- now ROM alias */
    /* WEAK_NOOP(gpio_write) — now static in extflash.c */
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

/* SSP bus-idle pending-cmd pointer — OEM DAT_000265A8 = 0x20009A90.
 * Points to the uint16_t "pending command" field of whatever module
 * is currently the bus-master target. 0xFFFF means idle. */
    .section .data.g_ssp_bus_pending_cmd_ptr, "aw", %progbits
    .weak    g_ssp_bus_pending_cmd_ptr
    .type    g_ssp_bus_pending_cmd_ptr, %object
    .size    g_ssp_bus_pending_cmd_ptr, 4
g_ssp_bus_pending_cmd_ptr:
    .word    0x20009A90

    .section .bss.g_bleware_gpio_ctx, "aw", %nobits
    .weak    g_bleware_gpio_ctx
    .type    g_bleware_gpio_ctx, %object
    .size    g_bleware_gpio_ctx, 4
g_bleware_gpio_ctx:
    .skip    4

/* GPIO state globals used by gpio_write (static in extflash.c):
 * OEM DAT_00022E38 = 0x20005A78 (max pin), DAT_00022E3C = 0x2000A2E8
 * (base-address table), DAT_00022E40 = 0x400220E0 (DOUT base). */
    .section .bss.g_gpio_max_pin, "aw", %nobits
    .weak    g_gpio_max_pin
    .type    g_gpio_max_pin, %object
    .size    g_gpio_max_pin, 4
g_gpio_max_pin:
    .skip    4

    .section .data.g_gpio_dout_base, "aw", %progbits
    .weak    g_gpio_dout_base
    .type    g_gpio_dout_base, %object
    .size    g_gpio_dout_base, 4
g_gpio_dout_base:
    .word    0x400220E0

/* Extflash CS gpio_ctx — OEM DAT_00026F90/DAT_00026FA0 both = 0x20001B34 */
    .section .bss.g_extflash_gpio_ctx, "aw", %nobits
    .weak    g_extflash_gpio_ctx
    .type    g_extflash_gpio_ctx, %object
    .size    g_extflash_gpio_ctx, 4
g_extflash_gpio_ctx:
    .skip    4

/* Extflash opcode bytes — sourced from driver-header table at 0x0002A450.
 * DAT_000217A8 = 0x0002A451 = rdsr_cmd (0x05), DAT_000244A4 = 0x0002A451
 * same table, byte +1 = rdsr, +2 = wren. */
    .section .data.g_extflash_rdsr_cmd, "aw", %progbits
    .weak    g_extflash_rdsr_cmd
    .type    g_extflash_rdsr_cmd, %object
    .size    g_extflash_rdsr_cmd, 1
g_extflash_rdsr_cmd:
    .byte    0x05

    .section .data.g_extflash_wren_cmd, "aw", %progbits
    .weak    g_extflash_wren_cmd
    .type    g_extflash_wren_cmd, %object
    .size    g_extflash_wren_cmd, 1
g_extflash_wren_cmd:
    .byte    0x06

/* SPI driver handle — OEM DAT_00024474 - 0xC = DAT_00024444 - 0xC,
 * both resolve through g_extflash_state at 0x20001B34 - 0xC = 0x20001B28 */
    .section .bss.g_extflash_spi_handle, "aw", %nobits
    .weak    g_extflash_spi_handle
    .type    g_extflash_spi_handle, %object
    .size    g_extflash_spi_handle, 4
g_extflash_spi_handle:
    .skip    4

/* Retry/backoff context pointer — OEM `*DAT_00025BA0`, a pointer in RAM
 * to a {u8 give_up_flag @0, u32 clock_handle @4} block read by
 * extflash_retry_backoff. */
    .section .bss.g_extflash_retry_ctx, "aw", %nobits
    .weak    g_extflash_retry_ctx
    .type    g_extflash_retry_ctx, %object
    .size    g_extflash_retry_ctx, 4
g_extflash_retry_ctx:
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
    /* WEAK_NOOP(svc_5560_char_uuid_to_index) -- now strong in gatt_uuid_matchers.c */
    /* WEAK_NOOP(svc_5510_char_uuid_to_index) -- now strong in gatt_uuid_matchers.c */
    /* WEAK_NOOP(svc_5520_char_uuid_to_index) -- now strong in gatt_uuid_matchers.c */
    /* WEAK_NOOP(svc_5530_char_uuid_to_index) -- now strong in gatt_uuid_matchers.c */
    /* WEAK_NOOP(svc_5570_char_uuid_to_index) -- now strong in gatt_uuid_matchers.c */
    /* WEAK_NOOP(svc_5590_char_uuid_to_index) -- now strong in gatt_uuid_matchers.c */
    /* WEAK_NOOP(svc_55a0_char_uuid_to_index) -- now strong in gatt_uuid_matchers.c */
    /* WEAK_NOOP(svc_55c0_char_uuid_to_index) -- now strong in gatt_uuid_matchers.c */
    /* WEAK_NOOP(vanmoof_ssp_uuid_to_index) -- now strong in gatt_uuid_matchers.c */
    /* WEAK_NOOP(cccd_write_validate) -- now strong */

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
    /* WEAK_NOOP(icall_send_service_msg) -- ROM thunk, vendored from TI SDK */
    /* WEAK_NOOP(icall_wait_match) -- ROM thunk, vendored from TI SDK */
    /* WEAK_NOOP(icall_free_message) -- ROM thunk, vendored from TI SDK */
    /* WEAK_NOOP(bios_get_thread_state) -- now ROM alias */
    /* WEAK_NOOP(log_reply_match_pred) -- ROM thunk, vendored from TI SDK */

/* ===== icall_runtime.c + ti_rtos_kernel.c ROM-thunk pass-throughs ===== */
    /* WEAK_NOOP(ti_task_self) -- now ROM alias */
    /* WEAK_NOOP(bios_hwi_disable) -- now ROM alias */
    /* WEAK_NOOP(bios_task_disable) -- now ROM alias */
    /* WEAK_NOOP(bios_hwi_restore) -- now ROM alias */
    /* WEAK_NOOP(bios_task_restore) -- now ROM alias */
    /* WEAK_NOOP(abort_continuation) -- now ROM alias */

/* ===== gatt_write.c helpers ===== */
/* `FUN_00021884` (→ `ssp_relay_u32`), `FUN_00023204` (→ `ssp_relay_u16`)
 * now strong in src/ssp_relay.c; `FUN_00026cc0` (→ `timekeeper_submit_epoch`)
 * now strong in src/timekeeper.c.
 * `auth_derive_session_key` is now strong in src/auth.c;
 * `backoffice_auth_session_init` in src/ble_connection.c. */
/* `timekeeper_apply_request` (FUN_00020B18) + `sysclock_snapshot` are
 * now strong in src/timekeeper.c. Its leaf helpers remain stubbed: */
    /* WEAK_NOOP(sysclock_snapshot) -- now strong in timekeeper.c */
    /* WEAK_NOOP(timekeeper_read_request) — now strong in timekeeper.c */

/* Heap-resident timekeeper shared state + tick-modulus constant. */
    .section .bss.g_timekeeper_state, "aw", %nobits
    .weak    g_timekeeper_state
    .type    g_timekeeper_state, %object
    .size    g_timekeeper_state, 4
g_timekeeper_state:
    .zero    4

    .section .data.g_timekeeper_tick_modulus, "aw", %progbits
    .weak    g_timekeeper_tick_modulus
    .type    g_timekeeper_tick_modulus, %object
    .size    g_timekeeper_tick_modulus, 4
g_timekeeper_tick_modulus:
    .word    0x3B9ACA00   /* 1 000 000 000 — OEM @ 0x0002371C */

/* ROM clock function-pointer table — OEM @ 0x00023718 = 0x1000018C.
 * Points into the TI-RTOS ROM API table at offset +0x30 (12 bytes in).
 * sysclock_snapshot uses this to call ClockP_getTicks or equivalent. */
    .section .data.g_rom_clock_table, "aw", %progbits
    .weak    g_rom_clock_table
    .type    g_rom_clock_table, %object
    .size    g_rom_clock_table, 4
g_rom_clock_table:
    .word    0x1000018C

/* Same tick-modulus constant used by timekeeper_apply_request for wrap
 * handling (OEM DAT_00020B64 — also 0x3B9ACA00). Timekeeper state uses
 * this for the modulus when tick-rollover is detected. */
    .section .data.g_tk_tick_modulus2, "aw", %progbits
    .weak    g_tk_tick_modulus2
    .type    g_tk_tick_modulus2, %object
    .size    g_tk_tick_modulus2, 4
g_tk_tick_modulus2:
    .word    0x3B9ACA00

/* ===== TI-RTOS ROM thunk aliases ===================================
 *
 * Many C sources declare `extern ti_semaphore_pend(...)` etc. as
 * shorthand for the ROM functions at 0x1002xxxx. Provide weak ASM
 * aliases that resolve to the same thunk bodies already defined at
 * the end of this file. */

#define ROM_ALIAS2(alias, thunk_name) \
    .section .text.##alias, "ax", %progbits ; \
    .weak    alias ; .type alias, %function ; .thumb_func ; \
alias: ; b.w thunk_name ; .size alias, .-alias

#define ROM_ALIAS1(alias, thunk_name) ROM_ALIAS2(alias, thunk_name)
#define ROM_ALIAS0(alias, thunk_name) ROM_ALIAS2(alias, thunk_name)

/* Semaphore_pend = ROM 0x1002BFB0, Semaphore_post = ROM 0x1002CD20 */
    ROM_ALIAS2(ti_semaphore_pend, thunk_EXT_FUN_1002bfb0)
    ROM_ALIAS0(ti_semaphore_post, thunk_EXT_FUN_1002cd20)

/* Queue_put = ROM 0x1002D8D8, Event_post = ROM 0x1002E8FC */
    ROM_ALIAS2(ti_queue_put, thunk_EXT_FUN_1002d8d8)
    ROM_ALIAS2(ti_event_post, thunk_EXT_FUN_1002e8fc)

/* Mailbox_post = ROM 0x1002D91C */
    ROM_ALIAS2(ti_mailbox_post, thunk_EXT_FUN_1002d91c)

/* BIOS_start = ROM 0x1002EAA4, Task_self = ROM 0x1002EB54 */
    ROM_ALIAS0(BIOS_start_alias, thunk_EXT_FUN_1002eaa4)
    ROM_ALIAS0(ti_task_self, thunk_EXT_FUN_1002eb54)

/* Hwi_disable = ROM 0x1002EA24, Hwi_restore = ROM 0x1002E474 */
    ROM_ALIAS0(bios_hwi_disable, thunk_EXT_FUN_1002ea24)
    ROM_ALIAS0(bios_hwi_restore, thunk_EXT_FUN_1002e474)

/* Task_disable = ROM 0x1002EB54 (same as Task_self? different slot),
 * Task_restore uses different entry */
    ROM_ALIAS0(bios_task_disable, thunk_EXT_FUN_1002eb54)
    ROM_ALIAS0(bios_task_restore, thunk_EXT_FUN_1002e9fc)

/* Thread-state query */
    ROM_ALIAS0(bios_get_thread_state, thunk_EXT_FUN_1002e648)

/* SSP frame alloc = ROM 0x1002DB60 */
    ROM_ALIAS0(ssp_frame_alloc, thunk_EXT_FUN_1002db60)

/* GAP ROM functions (used by gap_event_handler.c) */
    ROM_ALIAS1(rom_gap_establish_link,             thunk_EXT_FUN_10019188)
    ROM_ALIAS1(rom_gap_update_link_param,          thunk_EXT_FUN_10021d10)
    ROM_ALIAS1(rom_gap_config_conn_params,          thunk_EXT_FUN_10025464)
    ROM_ALIAS1(rom_gap_set_param,                   thunk_EXT_FUN_10022c94)
    ROM_ALIAS1(rom_gap_get_param,                   thunk_EXT_FUN_1001bd20)
    ROM_ALIAS1(rom_gap_send_msg,                    thunk_EXT_FUN_100157d8)
    ROM_ALIAS1(rom_gap_process_event,               thunk_EXT_FUN_1001c234)
    ROM_ALIAS1(rom_gap_update_link_param_req_reply,  thunk_EXT_FUN_1001f030)
    ROM_ALIAS1(rom_gap_terminate_link_req,           thunk_EXT_FUN_10025d68)
    ROM_ALIAS1(queue_post_message,                   thunk_EXT_FUN_1000e49c)

/* OAD helpers (used by oad.c) */
    ROM_ALIAS1(thunk_FUN_1002ecc2, thunk_EXT_FUN_1002ecc2)
    ROM_ALIAS1(thunk_FUN_1002e2c4, thunk_EXT_FUN_1002e2c4)
    ROM_ALIAS1(thunk_FUN_1002e9e6, thunk_EXT_FUN_1002e9e6)

/* ICall inter-task message passing (ROM) */
    ROM_ALIAS0(icall_send_service_msg, thunk_EXT_FUN_1002f738)
    ROM_ALIAS0(icall_free_message,     thunk_EXT_FUN_1002f7e8)

/* Crypto / TRNG (ROM) */
    ROM_ALIAS0(aes128_block_encrypt,   thunk_EXT_FUN_1002f366)
    ROM_ALIAS0(trng_fill_16,           thunk_EXT_FUN_1002fdda)

/* BLE GAP terminate link (ROM) */
    ROM_ALIAS0(ble_gap_terminate_link, thunk_EXT_FUN_1002df44)

/* Clock_getTicks-style monotonic counter = ROM 0x1002DC10 (used by
 * ble_connection_touch to stamp a per-connection keep-alive timestamp). */
    ROM_ALIAS0(rom_clock_get_ticks, thunk_EXT_FUN_1002dc10)

/* GAP_GetDevAddress = ROM 0x100221C4 (used by ble_device_address). */
    ROM_ALIAS1(rom_gap_get_dev_address, thunk_EXT_FUN_100221c4)

/* Semaphore_construct / _destruct / Params_init = ROM 0x1002DDC0 /
 * 0x1002E6FC / 0x1002EBE4 (used by module_forward_sync's per-call
 * local reply semaphore). */
    ROM_ALIAS2(ti_semaphore_construct,   thunk_EXT_FUN_1002ddc0)
    ROM_ALIAS0(ti_semaphore_destruct,    thunk_EXT_FUN_1002e6fc)
    ROM_ALIAS2(ti_semaphore_params_init, thunk_EXT_FUN_1002ebe4)

/* SSP internal flash helpers (this binary, not ROM) used by
 * module_forward_sync — weak no-ops until decoded:
 *   ssp_sync_reply_route     OEM 0x00027054 (reply-callback pointer)
 *   ssp_dequeue_frame_by_seq OEM 0x00021ABA (removes a frame by sequence) */
    WEAK_NOOP(ssp_sync_reply_route)
    WEAK_NOOP(ssp_dequeue_frame_by_seq)

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

/* Local BLE device address buffer (6 bytes), used by ble_device_address */
    .section .bss.g_ble_local_addr, "aw", %nobits
    .weak    g_ble_local_addr
    .type    g_ble_local_addr, %object
    .size    g_ble_local_addr, 6
g_ble_local_addr:
    .skip    6

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

/* Backoffice on-success tag — OEM DAT_00022C20 = "M-ID" LE */
    .section .data.g_backoffice_success_tag, "aw", %progbits
    .weak    g_backoffice_success_tag
    .type    g_backoffice_success_tag, %object
    .size    g_backoffice_success_tag, 4
g_backoffice_success_tag:
    .word    0x44492D4D   /* "M-ID" little-endian */

/* GATT notify channel state — RAM 0x200049CC (DAT_0001B5B0) + tag byte */
    .section .bss.g_gatt_notify_state, "aw", %nobits
    .weak    g_gatt_notify_state
    .type    g_gatt_notify_state, %object
    .size    g_gatt_notify_state, 20
g_gatt_notify_state:
    .skip    20

    .section .bss.g_gatt_notify_chan_tag, "aw", %nobits
    .weak    g_gatt_notify_chan_tag
    .type    g_gatt_notify_chan_tag, %object
    .size    g_gatt_notify_chan_tag, 1
g_gatt_notify_chan_tag:
    .skip    1

/* ===== bluetoothtask_post.c + ti_rtos_msgq.c helpers =====
 *
 * `ble_post_disconnect` is now a real strong symbol in
 * src/bluetoothtask_post.c; `task_queue_enqueue_and_signal` is now
 * a real strong symbol in src/ti_rtos_msgq.c. ROM-thunk pass-throughs
 * remain weak until they get wired to the actual SDK. */
    /* WEAK_NOOP(ti_queue_put) -- now ROM alias */
    /* WEAK_NOOP(ti_event_post) -- now ROM alias */

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
    /* WEAK_NOOP(gatt_config_lookup_item) -- now strong in gatt_config.c */
    /* WEAK_NOOP(runtime_permission_mask) — now strong in secrets.c */
    /* WEAK_NOOP(module_publish_sync_with_timeout) -- already strong */
    /* WEAK_NOOP(module_bus_is_idle) — now strong in protocols/ssp.c */
    /* WEAK_NOOP(aes128_block_encrypt) -- ROM thunk, vendored from TI SDK */
    /* WEAK_NOOP(gatt_scratch_alloc) — now strong in gatt_read.c */
    /* WEAK_NOOP(gatt_scratch_free) — now strong in gatt_read.c */
    /* WEAK_NOOP(backoffice_status_u16) — now strong in gatt_read.c */
    /* WEAK_NOOP(log_block_count_get) — now strong in log_gatt.c */
    /* WEAK_NOOP(log_total_size_byte) — now strong in log_gatt.c */
    /* WEAK_NOOP(ble_conn_state_byte) -- already strong */
    /* WEAK_NOOP(backoffice_status_u16) -- already strong */
    /* WEAK_NOOP(ecc_sign_with_factory_key) -- ROM thunk, vendored from TI SDK */
    /* WEAK_NOOP(trng_fill_16) -- ROM thunk, vendored from TI SDK */

    .section .rodata.g_ecc_curve_params, "a", %progbits
    .weak    g_ecc_curve_params
    .type    g_ecc_curve_params, %object
    .size    g_ecc_curve_params, 20
g_ecc_curve_params:
    .skip    20

/* ===== log_gatt.c helpers ===== */
    /* WEAK_NOOP(log_gatt_notify_channel) — now strong in log_gatt.c */
    /* WEAK_NOOP(att_mtu_clamp) -- already strong */
    /* log_read_entry, log_seek_to_timestamp now real in src/log_gatt.c
     * (strong symbols override these weak entries from earlier passes). */
    /* WEAK_NOOP(monitor_strtol) -- now strong in runtime.c */
    /* WEAK_NOOP(ti_semaphore_pend) -- now ROM alias */
    /* WEAK_NOOP(extflash_read) -- already strong */

    .section .bss.g_log_state, "aw", %nobits
    .weak    g_log_state
    .type    g_log_state, %object
    .size    g_log_state, 0x10
g_log_state:
    .skip    0x10

/* OEM DAT_000273e4 — flash literal holding 0x20005B6C. Pointer to a
 * single-byte log capacity field, read by log_total_size_byte. */
    .section .data.g_log_capacity_ptr, "aw", %progbits
    .weak    g_log_capacity_ptr
    .type    g_log_capacity_ptr, %object
    .size    g_log_capacity_ptr, 4
g_log_capacity_ptr:
    .word    0x20005B6C

/* Log notify channel state — OEM DAT_0001BA68 = 0x20004A00 area */
    .section .bss.g_log_notify_state, "aw", %nobits
    .weak    g_log_notify_state
    .type    g_log_notify_state, %object
    .size    g_log_notify_state, 20
g_log_notify_state:
    .skip    20

    .section .bss.g_log_notify_chan_tag, "aw", %nobits
    .weak    g_log_notify_chan_tag
    .type    g_log_notify_chan_tag, %object
    .size    g_log_notify_chan_tag, 1
g_log_notify_chan_tag:
    .skip    1

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

/* ===== gap_event_handler.c — ROM thunks and helpers =====
 *
 * These symbols are called by `gap_event_91_3e_handler` (OEM 0x00010B40).
 * They resolve to TI BLE5-Stack ROM (AGAMA R1) in the real firmware;
 * weak no-ops here until the SDK is vendored.
 */
    /* WEAK_NOOP(rom_gap_establish_link) -- now ROM alias */
    /* WEAK_NOOP(rom_gap_update_link_param) -- now ROM alias */
    /* WEAK_NOOP(rom_gap_config_conn_params) -- now ROM alias */
    /* WEAK_NOOP(rom_gap_set_param) -- now ROM alias */
    /* WEAK_NOOP(rom_gap_get_param) -- now ROM alias */
    /* WEAK_NOOP(rom_gap_send_msg) -- now ROM alias */
    /* WEAK_NOOP(rom_gap_process_event) -- now ROM alias */
    /* WEAK_NOOP(rom_gap_update_link_param_req_reply) -- now ROM alias */
    /* WEAK_NOOP(rom_gap_terminate_link_req) -- now ROM alias */
    /* WEAK_NOOP(gap_event_sub_dispatch) -- now ROM alias */
    /* WEAK_NOOP(queue_post_message) -- now ROM alias */

/* Destination service ID byte — OEM 0x00010C64 holds 0x2000566C.
 * The byte at that RAM address selects which ICall service receives
 * the gap-end-msg post. */
    .section .bss.s_dest_service_id, "aw", %nobits
    .weak    s_dest_service_id
    .type    s_dest_service_id, %object
    .size    s_dest_service_id, 1
s_dest_service_id:
    .skip    1

/* OAD-NVM1 header magic — OEM DAT_0000D5C8 = "OAD NVM1" */
    .section .rodata.g_oad_header_magic, "a", %progbits
    .weak    g_oad_header_magic
    .type    g_oad_header_magic, %object
    .size    g_oad_header_magic, 8
g_oad_header_magic:
    .ascii  "OAD NVM1"

/* ===== Newly-decoded module externs (xs3_app/statemachine/hci/gap_adv/
 * bond/buttonpress/util/audiotask) =====
 *
 * Weak fallbacks for callees and globals these TUs reference that are
 * not themselves decoded yet. Each is overridden by the real body/SDK
 * once it lands; in the stub build they are --gc-sections'd away. */

/* xs3_app.c: audiotask trigger — OEM FUN_000237C8. */
    WEAK_NOOP(audiotask_kick)

/* xs3_hci.c: per-connection PHY accessors (belong with ble_connection.c
 * once that record API is fully decoded) — OEM 0x22870 / 0x228F0 / 0x22050. */
    WEAK_NOOP(conn_phy_get_current)
    WEAK_NOOP(conn_phy_set_requested)
    WEAK_NOOP(conn_phy_reapply)

/* xs3_gap_adv.c helpers — OEM FUN_00022A30 / 0x00024FE8 / 0x00025010. */
    WEAK_NOOP(bleware_snprintf)
    WEAK_NOOP(gap_adv_disable_set1)
    WEAK_NOOP(gap_adv_disable_set2)

/* xs3_bond.c: all-bytes-equal predicate — OEM FUN_00025BFE (distinct from
 * secrets.c's static byte_range_all_equal @ 0x00026564). */
    WEAK_NOOP(bytes_all_equal)
    
    .section .bss.g_sm_context, "aw", %nobits
    .weak    g_sm_context
    .type    g_sm_context, %object
    .size    g_sm_context, 4
g_sm_context:
    .skip    4

/* xs3_gap_adv.c globals: adv-config struct (RAM 0x20005290) + random-
 * address command buffer (RAM 0x20005AF8). */
    .section .bss.g_gap_adv_cfg, "aw", %nobits
    .weak    g_gap_adv_cfg
    .type    g_gap_adv_cfg, %object
    .size    g_gap_adv_cfg, 0x60
g_gap_adv_cfg:
    .skip    0x60

    .section .bss.g_gap_adv_rand_addr, "aw", %nobits
    .weak    g_gap_adv_rand_addr
    .type    g_gap_adv_rand_addr, %object
    .size    g_gap_adv_rand_addr, 6
g_gap_adv_rand_addr:
    .skip    6

/* xs3_bond.c globals: address-mode label table (RAM 0x200058E0, 4 ptrs)
 * + last-bond-state latch (RAM 0x200058DC, 1 byte). */
    .section .bss.g_bond_addr_mode_strings, "aw", %nobits
    .weak    g_bond_addr_mode_strings
    .type    g_bond_addr_mode_strings, %object
    .size    g_bond_addr_mode_strings, 16
g_bond_addr_mode_strings:
    .skip    16

    .section .bss.g_bond_last_state, "aw", %nobits
    .weak    g_bond_last_state
    .type    g_bond_last_state, %object
    .size    g_bond_last_state, 1
g_bond_last_state:
    .skip    1

/* xs3_buttonpress.c: log-enable gate byte (RAM 0x20005670 — shares the
 * app-state block xs3_app.c addresses by literal; byte 0 here, audio
 * fields at +4/+0x14 there). */
    .section .bss.g_buttonpress_log_enabled, "aw", %nobits
    .weak    g_buttonpress_log_enabled
    .type    g_buttonpress_log_enabled, %object
    .size    g_buttonpress_log_enabled, 1
g_buttonpress_log_enabled:
    .skip    1

/* util.c: g_ctype_table is now materialized as a real const array in
 * src/util.c (read verbatim from OEM flash 0x00029B2C). */

/* monitor.c / console driver — the input parser (monitor_task_iteration
 * @ 0x13924 / monitor_readline @ 0x9E84), its tab-completion helper
 * (monitor_predict_command @ 0xC948), and the console-device driver
 * (src/monitor/console.c: monitor_console_read/_flush/_active +
 * monitor_event_signal) are now decoded; strcpy is strong in
 * src/runtime.c. The driver's lowest-level UART/RTOS leaves stay
 * undecoded and gc-section away with the (unrooted) console code, so no
 * weak fallbacks are needed for them.
 *
 * strlen: the application reaches ROM libc strlen (ROM 0x1002FDDA)
 * through the import veneer at flash 0x00028058 — represent it as a ROM
 * alias, not a no-op (a no-op returns 0 and would break the line editor's
 * up-arrow recall and tab completion). NOTE: trng_fill_16 above is also
 * aliased to thunk_EXT_FUN_1002fdda — a pre-existing mislabel, since
 * 0x1002FDDA is provably strlen (two call sites); trng_fill_16's true ROM
 * address still needs re-checking. */
    ROM_ALIAS0(strlen, thunk_EXT_FUN_1002fdda)

/* monitor console tick divisor (*0x0002BB88 == 10) + history word pointer
 * (RAM 0x2000457C). */
    .section .bss.g_console_tick_divisor, "aw", %nobits
    .weak    g_console_tick_divisor
    .type    g_console_tick_divisor, %object
    .size    g_console_tick_divisor, 4
g_console_tick_divisor:
    .skip    4

    .section .bss.g_monitor_history_word, "aw", %nobits
    .weak    g_monitor_history_word
    .type    g_monitor_history_word, %object
    .size    g_monitor_history_word, 4
g_monitor_history_word:
    .skip    4

