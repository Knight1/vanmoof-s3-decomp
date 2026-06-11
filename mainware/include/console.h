#ifndef MAINWARE_CONSOLE_H
#define MAINWARE_CONSOLE_H

#include <stdint.h>

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
void console_cmd_battery(char *args);          /* `battery`   0x08042F28 */
void console_cmd_shifterstatus(char *args);    /* `shifterstatus` 0x08042F74 */
void console_cmd_motorstatus(char *args);      /* `motorstatus` 0x08042E54 */
void console_cmd_adc(char *args);              /* `adc`       0x08043028 */
void console_cmd_stc(char *args);              /* `stc`       0x08041614 */
void console_cmd_batware(char *args);          /* `batware`   0x08041E50 */
void console_cmd_logprn(char *args);           /* `logprn`    0x08041F88 */
void console_cmd_logclr(char *args);           /* `logclr`    0x08041F34 */
void console_cmd_factory(char *args);          /* `factory`   0x08041E70 */
void console_cmd_reboot(char *args);           /* `reboot`    0x08041DA4 */
void console_cmd_sound(char *args);            /* `sound`     0x08041D10 */
void console_cmd_bwritedata(char *args);       /* `bwritedata` 0x08041BB4 */
void console_cmd_swritereg(char *args);        /* `swritereg` 0x08041528 */
void console_cmd_help(char *args);             /* `help`      0x08040AA0 */
void console_cmd_logout(char *args);           /* `logout`    0x08040A4C */
void console_cmd_blereset(char *args);         /* `blereset`  0x08041FB8 */
void console_cmd_bledebug(char *args);         /* `bledebug`  0x08040C6C (→UART7) */
void console_cmd_loop(char *args);             /* `loop`      0x08040C28 */
void console_cmd_logapp(char *args);           /* `logapp`    0x08041E94 */
void console_cmd_powerchange(char *args);      /* `powerchange` 0x080412BC */
void console_cmd_batreset(char *args);         /* `batreset`  0x08041DD8 */
void console_cmd_shiftware(char *args);        /* `shiftware` 0x08041DBC */
void console_cmd_shiftdebug(char *args);       /* `shiftdebug` 0x08041D50 */
void console_cmd_shiftresetcounter(char *args);/* `shiftresetcounter` 0x08040CB4 */
void console_cmd_gsmdebug(char *args);         /* `gsmdebug`  0x08040C90 (→UART2) */
void console_cmd_bmsdebug(char *args);         /* `bmsdebug`  0x08040CD8 */
void console_cmd_stcreset(char *args);         /* `stcreset`  0x080415EC */
void console_cmd_setoad(char *args);           /* `setoad`    0x080415B4 */
void console_cmd_setgear(char *args);          /* `setgear`   0x080413B4 */

/* ── UART8 (the ES3 console prompt) byte transport ──────────────────────────
 * Installed into the g_log_func dispatch table by console_io_table_install:
 * [0]=console_printf, [1]=uart8_tx_byte, [2]=uart8_puts, [3]=uart8_write,
 * [4]=uart_rx_ringbuf_get_byte. console_printf is g_log_func[0], the
 * firmware-wide printf. */
int  console_printf(const char *fmt, ...);          /* OEM 0x080367F0 */
int  uart8_tx_byte(uint8_t b);                       /* OEM 0x08036754 */
void uart8_puts(const char *s);                      /* OEM 0x0803678C */
void uart8_write(const uint8_t *buf, uint16_t len);  /* OEM 0x0803679E */
int  uart_rx_ringbuf_get_byte(uint8_t *out);         /* OEM 0x080367B8 (1 = got a byte) */
void uart8_irq_handler(void);                        /* OEM 0x080368D4 */

/* ── USART1 (the second ES3 console port, twin of UART8) byte transport ──────
 * Bound into g_log_func by usart1_io_table_install while USART1 is the active
 * console; usart1_printf is then g_log_func[0]. */
int  usart1_printf(const char *fmt, ...);            /* OEM 0x08035EBC */
int  usart1_tx_byte(uint8_t b);                      /* OEM 0x08035E28 */
void usart1_puts(const char *s);                     /* OEM 0x08035E5C */
void usart1_write(const uint8_t *buf, uint16_t len); /* OEM 0x08035E6E */
int  usart1_rx_byte(uint8_t *out);                   /* OEM 0x08035E88 (1 = got a byte) */
void usart1_irq_handler(void);                       /* OEM 0x08035F98 */

#endif
