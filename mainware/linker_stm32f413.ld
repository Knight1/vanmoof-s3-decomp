/* linker_stm32f413.ld — mainware
 *
 * Memory map for ST STM32F413VGT6 (Cortex-M4F):
 *   FLASH : 1 MB total. Mainware lives in sector 5 + part of sector 6
 *           (0x08020000..~0x080556A0, ≈ 213 KB). The first 512 bytes
 *           of sector 5 are the VanMoof envelope; the STM32 vector
 *           table starts 0x200 in (at 0x08020200).
 *   SRAM  : 0x20000000, 320 KB contiguous = SRAM1 (256 K) + SRAM2 (64 K).
 *
 * Mainware's initial SP from the vector table is 0x20037000 — about
 * 220 KB above SRAM base. The upper ~100 KB of SRAM is left to the
 * runtime (BLE rx/tx buffers, modem AT buffers, image staging during
 * subsystem-firmware push).
 *
 * The envelope at 0x08020000 contains:
 *   0x08020000..0x0802000F  — 16 B VanMoof container header (magic +
 *                             version + CRC32 + length)
 *   0x08020010..0x08020027  — ASCII build date+time (__DATE__/__TIME__)
 *   0x08020028..0x080201FF  — 0xFF padding to VTOR alignment
 * For now this region is not described — we just leave FLASH starting
 * at 0x08020200 where the vector table goes. The envelope will be
 * synthesised at image-package time by the bin-postprocess step
 * (analogous to shifterware's CRC patcher).
 */

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08020200, LENGTH = 384K  /* through end of sector 6 */
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 320K
}

_estack = 0x20037000;
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
