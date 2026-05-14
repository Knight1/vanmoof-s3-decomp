/* linker_mm32f031.ld — shifterboot
 *
 * Memory map for MM32F031F6U6 (ARM Cortex-M0):
 *   FLASH : 0x08000000, 32 KB total (this loader occupies the first 6 KB)
 *   SRAM  : 0x20000000,  4 KB
 *
 * The bootloader's initial SP from the vector table is 0x200006F8 — it
 * uses only the bottom ~1.78 KB of SRAM, leaving the upper region for
 * persistent state that may survive the warm jump into the application.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 32K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 4K
}

_estack = 0x200006F8;
_Min_Stack_Size = 0x200;       /* refine from OEM */
_Min_Heap_Size  = 0x0;

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        *(.eh_frame)

        KEEP (*(.init))
        KEEP (*(.fini))

        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM       : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH

    _sidata = LOADADDR(.data);
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT > FLASH

    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > SRAM

    ._user_heap_stack :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        PROVIDE(_end = .);
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } > SRAM

    /DISCARD/ :
    {
        libc.a   ( * )
        libm.a   ( * )
        libgcc.a ( * )
    }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}
