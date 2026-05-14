/* linker_stm32f7.ld — mainboot
 *
 * Memory map derived from muco-boot.bin (the OEM image):
 *   FLASH : 0x08000000, 32 KB observed (this loader fits in the first
 *           sector of an STM32F7 — actual flash size is at least 256 KB
 *           on the candidate parts, but we only carve out what the
 *           bootloader uses).
 *   SRAM  : 0x20000000, 320 KB contiguous (DTCM+SRAM1+SRAM2 on F7;
 *           SRAM1+SRAM2+SRAM3 on F469/F479).
 *
 * The bootloader's initial SP from the vector table is 0x20050000 — the
 * very top of the contiguous SRAM region. Reset_Handler clears .bss only
 * up to 0x20000950, so the upper bulk of SRAM is presumably reserved for
 * runtime allocations (stack growth, plus likely a fast-path region the
 * application reads back).
 *
 * NOTE: the exact STM32 part is not yet confirmed. The values below are
 * the observed OEM-image constants; tighten or replace once the part is
 * identified from RCC/peripheral literal pools.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 32K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 320K
}

_estack = 0x20050000;
_Min_Stack_Size = 0x400;       /* refine from OEM */
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
