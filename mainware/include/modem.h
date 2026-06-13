#ifndef MAINWARE_MODEM_H
#define MAINWARE_MODEM_H

#include <stdint.h>

/*
 * mainware — u-blox SARA cellular modem driver.
 *
 * The bike's "phone home" path. A u-blox SARA module hangs off a UART; the
 * firmware drives it with a two-level state machine:
 *
 *   modem_sim_state_machine (the outer SM, one tick per super-loop) selects a
 *   high-level *state* (POWERON, SMS, PDP-context, PING, MESSAGE, LOCATION,
 *   POWEROFF — see modem_sm_state_t). Each state is implemented by a
 *   `modem_step_*` sub-state-machine that walks a list of AT commands.
 *
 *   modem_at_exec is the bottom layer: it sends one AT command, waits for the
 *   expected response (with ret/timeout), and reports busy/done/fail. The
 *   per-command parameters live in a flash *script table* (modem_at_entry_t).
 *
 * The SIM is locked: sim_iccid_check compares the SIM's ICCID against the
 * VanMoof Vodafone-NL prefix "8931440400" — see docs/modem.md.
 *
 * Modem context (working state, AT scratch buffer, per-step sub-counters) is a
 * fixed SRAM struct (OEM 0x20000294). The backend hosts and HTTP endpoints are
 * documented in docs/modem.md and net.c.
 */

/* High-level modem states (the codes modem_sm_state_name maps to strings). */
typedef enum {
    MODEM_IDLE          = 0,
    MODEM_POWERON       = 1,   /* power rails + PWR_KEY pulse + AT init handshake */
    MODEM_SMS_INIT      = 2,
    MODEM_SMS_READ      = 3,
    MODEM_SMS_WRITE     = 4,
    MODEM_CTX_ACTIVATE  = 5,   /* PDP/HTTP context up */
    MODEM_CTX_DEACTIVATE= 6,
    MODEM_PING_SEND     = 7,
    MODEM_MESSAGE_SEND  = 8,   /* HTTPS POST /bike-message */
    MODEM_LOCATION_SEND = 9,
    MODEM_POWEROFF      = 10,  /* AT shutdown, wait Vgsm<200mV, drop power rails */
} modem_sm_state_t;

/*
 * One entry of the flash AT-command script (OEM 0x08043EDC, 0x20 bytes each).
 * The script is one flat table carved into per-state command arrays at fixed
 * byte offsets; see MODEM_AT_TABLE in modem.c.
 */
typedef struct {
    const char *fmt;        /* +0x00  AT command (printf format, e.g. "AT+CPIN=\"%s\"\r\n") */
    uint32_t    num_resp;   /* +0x04  number of response lines expected before the match */
    const char *expect;     /* +0x08  expected reply substring ("OK", "+", ">" …)         */
    uint32_t    timeout1;   /* +0x0C  inter-byte / send timeout (ms)                       */
    uint32_t    retries;    /* +0x10  retry count (passed to modem_at_exec as a byte)      */
    uint32_t    timeout2;   /* +0x14  response timeout (ms)                                */
    uint32_t    build_cb;   /* +0x18  optional custom request-builder callback (0 = none)  */
    uint32_t    handle_cb;  /* +0x1C  optional response-handler callback (0 = none)        */
} modem_at_entry_t;

/* The outer SIM/modem state machine — ticked once per super-loop. */
char modem_sim_state_machine(void);

/* Map a modem_sm_state_t to its log name ("IDLE", "POWERON", …). */
const char *modem_sm_state_name(uint32_t state);

/* Optional per-command callbacks: build_cb formats a custom request into the
 * packet {txbuf, bufsize, fmt}; handle_cb post-processes a matched response.
 * Both return 0 done / 1 busy / 2 alt-done. A null pointer selects the default
 * (snprintf the format / accept the match). */
typedef int (*modem_at_cb_t)(void *pkt);

/* Run one AT command from the script: send, await `expect`, retry up to
 * `retries`. Returns 1 = busy/in-progress, 0 = done-OK, 2/3 = retry/fail. */
int modem_at_exec(void *txbuf, int bufsize, const char *fmt, int num_resp,
                  char *expect, unsigned int timeout1, char retries,
                  unsigned int timeout2, modem_at_cb_t build_cb,
                  modem_at_cb_t handle_cb);

/* AT response matcher: true if the last reply line equals `expect`. */
int modem_at_response_match(int buf, int len, char *expect);

/* The per-state sub-state-machines (each returns 1 busy / 0 done / fail). */
unsigned int modem_step_poweron(void);
unsigned int modem_step_poweroff(void);
int  modem_step_sms_init(void);
int  modem_step_sms_read(void);
int  modem_step_sms_write(void);
int  modem_step_ctx_activate(void);
unsigned int modem_step_ctx_deactivate(void);
int  modem_step_ping_send(void);
int  modem_step_message_send(void);
int  modem_step_location_send(void);

/* Non-zero once the modem has read valid info (ICCID/IMSI/IMEI) — a bus poll. */
int modem_info_ready(void);

/* The network-registration flag getter (logs + returns the cached flag). */
uint8_t modem_registration_get(void);

/* Validate the SIM's ICCID against the VanMoof Vodafone-NL prefix; logs
 * "Wrong iccid" + Holder/PCB on mismatch. Runs at the POWEROFF recycle. */
void sim_iccid_check(void);

/* USART2 byte transport for the modem AT channel. modem_uart_putc is the locked
 * single-byte TX (OEM 0x080360F8, returns the ring-push status); modem_uart_flush
 * (above, extern) is its NUL-terminated-string wrapper. usart2_irq_handler is the
 * RX/TX byte-pump ISR (OEM 0x0803617C). */
int  modem_uart_putc(uint8_t b);
void usart2_irq_handler(void);

/* SMS info-tracking one-shot latch (flag @ 0x2000839C). latch_once returns 0 on
 * the first call (and arms the flag), 1 afterwards; _get reads the flag. OEM
 * 0x0803D648 / 0x0803D65C. */
int     sms_tracking_latch_once(void);
uint8_t sms_tracking_get(void);

#endif
