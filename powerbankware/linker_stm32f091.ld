/* Linker script for STM32F091xC (ARM Cortex-M0)
 * 256 KB flash, 32 KB SRAM.
 *
 * The MCU's flash starts at 0x08000000, but the first 32 KB is owned by
 * `powerbankboot` — the powerbankware application is loaded at 0x08008000
 * with 224 KB available. Derived from the OEM image: reset vector
 * 0x08019270 and .data LMA 0x0801F1A0 only resolve with this base.
 *
 * Unlike batteryware (Cortex-M0+, uses VTOR), this part is Cortex-M0 (no
 * VTOR): the application relocates its vector table by copying it to SRAM
 * 0x20000000 and setting SYSCFG_CFGR1.MEM_MODE = 3 (done in HAL init, not
 * in the reset handler).
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08008000, LENGTH = 224K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 32K
}

ENTRY(Reset_Handler)

SECTIONS
{
    /* VanMoof 40-byte image header */
    .header : ALIGN(4)
    {
        KEEP(*(.header))
    } > FLASH

    /* Cortex-M0 vector table — immediately after the header, at 0x08008028 */
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

    /* Image end symbol — used by the header for imageSize */
    _image_end = .;

    /* Initialised data in RAM — runtime image in flash (OEM .data @ 0x200000C0,
     * LMA 0x0801F1A0; .bss to 0x20002638; stack top 0x20008000) */
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
