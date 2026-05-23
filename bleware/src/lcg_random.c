/* lcg_random.c — 15-bit pseudo-random helper.
 *
 * OEM symbol: `lcg_random_u15` @ 0x00023E34. Classic
 * Borland/glibc LCG:
 *
 *     state = 1103515245 * state + 12345;     (mod 2^32)
 *     return (state & 0x7FFFFFFF) >> 16;       — 15-bit value
 *
 * State lives in RAM at 0x20005B2C; two function-pointer slots at
 * 0x20005A58 / 0x20005A5C wrap the body in a critical section
 * (installed dynamically by stack init — not statically traceable).
 *
 * Callers: `indicate_seq_advance` (this TU's sibling) and
 * `FUN_00015AD8` (un-decoded). Both want a small fresh "fingerprint"
 * value — quality of randomness doesn't matter, only that successive
 * values are distinct over short windows.
 */

#include <stdint.h>

#include "bleware.h"

/* Lock pair installed by stack init at these RAM slots. The function
 * pointers themselves come from runtime registration; the OEM never
 * touches the slots from a statically-visible call site. */
extern void (*const g_lcg_lock_enter)(void);  /* @ 0x20005A58 (read via flash literal 0x00023E5C) */
extern void (*const g_lcg_lock_exit)(void);   /* @ 0x20005A5C (read via flash literal 0x00023E64) */
extern uint32_t      g_lcg_state;             /* @ 0x20005B2C (read via flash literal 0x00023E60) */

#define LCG_A  1103515245u   /* 0x41C64E6D */
#define LCG_C  12345u        /* 0x3039     */

uint32_t lcg_random_u15(void)
{
    uint32_t s;

    g_lcg_lock_enter();
    s = LCG_A * g_lcg_state + LCG_C;
    g_lcg_state = s;
    g_lcg_lock_exit();

    return (s & 0x7FFFFFFFu) >> 16;
}
