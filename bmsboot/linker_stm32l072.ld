/* Linker script for STM32L072CZT6 (ARM Cortex-M0+) — bmsboot
 *
 * The bootloader owns the first 20 KB of flash. The CPU resets straight into
 * the vector table at 0x08000000 (slot 0 = initial SP = 0x20005000, slot 1 =
 * Reset_Handler = 0x08001E60 in the OEM image). Unlike the batteryware
 * application image, the loader has NO VanMoof 40-byte header — the vector
 * table is at the very base of flash.
 *
 * The application banks live above this window:
 *   AP     0x08005000 .. 0x0801A7FF (86 KB) — booted (batteryware runs here)
 *   Shadow 0x0801A800 .. 0x0802FFFF (86 KB) — backup / OTA staging
 *
 * boot_hw_init() copies this vector table to SRAM (0x20000000) and sets VTOR;
 * the STM32L0 (unlike the F0) has a relocatable vector table.
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 20K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

ENTRY(Reset_Handler)

SECTIONS
{
    /* Cortex-M0+ vector table — at flash base 0x08000000 (48 words) */
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
     * The OEM Reset_Handler copies _sidata -> [_sdata.._edata) and zeroes .bss
     * inline (no separate routine). LMA in the OEM is 0x080049D8. */
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

    /* Stack — top of SRAM (20 KB) */
    _estack = ORIGIN(SRAM) + LENGTH(SRAM);

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.ARM.attributes)
    }
}
