/* cinit.c — TI CGT-style C runtime init.
 *
 * OEM at 0x00017EF0 (`cinit_walker` in Ghidra). Two-pass walker:
 *
 *   Pass 1 — cinit table:
 *     For each (record_ptr, body_ptr) pair in the table at
 *     [DAT_F80, DAT_F84), look up record's first byte as an index
 *     into the handler table at DAT_F88, and call
 *         handler(record + 1, body)
 *     The handlers are usually two entries: a `memset(dst, 0, len)`
 *     for BSS-fill and an `LZSS_decompress` for compressed .data.
 *
 *   Pass 2 — auto-init constructors:
 *     For each non-null entry in [DAT_F78, DAT_F7C), call it as
 *     `void (*)(void)`. These are global C++ constructors / TI
 *     module-init pointers.
 *
 * Skeleton: just zero the linker-declared .bss and copy .data from
 * its load-address image. The constructor pass is empty for now.
 *
 * Refs: TI ARM CGT linker scatter-table format; see
 * "TI ARM Optimizing C/C++ Compiler User's Guide".
 */

#include <stdint.h>
#include <string.h>

extern uint32_t _sdata, _edata, _sidata;
extern uint32_t _sbss, _ebss;

void cinit_walker(void)
{
    /* .data init: word-copy from flash LMA to SRAM VMA. */
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;
    while (dst < &_edata) {
        *dst++ = *src++;
    }

    /* .bss zero. */
    dst = &_sbss;
    while (dst < &_ebss) {
        *dst++ = 0u;
    }

    /* TODO: walk auto-init / global-constructor table — empty for
     * this skeleton until a constructor lands. */
}
