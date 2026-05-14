/* linker_cc2642r1.ld — bleboot
 *
 * Memory map for TI CC2642R1F (Cortex-M4F, BLE 5.2 SoC):
 *   FLASH : 352 KB main flash at 0x00000000..0x0005FFFF, organised as
 *           88 × 4 KB pages... no, wait — CC13x2/CC26x2 erase sectors
 *           are 8 KB. So 44 × 8 KB pages. The *last* page (8 KB at
 *           0x00056000..0x00057FFF) is the CCFG sector and is where
 *           the BIM (Boot Image Manager) lives, with the CCFG fields
 *           pinned to the last 88 bytes and an OAD image header just
 *           above them. The earlier 344 KB are available to the
 *           downloadable application image(s).
 *   SRAM  :  80 KB at 0x20000000..0x20013FFF.
 *
 * bleboot's initial SP from the vector table is 0x20014000 — top of
 * SRAM. The runtime requirement is tiny (small bootloader: select an
 * image, verify, jump) so almost all of SRAM is available to it; the
 * application running on top of bleboot gets the rest at runtime.
 *
 * The vector table is at the very start of the image (file offset 0
 * = flash 0x00056000) — no envelope, unlike shifterware/mainware.
 * The CCFG_O_IMAGE_VALID_CONF field at the tail of the CCFG page
 * points back at 0x00056000 so the ROM bootloader knows to jump
 * here.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00056000, LENGTH = 8K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 80K
}

_estack = 0x20014000;
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
