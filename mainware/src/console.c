#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "app_state.h"
#include "console.h"
#include "log.h"
#include "scheduler.h"
#include "stm32f413_gpio.h"

#define LOGIN_FAIL_LIMIT      5u     /* lockout after N consecutive bad lines */
#define LOGIN_LOCKOUT_TICKS   5000u  /* 0x1388 — 5 s at 1 ms/tick */

#define VOLUME_MAX            0x40u  /* "Volume 0..64" range check */
#define AUDIO_AMP_ENABLE_PIN  (1u << 2)  /* PE2 — drive high to enable amp */
#define AUDIO_AMP_MUTE_PIN    (1u << 5)  /* PD5 — drive high to mute */

/* Helpers we recognise (vendor-stock CubeF4 HAL + newlib). They're
 * supplied by the vendored upstream sources at link-time. */
extern void HAL_GPIO_WritePin(void *GPIOx, uint16_t pin_mask, int state);

/* Helpers we have not decoded yet — kept as opaque extern declarations
 * so we can call them at the right addresses for behavioural
 * equivalence without committing to a name we'd have to revise. */
extern uint32_t FUN_08031728(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
extern int      FUN_080391B8(uint8_t *p);

/* Hard-coded fallback password. Reading the OEM rodata at 0x080547EC.
 * Accepted in addition to whatever the user has stored in
 * g_app_state.ctx_sub->user_password — works even when that slot is
 * empty, since strcmp against an empty user_password always returns
 * non-zero for non-empty input and the second comparison runs
 * unconditionally. */
static const char k_login_fallback_password[] =
    "vEVjGF!paYsM2EBV8SoDT8*T0eB&#T6xevaoxCaO";

/* Console line tokenizer (OEM 0x08040A5C). Delimiters are space / `.` / `:`;
 * the line terminator is NUL. Leaves `*pp` at the first character of the next
 * token (or on the terminating NUL when the line runs out). */
int console_next_token(char **pp)
{
    char c;

    if (**pp == '\0') {
        return 0;
    }

    /* Skip the current token. */
    for (;;) {
        c = **pp;
        if (c == '\0' || c == ' ' || c == '.' || c == ':') {
            break;
        }
        (*pp)++;
    }

    /* Skip the run of delimiters to the next token. */
    for (;;) {
        c = **pp;
        if ((c != ' ' && c != '.' && c != ':') || c == '\0') {
            break;
        }
        (*pp)++;
    }

    return c != '\0';
}

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

/* Internal: shared body of `volume_medium_set` / `volume_high_set` —
 * they differ only in which byte in `ctx_sub` receives the parsed
 * value. The function deliberately mirrors the OEM control flow,
 * including the snapshot memcpy of `ctx_sub[0x104..0x1C4]` onto the
 * stack just before the audio-engine apply call. The snapshot
 * doesn't seem to be read back anywhere we've decoded, but it's
 * preserved verbatim so we don't accidentally drop a side-effect
 * that some not-yet-decoded callee depends on. */
static void volume_set_common(char *input, uint8_t *target)
{
    char     snapshot[0xC0];
    char    *cursor = input;
    uint8_t  parsed;

    if (!console_next_token(&cursor)) {
        g_log_func("Volume %d\r\n", *target);
        return;
    }

    parsed = (uint8_t)strtol(cursor, NULL, 10);
    if (parsed > VOLUME_MAX) {
        g_log_func("Volume 0..64\r\n");
        return;
    }

    /* Drive the audio amp: PE2 high, PD5 low (un-mute / power-up). */
    HAL_GPIO_WritePin((void *)GPIOE_BASE, AUDIO_AMP_ENABLE_PIN, 1);
    HAL_GPIO_WritePin((void *)GPIOD_BASE, AUDIO_AMP_MUTE_PIN,   0);

    *target = parsed;
    g_log_func("Volume %d\r\n", parsed);

    memcpy(snapshot,
           (void *)((char *)g_app_state.ctx_sub + 0x104),
           sizeof snapshot);

    {
        uint32_t res = FUN_08031728(
            g_app_state.ctx_sub->audio_engine_cfg[0],
            g_app_state.ctx_sub->audio_engine_cfg[1],
            g_app_state.ctx_sub->audio_engine_cfg[2],
            g_app_state.ctx_sub->audio_engine_cfg[3]);
        g_log_func("res: %d\r\n", res);
    }

    if (FUN_080391B8(&parsed) != 0) {
        g_log_func(" ERR set volume\r\n");
    }

    if (parsed == 0) {
        /* Power down: invert the amp pins, print confirmation. */
        HAL_GPIO_WritePin((void *)GPIOE_BASE, AUDIO_AMP_ENABLE_PIN, 0);
        HAL_GPIO_WritePin((void *)GPIOD_BASE, AUDIO_AMP_MUTE_PIN,   1);
        g_log_func("Audio off\r\n");
    }

    (void)snapshot;  /* keep snapshot live across the apply call */
}

void volume_high_set(char *input)
{
    volume_set_common(input, &g_app_state.ctx_sub->volume_high);
}

void volume_medium_set(char *input)
{
    volume_set_common(input, &g_app_state.ctx_sub->volume_medium);
}

void console_start_motor_update(char *input)
{
    (void)input;
    g_log_func("Start motor update..");
    update_mode_request(4);
}

void console_soc_set(char *input)
{
    char *cursor = input;
    long parsed;

    if (!console_next_token(&cursor)) {
        return;
    }

    parsed = strtol(cursor, NULL, 10);
    g_app_state.ctx_sub->set_soc = (uint8_t)parsed;
    g_log_func("Set SOC %d\r\n", (uint16_t)parsed);
    /* OEM passes 2 here, which announce_mark() ignores (it only handles
     * channels 0/1). The SOC override is picked up directly by the super-loop
     * each iteration, so the no-op call has no observable effect. */
    announce_mark(2);
}

/* `region` console command (OEM 0x080421CC). Sets the bike's region / speed
 * mode — 0=EU, 1=US, 2=JP, 3=OFFROAD (OFFROAD lifts the regulated speed cap) —
 * then echoes the current lock state and region. The lock state at `+0x144` is
 * read and written back unchanged (only the region is set here); a value < 3 is
 * required to accept a change. Applying the region re-runs the config-apply
 * (FUN_08031728) that pushes the `ctx_sub[0xF4..0x103]` block to the drive
 * subsystem — the same path the volume commands use; the OEM snapshots
 * `ctx_sub[0x104..0x1C4]` onto the stack first because the apply helper reads
 * it back from the caller's frame. */
void console_region_set(char *input)
{
    char     snapshot[0xC0];
    char    *cursor = input;
    uint8_t  lock = g_app_state.ctx_sub->region_lock;

    if (console_next_token(&cursor)) {
        unsigned long parsed = (unsigned long)strtol(cursor, NULL, 10);

        if ((parsed & 0xFFFFu) < 4u && lock < 3u) {
            g_app_state.ctx_sub->region = (uint8_t)parsed;
            g_app_state.ctx_sub->region_lock = lock;

            memcpy(snapshot,
                   (void *)((char *)g_app_state.ctx_sub + 0x104),
                   sizeof snapshot);

            {
                uint32_t res = FUN_08031728(
                    g_app_state.ctx_sub->audio_engine_cfg[0],
                    g_app_state.ctx_sub->audio_engine_cfg[1],
                    g_app_state.ctx_sub->audio_engine_cfg[2],
                    g_app_state.ctx_sub->audio_engine_cfg[3]);
                g_log_func("Set region and lock, res: %d\r\n", res);
            }
            (void)snapshot;
        } else {
            g_log_func("Parameter 0..3 0..2\r\n");
        }
    }

    switch (g_app_state.ctx_sub->region_lock) {
    case 1:  g_log_func("Region (off road disabled): "); break;
    case 2:  g_log_func("Region (locked): ");            break;
    default: g_log_func("Region (unlocked): ");          break;
    }

    switch (g_app_state.ctx_sub->region) {
    case 0: g_log_func("0 = REGION_EU\r\n");      break;
    case 1: g_log_func("1 = REGION_US\r\n");      break;
    case 2: g_log_func("2 = REGION_JP\r\n");      break;
    case 3: g_log_func("3 = REGION_OFFROAD\r\n"); break;
    }
}
