/* TI CGT compiler-runtime `_auto_init_table` — cinit record walker.
 *
 * The OEM image at flash 0x00056BF0 is 50 B and reads from four
 * link-time symbol pairs:
 *   __TI_Handler_Table_Base / _Limit   — function-pointer table at
 *      flash 0x000571F8..0x00057204 (3 handlers x 4 B). Each entry
 *      points at one cinit body-type handler (byte-stream copy at
 *      0x56924, zero-fill at 0x57074, generic copy at 0x57148).
 *   __TI_CINIT_Base / _Limit           — record table at flash
 *      0x00057218..0x00057228 (2 records x 8 B). Each record is a
 *      pair (body_ptr, dst_ptr); body's leading byte is a type
 *      index into the handler table.
 *
 * The OEM does two equality checks (handler table non-empty, cinit
 * table non-empty), then walks the record table dispatching each
 * entry. `--gc-sections` would normally fold the always-false checks
 * away at link time; we hide the base/limit pointers behind volatile
 * locals so GCC keeps the OEM shape (and so the .o stays insensitive
 * to whether the linker populates the tables yet).
 *
 * Per-call: `handler(body + 1, dst)` — body+1 skips the type byte.
 *
 * Compiled body matches the OEM 50 B shape modulo register
 * allocation; the post-loop `nop; nop` filler is folded into the
 * function's natural alignment pad here. */

#include <stdint.h>

typedef void (*cinit_handler_t)(const void *body, void *dst);

struct cinit_record {
    const uint8_t *body;
    void          *dst;
};

extern cinit_handler_t const __TI_Handler_Table_Base[];
extern cinit_handler_t const __TI_Handler_Table_Limit[];
extern const struct cinit_record __TI_CINIT_Base[];
extern const struct cinit_record __TI_CINIT_Limit[];

void _auto_init_table(void)
{
    cinit_handler_t const *volatile handler_base  = __TI_Handler_Table_Base;
    cinit_handler_t const *volatile handler_limit = __TI_Handler_Table_Limit;
    if (handler_base == handler_limit) {
        return;
    }

    const struct cinit_record *volatile cinit_base  = __TI_CINIT_Base;
    const struct cinit_record *volatile cinit_limit = __TI_CINIT_Limit;
    if (cinit_base == cinit_limit) {
        return;
    }

    uint32_t n = (uint32_t)((const uint8_t *)cinit_limit -
                             (const uint8_t *)cinit_base) >> 3;
    const struct cinit_record *rec = cinit_base;
    do {
        uint8_t          type    = rec->body[0];
        cinit_handler_t  handler = handler_base[type];
        handler(rec->body + 1, rec->dst);
        rec++;
    } while (--n != 0);
}
