/* TI CGT cinit dispatch handlers — bodies for the function-pointer
 * table at flash 0x000571F8 (`__TI_Handler_Table_Base`). Each
 * handler is called by `_auto_init_table` once per matching record
 * in `__TI_CINIT_Base..__TI_CINIT_Limit`, with signature
 *   handler(body + 1, dst)   // body+1 skips the type-byte selector
 *
 * Type 0 → cinit_byte_stream_copy (OEM 0x00056924, 104 B)
 *   LZSS-style decompressor with a 12-bit sliding-window offset and
 *   variable-length back-reference lengths. Terminator is a back-ref
 *   with offset == 0xFFF. Encoding (after the type byte is stripped):
 *     - 1 control byte; each of its 8 bits LSB-first selects literal
 *       (bit set) or back-reference (bit clear).
 *     - literal:  1 byte copied verbatim from input to dst.
 *     - back-ref: 2 bytes (off_hi:8 | (off_lo<<4 | len_nibble)).
 *       length = len_nibble + 3 (3..18); if length == 18 then a
 *       variable-length extension follows (LEB128-ish: each byte's
 *       MSB is a continuation flag, low 7 bits accumulate into a
 *       running sum added to length). Then copy `length` bytes from
 *       (dst - offset - 1) to dst with overlap-safe forward semantics.
 *
 * Type 1 → cinit_generic_copy (OEM 0x00057148, 14 B)
 *   Tail-call to a generic memcpy with length read from body[3..6]
 *   and src starting at body+7. The OEM tail-calls a 152 B
 *   alignment-optimised memcpy at 0x000565E0; we use a small
 *   portable byte loop instead — behaviour-equivalent, smaller
 *   bytes.
 *
 * For the OEM bleboot image, the only cinit record exercising the
 * byte-stream handler (record 1 at 0x57218) decompresses to 4 bytes
 * written at SRAM 0x20000408, which the encoding's back-ref-to-
 * already-zeroed-memory trick fills with zeros (record 0 zero-fills
 * the prior 264 B). End result: SRAM[0x300..0x40C] is all zero on
 * boot, with 264 + 4 = 268 B initialised. */

#include <stdint.h>

void cinit_byte_stream_copy(const void *body_p, void *dst_p)
{
    const uint8_t *src = (const uint8_t *)body_p;
    uint8_t       *dst = (uint8_t *)dst_p;
    uint32_t       ctrl  = *src++;
    uint32_t       ctrl_bits = 0;

    for (;;) {
        while (ctrl_bits < 8) {
            uint32_t take_literal = ctrl & 1u;
            ctrl >>= 1;
            ctrl_bits++;
            if (take_literal) {
                *dst++ = *src++;
                continue;
            }

            uint32_t b0     = *src++;
            uint32_t b1     = *src++;
            uint32_t length = (b1 & 0x0Fu) + 3u;
            uint32_t offset = ((b1 >> 4) & 0x0Fu) | (b0 << 4);

            if (length == 18u) {
                uint32_t ext = *src++;
                while (ext & 0x80u) {
                    ext = (ext & 0x7Fu) | ((uint32_t)(*src++) << 7);
                }
                length += ext;
            }

            if (offset == 0x0FFFu) {
                return;
            }

            const uint8_t *back = dst - offset - 1;
            for (uint32_t i = 0; i < length; i++) {
                *dst++ = *back++;
            }
        }
        ctrl      = *src++;
        ctrl_bits = 0;
    }
}

void cinit_generic_copy(const void *body_p, void *dst_p)
{
    const uint8_t *body = (const uint8_t *)body_p;
    uint32_t       length;
    /* Length lives at body+3 (the inline-asm `ldr.w r2, [r0, #3]` is
     * a deliberately misaligned word load — legal on Cortex-M4 with
     * the default alignment-trap-disabled config). Source bytes
     * start at body+7. */
    __builtin_memcpy(&length, body + 3, 4);
    const uint8_t *src = body + 7;
    uint8_t       *dst = (uint8_t *)dst_p;
    while (length-- > 0u) {
        *dst++ = *src++;
    }
}
