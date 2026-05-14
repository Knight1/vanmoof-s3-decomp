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

#endif
