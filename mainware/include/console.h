#ifndef MAINWARE_CONSOLE_H
#define MAINWARE_CONSOLE_H

/* Debug serial-console handlers for the "ES3" prompt exposed by
 * mainware on the main-controller's debug UART. The console reads a
 * line at a time and invokes one of several per-state callbacks
 * (login, set user password, set admin password, set baud, …). */

/* `login_handler(input)` — called by the console with a NUL-terminated
 * input line while the console is in the "login" state. Matches the
 * line against the user-configurable service password
 * (`g_app_state.ctx_sub->user_password`) and, failing that, against a
 * hard-coded fallback embedded in the image. On success it sets
 * `g_app_state.ctx_sub->logged_in = 1`; on failure it counts attempts
 * and arms a 5-second lockout via the scheduler. */
void login_handler(char *input);

/* `volume_medium_set` / `volume_high_set` — set the two volume bytes
 * the console exposes (offsets `+0x105` / `+0x106` of the per-session
 * context). With no argument, just print the current value; with an
 * argument, parse it as a decimal in `[0, 64]` (the `"Volume 0..64"`
 * range), apply it, drive the audio amp via GPIO, and run an audio-
 * config apply step. A parsed `0` switches the amp off entirely.
 * The "low" counterpart at `+0x104` is wired up by a different (not
 * yet decoded) handler. */
void volume_medium_set(char *input);
void volume_high_set(char *input);

/* `console_start_motor_update` — single-line "start motor update" stub
 * that just logs and asks the runtime to enter the motor-update
 * subsystem mode. */
void console_start_motor_update(char *input);

/* `console_soc_set` — parse an int from the input line and stash it in
 * `ctx_sub->set_soc` (+0x3D4), then announce the change to whichever
 * subsystem subscribes to that channel. */
void console_soc_set(char *input);

#endif
