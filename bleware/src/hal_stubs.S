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
