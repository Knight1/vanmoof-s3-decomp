/* linker_mm32f031.ld
 *
 * Memory map for MM32F031F6U6 (ARM Cortex-M0):
 *   FLASH : 0x08000000, 32 KB total
 *   SRAM  : 0x20000000,  4 KB
 *
 * The shifterware application lives in the 20 KB partition starting at
 * **0x08003000** (shifterboot owns 0x08000000..0x08002FFF). The first
 * 232 bytes of that partition are the VanMoof image header (40 B) +
 * the Cortex-M0 vector table (192 B), defined in
 * `src/startup_mm32f031.S`. The remaining 0x080030E8 onward is .text /
 * .rodata / .data initialisers.
 *
 * Image size (header.imageSize) is computed at link time as
 * `_image_end - g_vanmoof_image_header`; see startup_mm32f031.S.
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08003000, LENGTH = 20K
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 4K
}

/* Stack grows down from the top of SRAM. */
_estack = ORIGIN(SRAM) + LENGTH(SRAM);
_Min_Stack_Size = 0x400;       /* 1 KB — refine if/when OEM SP usage observed */
_Min_Heap_Size  = 0x0;         /* no heap; firmware does not malloc */

SECTIONS
{
    /* ---- VanMoof image header (40 B) at flash 0x08003000 ----------- */
    .vanmoof_header :
    {
        KEEP(*(.vanmoof_header))
    } > FLASH

    /* ---- Cortex-M0 vector table (192 B) at flash 0x08003028 -------- */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    /* ---- .text (code) ---------------------------------------------- */
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

    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH

    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH

    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > FLASH

    /* Initialised data: load from FLASH, run from SRAM. Reset_Handler
     * copies from _sidata to [_sdata, _edata). */
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

    /* Marks the end of all flash-resident bytes — used by the image
     * header's `imageSize` field to advertise the total partition
     * footprint to shifterboot. */
    _image_end = LOADADDR(.data) + SIZEOF(.data);

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

    /* The OEM firmware accesses dozens of globals at hard-coded SRAM
     * addresses (G_TICK_B @ 0x200000D0, G_OTA_STAGE_HW @ 0x200001E0,
     * the cmd-0x82 OTA staging halfword buffer, the modbus RX/TX
     * scratch, etc.) without declaring them in any source object.
     * They live in the SRAM hole between `_ebss` and the stack
     * reservation, and the OEM relies on cold-reset SRAM being zero
     * — `main` never zero-fills them. Reset_Handler does it for us;
     * `_sram_clear_end` marks the upper bound of the region it must
     * clear. We stop short of `._user_heap_stack` because that range
     * is owned by the live stack we're executing on. */
    _sram_clear_end = ORIGIN(SRAM) + LENGTH(SRAM) - _Min_Stack_Size - _Min_Heap_Size;

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
    }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}
