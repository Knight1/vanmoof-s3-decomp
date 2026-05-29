/* Linker script for STM32L072CZT6 (ARM Cortex-M0+)
 * 192 KB flash, 20 KB SRAM.
 *
 * The MCU's flash starts at 0x08000000, but the first 20 KB is owned
 * by `bmsboot` — the batteryware application is loaded at 0x08005000
 * with 172 KB available. Literal-pool addresses in the OEM binary
 * (e.g. AHBPrescTable @ 0x080181E8, PLLMulTable @ 0x08018200) only
 * resolve correctly with this base.
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08005000, LENGTH = 172K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

ENTRY(Reset_Handler)

SECTIONS
{
    /* VanMoof 40-byte image header */
    .header : ALIGN(4)
    {
        KEEP(*(.header))
    } > FLASH

    /* Vector table — must follow immediately after header at 0x08000028 */
    .vector : ALIGN(4)
    {
        KEEP(*(.vector))
    } > FLASH

    /* Code and read-only data */
    .text :
    {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* C-runtime init arrays. Empty in this image (no static ctors), so
     * each start == end — matching the OEM, where __libc_init_array_lite's
     * boundary symbols all resolve to one address. Provides the symbols
     * referenced by __libc_init_array_lite in startup_stm32l072.S. */
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

    /* Image end symbol — used by header for imageSize */
    _image_end = .;

    /* Initialised data in RAM — runtime image in flash */
    .data :
    {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT > FLASH

    _sidata = LOADADDR(.data);

    /* Uninitialised data (BSS) */
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

    /* Discard unused sections */
    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.ARM.attributes)
    }
}
