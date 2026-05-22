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
    WEAK_NOOP(monitor_alloc)
    WEAK_NOOP(monitor_strlen)
    WEAK_NOOP(log_submit)
    WEAK_NOOP(monitor_free)
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
    WEAK_NOOP(FUN_00024740)

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
    WEAK_NOOP(module_publish_command)

/* YModem target-offset pointer — OEM `*DAT_0000CC70`, a u32 in RAM
 * that cmd_audio_upload writes into before invoking ymodem_receive. */
    .section .bss.g_ymodem_target_offset, "aw", %nobits
    .weak    g_ymodem_target_offset
    .type    g_ymodem_target_offset, %object
    .size    g_ymodem_target_offset, 4
g_ymodem_target_offset:
    .skip    4

/* ===== gatt_read.c helpers ===== */
    WEAK_NOOP(gatt_config_lookup_item)
    WEAK_NOOP(ble_connection_get_session_key)
    WEAK_NOOP(runtime_permission_mask)
    WEAK_NOOP(module_publish_sync_with_timeout)
    WEAK_NOOP(module_bus_is_idle)
    WEAK_NOOP(aes128_block_encrypt)
    WEAK_NOOP(gatt_scratch_alloc)
    WEAK_NOOP(gatt_scratch_free)
    WEAK_NOOP(log_block_count_get)
    WEAK_NOOP(log_total_size_byte)
    WEAK_NOOP(timekeeper_read_be)
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
    WEAK_NOOP(log_read_entry)
    WEAK_NOOP(log_seek_to_timestamp)
    WEAK_NOOP(log_parse_ascii_uint)
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
