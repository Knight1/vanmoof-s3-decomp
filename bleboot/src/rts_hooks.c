/* TI ARM Compiler runtime hooks that the OEM image kept at their
 * weak-default values. These are intentionally trivial: replicate
 * the exact byte sequence the upstream weak implementations emit,
 * so the OEM image and our reconstruction stay byte-equivalent at
 * the call sites. */

/* `_system_pre_init` — TI CGT compiler-runtime calls this before
 * `_auto_init_*` (cinit). Returning nonzero enables cinit; zero
 * skips it. The weak default in TI's RTS library is exactly
 * `return 1;`, which compiles to `movs r0, #1; bx lr` (4 bytes).
 * The OEM kept that default. */
int _system_pre_init(void)
{
    return 1;
}

/* `_exit` — TI CGT runtime calls this after `main` returns to
 * terminate the program. The weak default is a trap loop; the OEM
 * kept it. We render it as `nop; b .` to match the OEM byte
 * sequence (`bf00 e7fe`) rather than the canonical `b .` (`e7fe`)
 * alone — the leading nop comes from the TI CCS startup template
 * and is what the OEM image carries. */
__attribute__((naked, noreturn)) void _exit(int status)
{
    (void)status;
    __asm__ volatile (
        "nop      \n\t"
        "1: b 1b  \n\t"
    );
}

#include <stdint.h>

/* `auto_init_zero_fill` (`FUN_00057074` in the OEM, 28 B). TI CGT
 * compiler-runtime `_auto_init`-record zero-fill handler — one of
 * the slots in the dispatch table at flash `0x000571F8` (along
 * with the byte-stream copy handler at `0x00056924` and the
 * generic-copy handler at `0x00057148`) that
 * `_auto_init_table` (`0x00056BF0`) indexes into by the record's
 * type byte. Walks the table at `0x00057218..0x00057228` (16 bytes
 * = two records); record `(handler=this, src=0x57208,
 * dst=0x20000300, length-at-src+3=0x108)` zero-fills the 264-byte
 * CRC scratch + chip-cursor region at SRAM `0x20000300..0x20000408`.
 *
 * The handler-table dispatch hands us `(body=record+1, dst)`.
 * The record's 4-byte length field lives at `body + 3` (i.e.
 * record + 4) — a misaligned word read that's only legal because
 * the CC2642 Cortex-M4 has alignment-trap disabled by default
 * and the destination word happens to be 4-aligned (record is
 * 4-aligned, +4 stays 4-aligned).
 *
 * Loop iteration count: `length`. The OEM uses a `length - 1`
 * countdown that exits when the counter wraps to `0xFFFFFFFF`,
 * which makes the zero-length case a no-op and otherwise writes
 * exactly `length` bytes. */
void auto_init_zero_fill(const void *body, uint8_t *dst)
{
    uint32_t remaining = *(const uint32_t *)((const uint8_t *)body + 3) - 1u;
    if (remaining == 0xFFFFFFFFu) {
        return;
    }
    do {
        *dst++ = 0u;
        remaining--;
    } while (remaining != 0xFFFFFFFFu);
}
