/* linker_cc2642r1.ld — bleware
 *
 * Memory map for TI CC2642R1F (Cortex-M4F + BLE 5.2 SoC):
 *   FLASH : 352 KB main flash at 0x00000000..0x0005FFFF, organised as
 *           44 x 8 KB erase pages. The last page (8 KB at
 *           0x00056000..0x00057FFF) belongs to bleboot (the BIM).
 *           bleware uses the first 344 KB.
 *   SRAM  :  80 KB at 0x20000000..0x20013FFF.
 *
 * bleware ships as a TI **OAD-NVM1** image — the first 0x90 bytes
 * at flash 0x00000000..0x0000008F are the OAD image header (parsed
 * by bleboot to verify the image and locate `prgEntry`). The runtime
 * vector table starts at flash 0x00000090 (= `prgEntry`).
 *
 * Initial SP from bleware 1.4.01's vector slot 0: 0x20013A00.
 * That's 1.5 KB below SRAM top — bleboot's 0x20014000 spec, minus a
 * small guard/shared-ram window between the two images. The remaining
 * SRAM is the heap and per-task stacks.
 */

ENTRY(Reset_Handler)

MEMORY
{
    OAD_HDR (rx) : ORIGIN = 0x00000000, LENGTH = 0x90
    FLASH   (rx) : ORIGIN = 0x00000090, LENGTH = 352K - 8K - 0x90   /* 344 KB minus header */
    SRAM    (rwx) : ORIGIN = 0x20000000, LENGTH = 80K
}

_estack = 0x20014000;
_stack_top  = 0x20013A00;       /* bleware's actual vec[0] */
_stack_size = 0x600;            /* 1.5 KB, matches Reset_Handler's MSP computation */

_Min_Stack_Size = _stack_size;
_Min_Heap_Size  = 0x0;

SECTIONS
{
    /* The OAD-NVM1 image header lives at flash 0x0..0x8F. Provided
     * by a dedicated `.oad_header` section that src/oad_header.c
     * places at the start of the binary. The header is platform-
     * specific data (CRC32, version, length, prgEntry, softVer, ...);
     * see docs/hardware.md for the exact field layout. */
    .oad_header 0x00000000 :
    {
        KEEP(*(.oad_header))
    } > OAD_HDR

    /* Cortex-M vector table at flash 0x00000090. Vector slot 0 = MSP,
     * slot 1 = Reset_Handler, slots 2..14 = TI SimpleLink ROM
     * handlers (0x1002FB39 default-trap, 0x1002FB11 PendSV ROM stub),
     * slot 15 = SysTick (= 0xFFFFFFFF on bleware 1.4.01 — unused),
     * slots 16..47 = IRQs. */
    .isr_vector 0x00000090 :
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
        *(.rodata.*)
        . = ALIGN(4);
    } > FLASH

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM       : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH

    /* ---- RAM ---- */

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
