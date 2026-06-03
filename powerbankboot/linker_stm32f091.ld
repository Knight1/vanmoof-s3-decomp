/* Linker script for STM32F091xC (ARM Cortex-M0) — powerbankboot
 *
 * The bootloader owns the first 32 KB of flash. The CPU resets straight into
 * the vector table at 0x08000000 (slot 0 = initial SP = 0x20008000, slot 1 =
 * Reset_Handler = 0x08002878 in the OEM image). Unlike the application images,
 * the loader has NO VanMoof 40-byte header — the vector table is at the very
 * base of flash.
 *
 * The application banks live above this window:
 *   AP     0x08008000 .. 0x08023FFF (112 KB) — booted
 *   Shadow 0x08024000 .. 0x0803FFFF (112 KB) — backup / OTA staging
 *
 * Cortex-M0 has no VTOR; the loader runs with its vectors at flash base and the
 * application relocates its own table after the jump.
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 32K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
}

ENTRY(Reset_Handler)

SECTIONS
{
    /* Cortex-M0 vector table — at flash base 0x08000000 (48 words) */
    .isr_vector : ALIGN(4)
    {
        KEEP(*(.isr_vector))
    } > FLASH

    /* Code and read-only data */
    .text :
    {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH
    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH

    /* Initialised data: RAM at runtime, init image in flash (LMA).
     * init_data_bss() copies _sidata -> [_sdata.._edata) and zeroes .bss; the
     * OEM re-runs it between STL phases, so it must stay idempotent. */
    .data :
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT > FLASH

    _sidata = LOADADDR(.data);

    .bss :
    {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > SRAM

    /* Stack — top of SRAM (32 KB) */
    _estack = ORIGIN(SRAM) + LENGTH(SRAM);

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.ARM.attributes)
    }
}
