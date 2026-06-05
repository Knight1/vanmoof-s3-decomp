#ifndef MAINWARE_CONSOLE_H
#define MAINWARE_CONSOLE_H

/* Debug serial-console handlers for the "ES3" prompt exposed by
 * mainware on the main-controller's debug UART. The console reads a
 * line at a time and invokes one of several per-state callbacks
 * (login, set user password, set admin password, set baud, …). */

/* `console_next_token(pp)` — advance `*pp` past the current token and the run
 * of delimiters after it, leaving it at the start of the next token. Tokens
 * are separated by space / `.` / `:` (and terminated by NUL). Returns 1 if a
 * next token exists, 0 if the line is exhausted. Every command handler that
 * takes an argument calls this first. */
int console_next_token(char **pp);

/* `login_handler(input)` — called by the console with a NUL-terminated
 * input line while the console is in the "login" state. Matches the
 * line against the user-configurable service password
 * (`g_app_state.ctx_sub->user_password`) and, failing that, against a
 * hard-coded fallback embedded in the image. On success it sets
 * `g_app_state.ctx_sub->logged_in = 1`; on failure it counts attempts
 * and arms a 5-second lockout via the scheduler. */
void login_handler(char *input);

/* `vollow` / `volmid` / `volhigh` console commands — set the three volume bytes
 * (offsets `+0x105` / `+0x106` / `+0x107` of the per-session context). With no
 * argument, print the current value; with an argument, parse it as a decimal in
 * `[0, 64]` (the `"Volume 0..64"` range), apply it, drive the audio amp via
 * GPIO, and run a config-persist step. A parsed `0` switches the amp off. (The
 * low/medium/high handler→byte binding is per the console command table; the
 * `"Volume Low/Medium/High"` dump labels alone are off by one.) */
void volume_low_set(char *input);
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

/* `console_region_set` — the `region` command. Sets the bike's region / speed
 * mode (0=EU, 1=US, 2=JP, 3=OFFROAD; OFFROAD lifts the speed cap) and echoes
 * the current lock state + region. */
void console_region_set(char *input);

/* Additional debug-console command handlers (dispatch table @ 0x0804F5C4; full
 * 49-command map in docs/console.md). All take the argument string. */
void console_cmd_distance(char *args);         /* `distance`  0x08041360 */
void console_cmd_wheelsize(char *args);        /* `wheelsize` 0x08042120 */
void console_cmd_speed(char *args);            /* `speed`     0x0804131C */
void console_cmd_shipping(char *args);         /* `shipping`  0x080415D0 */
void console_cmd_factory_shipping(char *args); /* `factory-shipping` 0x08041FF8 (no return) */
void console_cmd_gsminfo(char *args);          /* `gsminfo`   0x08040D14 */
void console_cmd_gsmstart(char *args);         /* `gsmstart`  0x08041D38 */
void console_cmd_bwritereg(char *args);        /* `bwritereg` 0x08041C84 */
void console_cmd_breadreg(char *args);         /* `breadreg`  0x08041B30 */
void console_cmd_swritedata(char *args);       /* `swritedata` 0x0804168C */
void console_cmd_sreadreg(char *args);         /* `sreadreg`  0x080414A4 */

#endif
