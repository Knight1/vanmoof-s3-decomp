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
