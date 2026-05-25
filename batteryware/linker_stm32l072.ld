/* Linker script for STM32L072CZT6 (ARM Cortex-M0+)
 * 192 KB flash, 20 KB SRAM.
 * Batteryware application lives at 0x08000000 (flash base).
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 192K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

ENTRY(Reset_Handler)

SECTIONS
{
    /* VanMoof 40-byte image header */
    .header 0x08000000 : ALIGN(4)
    {
        *(.header)
    } > FLASH

    /* Vector table */
    .vector 0x08000028 : ALIGN(4)
    {
        *(.vector)
    } > FLASH

    /* Code and read-only data */
    .text :
    {
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

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
    }
}
