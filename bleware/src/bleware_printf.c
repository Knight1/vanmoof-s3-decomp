/* bleware_printf.c — unbounded sprintf-style formatter.
 *
 * `bleware_snprintf` is the OEM's only printf-family entry point in the
 * application image (despite the name, it takes no size argument — it is
 * an sprintf). It marshals its variadic arguments and an output-cursor
 * holder, drives the ~2.6 KB TI CGT formatting core at FUN_00000BC0, and
 * then writes the trailing NUL at wherever the core left the cursor.
 *
 * The core is shared with the rest of the firmware's formatting paths and
 * is left as an extern here — it is the TI compiler runtime's internal
 * formatting engine (`_pproc_*` driver), not application code.
 *
 * The core calls back into the caller through two function pointers that
 * the wrapper passes in from its literal pool:
 *
 *   putc_cb  (OEM @ 0x00027524)  void(char c, char **cursor)
 *       *(*cursor)++ = c;          — append one byte, advance the cursor.
 *
 *   block_cb (OEM @ 0x000264BC)  void(const char *src, char **cursor,
 *                                     unsigned int n)
 *       memcpy(*cursor, src, n); *cursor += n;  — append a run of bytes.
 *
 * Both callbacks mutate the single output cursor through a pointer-to-
 * pointer, which is how the core knows where the formatted text ended so
 * the wrapper can drop the terminating NUL there.
 *
 * OEM @ 0x00022A30 (snprintf wrapper). Supersedes the WEAK_NOOP in
 * src/hal_stubs.S.
 */

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#include "bleware.h"

/* TI CGT formatting core (the firmware's internal vsnprintf engine).
 *
 *   fmt_state   in/out cursor over the format string; fmt_state[0] is
 *               advanced past each consumed format byte.
 *   va_args     pointer to the variadic argument area (AAPCS: the spilled
 *               register/stack args that follow `fmt`).
 *   out_cursor  one-element holder; out_cursor[0] is the live output
 *               pointer, advanced by the two callbacks below.
 *   putc_cb     single-byte emit callback (see file banner).
 *   block_cb    multi-byte emit callback (see file banner).
 *
 * Returns the number of bytes emitted (negative on error). The wrapper
 * discards it. OEM @ 0x00000BC0. */
extern int FUN_00000BC0(const char  **fmt_state,
                        void         *va_args,
                        char        **out_cursor,
                        void        (*putc_cb)(char, char **),
                        void        (*block_cb)(const char *, char **,
                                                unsigned int));

/* Literal-pool callback pointers (DAT_00022A6C / DAT_00022A68). The OEM
 * stores them as raw Thumb function addresses (LSB set) in the wrapper's
 * constant pool; here we reference the callbacks by their OEM symbols.
 *
 *   FUN_00027524  putc_cb   — *(*cursor)++ = c
 *   FUN_000264BC  block_cb  — memcpy(*cursor, src, n); *cursor += n
 *
 * Both are application-flash leaves left undecoded in this pass; they are
 * resolved at link time alongside the formatting core. */
extern void FUN_00027524(char c, char **cursor);
extern void FUN_000264BC(const char *src, char **cursor, unsigned int n);

/* Format `fmt` (and its variadic arguments) into `out`, then NUL-
 * terminate. No bound is applied — the caller must size `out` for the
 * worst-case expansion, exactly as the OEM does.
 *
 * OEM @ 0x00022A30. The wrapper spills its arguments, builds a va_list
 * that points just past `fmt`, hands the core a one-element output-cursor
 * holder, and on return stores '\0' at the final cursor position. */
void bleware_snprintf(char *out, const char *fmt, ...)
{
    const char *fmt_state = fmt;   /* core advances this past the format */
    char       *cursor    = out;   /* the live output pointer */

    va_list ap;
    va_start(ap, fmt);

    /* The core takes the variadic ARGS-AREA POINTER VALUE — it dereferences
     * and advances it to fetch each argument — not the address of the
     * va_list holder. On the AArch32 EABI a va_list's first word is that
     * args pointer, so pass it through; handing over &ap would add a
     * spurious level of indirection. OEM @ 0x22A30 loads r1 = aligned(&fmt)+4
     * (the args area just past fmt). */
    FUN_00000BC0(&fmt_state, *(void **)&ap, &cursor, FUN_00027524, FUN_000264BC);

    va_end(ap);

    *cursor = '\0';
}
