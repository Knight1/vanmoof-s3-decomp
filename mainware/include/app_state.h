#ifndef MAINWARE_APP_STATE_H
#define MAINWARE_APP_STATE_H

#include <stdint.h>

/* The mainware "application state" lives in three pieces at known
 * absolute SRAM addresses:
 *
 *   g_console_state     0x2000010E   small console/login state block
 *   g_app_state         0x20009368   outer application state
 *     ->ctx_sub         (+0x2DC)     pointer into a per-session context
 *
 * Fields are surfaced in this header as we decode functions that touch
 * them. The structures are intentionally not exhaustively typed — only
 * the offsets we've seen and verified are committed. Padding fields
 * mark out the gaps so the layout stays stable as fields are added.
 *
 * The verified offsets so far come from `login_handler` (0x080425F4)
 * and its sibling password-set handlers. */

struct console_state {
    uint8_t  _pad0[5];       /* +0x00..+0x04 */
    /* +0x05 : login machine state byte.
     *   SCHED_SLOT_NONE (0xFA) = ready to accept a password line
     *   0..47                  = scheduler slot id of an active lockout */
    uint8_t  login_state;    /* +0x05  =  SRAM 0x20000113 */
    /* ... grows further; not yet decoded. */
};
_Static_assert(__builtin_offsetof(struct console_state, login_state) == 0x05,
               "console_state.login_state must sit at +0x05");

extern struct console_state g_console_state;   /* SRAM 0x2000010E */

struct session_ctx {
    uint8_t  _pad0[0x2D9];   /* +0x000..+0x2D8 */
    uint8_t  logged_in;      /* +0x2D9 — non-zero once login succeeded */
    uint8_t  _pad1[0x06];    /* +0x2DA..+0x2DF */
    uint8_t  fail_count;     /* +0x2E0 — consecutive failed password tries */
    uint8_t  _pad2[0xB7];    /* +0x2E1..+0x397 */
    char     user_password[0x40]; /* +0x398 — user-configurable service
                                   *           password; empty string
                                   *           means "not set". Length
                                   *           cap is 0x40 elsewhere in
                                   *           the console code. */
    /* ... grows further; not yet decoded. */
};
_Static_assert(__builtin_offsetof(struct session_ctx, logged_in)       == 0x2D9, "");
_Static_assert(__builtin_offsetof(struct session_ctx, fail_count)      == 0x2E0, "");
_Static_assert(__builtin_offsetof(struct session_ctx, user_password)   == 0x398, "");

struct app_state {
    uint8_t  _pad0[0x2DC];        /* +0x000..+0x2DB */
    struct session_ctx *ctx_sub;  /* +0x2DC — points into a per-session block */
    /* ... grows further; not yet decoded. */
};
_Static_assert(__builtin_offsetof(struct app_state, ctx_sub) == 0x2DC, "");

extern struct app_state g_app_state;   /* SRAM 0x20009368 */

#endif
