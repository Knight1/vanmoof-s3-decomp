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

/* ====================================================================
 * OEM global address pins (byte-exact scaffolding)
 * ====================================================================
 *
 * These assignments place each decoded global at the exact SRAM (or, for
 * the two flash constants noted below, flash) address the OEM image uses,
 * so that every PC-relative literal that references one of them resolves
 * to the same value the OEM stored. A linker-script assignment overrides
 * the weak placeholder definitions in src/hal_stubs.S without allocating
 * storage (SRAM is omitted from the raw .bin), and overlays the real OEM
 * memory map.
 *
 * This is a PREREQUISITE for, not a guarantee of, a byte-exact image:
 * full byte-exactness additionally needs every function placed at its OEM
 * flash address (which in turn needs the TI SimpleLink SDK to materialise
 * the ~85% of the image that is vendor ROM/stack). With --gc-sections the
 * stub build drops everything that references these symbols, so the pins
 * have no effect on the current 900-byte .bin — they are carried so the
 * addresses are correct the moment the code is rooted at OEM offsets.
 *
 * Globals whose OEM SRAM address is not yet known stay as weak BSS in
 * hal_stubs.S (unpinned) and are listed in docs/progress.md.
 * -------------------------------------------------------------------- */

/* Connection / SSP / GATT-dispatch state */
g_ble_connection_table_storage    = 0x20004158;
g_ssp_master                      = 0x20003104;
g_gatt_dispatch                   = 0x20003F84;

/* External-flash SPI driver */
g_extflash_gpio_ctx               = 0x20001B34;
g_extflash_spi_handle             = 0x20001B28;
g_gpio_max_pin                    = 0x20005A78;

/* RTOS heap / message-queue / ICall-entity bases */
g_heap_state_storage              = 0x20004E0C;
g_bluetoothtask_msg_queue_storage = 0x200057C8;
g_icall_entities_storage          = 0x20004E78;

/* Backoffice / GATT notify / log notify */
g_backoffice_reassembly           = 0x2000A248;
g_gatt_notify_state               = 0x200049CC;
g_log_notify_state                = 0x20004A00;
s_dest_service_id                 = 0x2000566C;

/* Advertising / bonding / button-press / state-machine */
g_gap_adv_cfg                     = 0x20005290;
g_gap_adv_rand_addr               = 0x20005AF8;
g_bond_addr_mode_strings          = 0x200058E0;
g_bond_last_state                 = 0x200058DC;
g_buttonpress_log_enabled         = 0x20005670;
g_sm_context                      = 0x20005950;

/* Monitor console */
g_monitor_history_word            = 0x2000457C;
g_console_disabled                = 0x2000AE04;
g_console_event                   = 0x20005770;

/* Flash constant (value 10), reached as *(0x0002BB88) — pin the symbol at
 * its flash literal so 1000 / g_console_tick_divisor reads the OEM word. */
g_console_tick_divisor            = 0x0002BB88;

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
