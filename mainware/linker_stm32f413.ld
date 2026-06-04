/* linker_stm32f413.ld — mainware
 *
 * Memory map for ST STM32F413VGT6 (Cortex-M4F):
 *   FLASH : 1 MB total. Mainware lives in sector 5 + part of sector 6
 *           (0x08020000..~0x080556A0, ≈ 213 KB). The first 512 bytes of
 *           sector 5 are the VanMoof envelope; the STM32 vector table starts
 *           0x200 in (at 0x08020200).
 *   SRAM  : 0x20000000, 320 KB contiguous = SRAM1 (256 K) + SRAM2 (64 K).
 *
 * The linker symbols below mirror the values Reset_Handler (0x08043E54) loads
 * from its literal pool (see docs/hardware.md): initial SP 0x20037000, .data
 * at SRAM 0x20000014 (the first word is g_systick_step), .bss after it.
 *
 * The 512-byte envelope at 0x08020000 (magic + version + CRC32 + length +
 * __DATE__/__TIME__) is emitted by startup_stm32f413.S's .envelope section so
 * the produced .bin aligns with the OEM file; the CRC and length are
 * post-build patches.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08020000, LENGTH = 384K  /* slot base, through sector 6 */
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 320K
}

_estack = 0x20037000;           /* OEM initial SP (vector slot 0) */
_Min_Stack_Size = 0x400;        /* refine from OEM */
_Min_Heap_Size  = 0x0;

SECTIONS
{
    /* VanMoof container envelope — 512 B at the slot base, before the vectors. */
    .envelope 0x08020000 :
    {
        KEEP(*(.envelope))
    } > FLASH

    .isr_vector 0x08020200 :
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

    /* .data — OEM VMA is SRAM 0x20000014 (leaving 0x20000000..0x13 as the
       retained low-RAM that holds the warm-boot marker). LMA follows .text. */
    .data 0x20000014 :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT > FLASH

    .bss (NOLOAD) :
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

    ._user_heap_stack (NOLOAD) :
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
