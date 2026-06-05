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
    uint8_t  _pad0[0xF4];           /* +0x000..+0x0F3 */
    uint32_t audio_engine_cfg[4];   /* +0x0F4..+0x103 — four words consumed by
                                     *                  the audio-config apply
                                     *                  helper after every
                                     *                  volume-set. Semantics
                                     *                  not yet decoded. */
    uint8_t  audio_cfg_byte;        /* +0x104  audio block start byte (snapshotted,
                                     *         not a volume-command target) */
    uint8_t  volume_low;            /* +0x105  written by the `vollow` cmd */
    uint8_t  volume_medium;         /* +0x106  written by the `volmid` cmd */
    uint8_t  volume_high;           /* +0x107  written by the `volhigh` cmd */
    uint8_t  _pad1a;                /* +0x108 */
    uint8_t  region;                /* +0x109 — region / speed mode set by the
                                     *           `region` console command:
                                     *           0=EU, 1=US, 2=JP, 3=OFFROAD
                                     *           (OFFROAD removes the speed cap).
                                     *           Pushed to the drive subsystem via
                                     *           the config-apply (FUN_08031728). */
    uint8_t  _pad1b[0x3A];          /* +0x10A..+0x143 */
    uint8_t  region_lock;           /* +0x144 — region lock state: 1=off-road
                                     *           disabled, 2=locked, else unlocked.
                                     *           Read (not set) by the `region` cmd. */
    uint8_t  _pad1c[0x80];          /* +0x145..+0x1C4 — rest of the snapshotted
                                     *                  block; not individually
                                     *                  decoded yet. */
    uint8_t  _pad2[0x114];          /* +0x1C5..+0x2D8 */
    uint8_t  logged_in;             /* +0x2D9 — non-zero once login succeeded */
    uint8_t  _pad3[0x06];           /* +0x2DA..+0x2DF */
    uint8_t  fail_count;            /* +0x2E0 — consecutive failed login tries */
    uint8_t  _pad4[0xB7];           /* +0x2E1..+0x397 */
    char     user_password[0x3C];   /* +0x398..+0x3D3 — user-configurable
                                     *           service password; empty
                                     *           string means "not set".
                                     *           Length cap is whatever the
                                     *           set-password handler
                                     *           enforces (not yet
                                     *           decoded); the buffer is
                                     *           bounded by `set_soc` at
                                     *           +0x3D4. The hard-coded
                                     *           backdoor at OEM rodata
                                     *           0x080547EC is 40 chars
                                     *           plus NUL, well within. */
    uint8_t  set_soc;               /* +0x3D4 — SOC override byte; written by the
                                     *           console `gear` command (help
                                     *           "set gear", but the handler
                                     *           prints "Set SOC %d" and writes
                                     *           here — a firmware relabel quirk);
                                     *           announced via announce_mark(2). */
    /* ... grows further; not yet decoded. */
};
_Static_assert(__builtin_offsetof(struct session_ctx, audio_engine_cfg) == 0xF4, "");
_Static_assert(__builtin_offsetof(struct session_ctx, volume_low)       == 0x105, "");
_Static_assert(__builtin_offsetof(struct session_ctx, volume_medium)    == 0x106, "");
_Static_assert(__builtin_offsetof(struct session_ctx, volume_high)      == 0x107, "");
_Static_assert(__builtin_offsetof(struct session_ctx, region)           == 0x109, "");
_Static_assert(__builtin_offsetof(struct session_ctx, region_lock)      == 0x144, "");
_Static_assert(__builtin_offsetof(struct session_ctx, logged_in)        == 0x2D9, "");
_Static_assert(__builtin_offsetof(struct session_ctx, fail_count)       == 0x2E0, "");
_Static_assert(__builtin_offsetof(struct session_ctx, user_password)    == 0x398, "");
_Static_assert(__builtin_offsetof(struct session_ctx, set_soc)          == 0x3D4, "");

struct app_state {
    uint8_t  _pad0[0x2DC];        /* +0x000..+0x2DB */
    struct session_ctx *ctx_sub;  /* +0x2DC — points into a per-session block */
    /* ... grows further; not yet decoded. */
};
_Static_assert(__builtin_offsetof(struct app_state, ctx_sub) == 0x2DC, "");

extern struct app_state g_app_state;   /* SRAM 0x20009368 */

#endif
