/* linker_cc2642r1.ld — bleboot
 *
 * Memory map for TI CC2642R1F (Cortex-M4F, BLE 5.2 SoC):
 *   FLASH : 352 KB main flash at 0x00000000..0x0005FFFF, organised as
 *           44 x 8 KB erase pages. The *last* page (8 KB at
 *           0x00056000..0x00057FFF) is the CCFG sector and is where
 *           the BIM (Boot Image Manager) lives, with the CCFG fields
 *           pinned to the last 88 bytes and a VanMoof BVER block at
 *           0x00057F38. The earlier 344 KB are available to the
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
 *
 * Fixed-address regions (pinned via named sections to match the OEM
 * byte layout — code references these by absolute pointer):
 *   .bim_flash.chip_table    @ 0x000571A8 (48 B)
 *   .bim_flash.adi_lut           @ 0x000571D8 (8 B)
 *   .bim_flash.oad_nvm1_a        @ 0x000571E0 (8 B)
 *   .bim_flash.oad_nvm1_b        @ 0x000571E8 (8 B)
 *   .bim_flash.spi_opcodes       @ 0x000571F0 (8 B)
 *   .bim_flash.cinit_handlers    @ 0x000571F8 (16 B incl. limit pad)
 *   .bim_flash.cinit_bodies      @ 0x00057208 (16 B)
 *   .bim_flash.cinit_table       @ 0x00057218 (16 B)
 *   .bver_block               @ 0x00057F38 (32 B)
 *   .ccfg                     @ 0x00057FA8 (88 B)
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
        *(.rodata.*)
        . = ALIGN(4);
    } > FLASH

    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > FLASH
    .ARM       : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH

    /* ---- Pinned flash literals (absolute addresses must match OEM) ---- */

    .bim_flash.chip_table 0x000571A8 :
    {
        KEEP(*(.bim_flash.chip_table))
    } > FLASH

    .bim_flash.adi_lut 0x000571D8 :
    {
        KEEP(*(.bim_flash.adi_lut))
    } > FLASH

    .bim_flash.oad_nvm1_a 0x000571E0 :
    {
        KEEP(*(.bim_flash.oad_nvm1_a))
    } > FLASH

    .bim_flash.oad_nvm1_b 0x000571E8 :
    {
        KEEP(*(.bim_flash.oad_nvm1_b))
    } > FLASH

    .bim_flash.spi_opcodes 0x000571F0 :
    {
        KEEP(*(.bim_flash.spi_opcodes))
    } > FLASH

    .bim_flash.cinit_handlers 0x000571F8 :
    {
        __TI_Handler_Table_Base = .;
        KEEP(*(.bim_flash.cinit_handlers))
        __TI_Handler_Table_Limit = __TI_Handler_Table_Base + 12;
    } > FLASH

    .bim_flash.cinit_bodies 0x00057208 :
    {
        KEEP(*(.bim_flash.cinit_bodies))
    } > FLASH

    .bim_flash.cinit_table 0x00057218 :
    {
        __TI_CINIT_Base = .;
        KEEP(*(.bim_flash.cinit_table))
        __TI_CINIT_Limit = .;
    } > FLASH

    .bver_block 0x00057F38 :
    {
        KEEP(*(.bver_block))
    } > FLASH

    .ccfg 0x00057FA8 :
    {
        KEEP(*(.ccfg))
    } > FLASH

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
