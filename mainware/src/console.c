#include <stdint.h>
#include <string.h>

#include "app_state.h"
#include "console.h"
#include "log.h"
#include "scheduler.h"

#define LOGIN_FAIL_LIMIT      5u     /* lockout after N consecutive bad lines */
#define LOGIN_LOCKOUT_TICKS   5000u  /* 0x1388 — 5 s at 1 ms/tick */

/* Hard-coded fallback password. Reading the OEM rodata at 0x080547EC.
 * Accepted in addition to whatever the user has stored in
 * g_app_state.ctx_sub->user_password — works even when that slot is
 * empty, since strcmp against an empty user_password always returns
 * non-zero for non-empty input and the second comparison runs
 * unconditionally. */
static const char k_login_fallback_password[] =
    "vEVjGF!paYsM2EBV8SoDT8*T0eB&#T6xevaoxCaO";

void login_handler(char *input)
{
    /* If a previous lockout has armed g_console_state.login_state with
     * a scheduler slot, see whether that slot has expired. If so,
     * release it (which also resets login_state back to
     * SCHED_SLOT_NONE) and clear the fail counter before processing
     * this line. */
    if (scheduler_slot_is_idle(g_console_state.login_state)) {
        scheduler_release(&g_console_state.login_state);
        g_app_state.ctx_sub->fail_count = 0;
    }

    if (input[0] == '\0') {
        return;
    }

    if (g_console_state.login_state != SCHED_SLOT_NONE) {
        /* Lockout slot still has time on it. Re-arm it to a fresh
         * window — any input typed during cooldown extends the wait —
         * and stall the user. */
        scheduler_start(g_console_state.login_state, LOGIN_LOCKOUT_TICKS, 0);
        g_log_func("Please wait..");
        return;
    }

    /* Try the user-configurable service password first. The
     * `user_password[0] != '\0'` guard is defensive: strcmp wouldn't
     * report a match between non-empty input and an empty stored
     * password anyway, but the OEM code re-checks. */
    if (strcmp(input, g_app_state.ctx_sub->user_password) == 0
            && g_app_state.ctx_sub->user_password[0] != '\0') {
        goto login_ok;
    }
    /* Fall back to the hard-coded password. */
    if (strcmp(input, k_login_fallback_password) == 0) {
        goto login_ok;
    }

    /* Mismatch. Log + count + arm lockout on the 5th consecutive
     * failure. The compare is on the pre-increment value, so the
     * branch fires when this call is the LIMIT-th failure. */
    g_log_func("Error login\r\n");
    {
        uint8_t fc = g_app_state.ctx_sub->fail_count;
        g_app_state.ctx_sub->fail_count = (uint8_t)(fc + 1u);
        if (fc == LOGIN_FAIL_LIMIT - 1u) {
            uint8_t slot = scheduler_alloc();
            g_console_state.login_state = slot;
            scheduler_start(slot, LOGIN_LOCKOUT_TICKS, 0);
        }
    }
    return;

login_ok:
    g_app_state.ctx_sub->fail_count = 0;
    g_app_state.ctx_sub->logged_in = 1;
    g_log_func("\r\nWelcome to ES3\r\n");
}
