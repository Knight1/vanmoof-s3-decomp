/*
 * modem.c — u-blox SARA-G350 cellular modem driver (the bike's "phone home").
 *
 * Two-level state machine (see modem.h and docs/modem.md):
 *
 *   modem_sim_state_machine  (outer SM, named+documented in docs/modem.md)
 *     -> modem_step_*        (per-state sub-state-machines, sourced here)
 *          -> modem_at_exec  (send one AT command, await response, retry)
 *
 * The modem context — AT-engine working state, the AT tx scratch buffer, and a
 * per-step (substate,substep) byte pair for every modem_step_* — is one fixed
 * SRAM struct (OEM 0x20000294). The AT commands themselves live in a flash
 * script table (OEM 0x08043EDC, modem_at_entry_t entries) carved into per-state
 * arrays at fixed byte offsets (MODEM_AT_TABLE).
 *
 * Behaviour-equivalent reconstruction (not byte-identical). RAM working blocks
 * and the flash script table are declared extern / linker-placed; their OEM
 * absolute addresses are noted in comments.
 */

#include <stdint.h>
#include "modem.h"
#include "log.h"
#include "scheduler.h"

/* ------------------------------------------------------------------ *
 *  externs — HAL / libc / cross-module helpers (OEM addresses noted)
 * ------------------------------------------------------------------ */
extern void  HAL_GPIO_WritePin(void *GPIOx, uint16_t pin_mask, int state); /* 0x08026AC6 */
extern int   HAL_GPIO_ReadPin(void *GPIOx, uint16_t pin_mask);             /* 0x08026AB8 */
extern int   snprintf(char *s, unsigned int n, const char *fmt, ...);
extern void *memset(void *s, int c, unsigned int n);

/* The modem UART (the SARA AT channel) ring transport. */
extern int   modem_uart_rx_byte(char *out);          /* 0x08036144: 0 = got a byte */
extern int   modem_uart_tx_byte(void *h, char b);    /* 0x08035C94: 0/2 status      */
extern void  modem_uart_flush(void *buf);            /* 0x08036130                  */
extern void  modem_at_response_copy(void *dst, int src); /* 0x0802F404              */

/* Bounded byte compare (OEM 0x0802181C — the GNU word-at-a-time strncmp). */
extern int   bounded_strncmp(const void *a, const void *b, unsigned int n);

/* Read the modem-supply rail Vgsm in mV (ADC); used to confirm power-down. */
extern unsigned int adc_read_vgsm(void);

/* Post an asynchronous request onto the inter-module/event bus (OEM 0x0802A268).
 * The second argument is a region/event tag. */
extern void async_request_post(unsigned int a, unsigned int tag);

/* The console `ver` handler — prints the firmware version banner (OEM, named in
 * console.c but not sourced). sim_iccid_check calls it at the POWEROFF recycle. */
extern void console_cmd_ver(const char *args);

/* ------------------------------------------------------------------ *
 *  RAM working state (OEM absolute addresses in comments)
 * ------------------------------------------------------------------ */

/* Modem working context (OEM SRAM 0x20000294). Accessed by byte offset:
 *   [0]      AT-engine state (0 idle / 1 sending / 2 waiting)
 *   [1]      AT retry counter
 *   [4..7]   saved AT tx buffer pointer (doubles as the UART handle, +4)
 *   [8..0xB] saved buffer size
 *   [0xC..F] received-byte counter
 *   [0x10]   expected response count
 *   [0x14/15] POWERON  (substate,substep)
 *   [0x18..] AT tx scratch buffer
 *   [0x218/219] POWEROFF      [0x21A/B] SMS_INIT    [0x21C/D] SMS_READ
 *   [0x21E/F] SMS_WRITE        [0x220/1] CTX_ACT     [0x222/3] CTX_DEACT
 *   [0x224/5] PING             [0x226/7] MESSAGE     [0x228/9] LOCATION */
extern uint8_t g_modem_ctx[];

/* The modem timer block (OEM 0x20000070): [0]/[1] are modem_at_exec's send and
 * response scheduler slots — reused by POWERON for its PWR_KEY timing — and [2]
 * is POWERON's live AT-init command count (set from the detected model). */
extern uint8_t g_modem_at_timer[3];

/* AT+CGMM model response (OEM 0x20009CD0), matched against "SARA"/"LARA" to pick
 * the init command count. Non-empty first byte = a model string was received. */
extern char g_modem_model_resp[];

/* POWEROFF timer slots (OEM DAT_080301A8): modem timers at [0]/[1], 2-second
 * shutdown guard at [3]; plus a separate guard slot (OEM DAT_080301B0). */
extern uint8_t g_modem_pwroff[4];
extern uint8_t g_modem_pwroff_guard;

/* The flash AT-command script (OEM 0x08043EDC). One flat table, carved into
 * per-state command arrays at fixed byte offsets; each entry is 0x20 bytes. */
extern const uint8_t g_modem_at_script[];
#define MODEM_AT_TABLE(state_off) \
    ((const modem_at_entry_t *)(g_modem_at_script + (state_off)))

/* Per-state byte offsets of the command arrays inside g_modem_at_script. */
#define AT_OFF_POWERON_OK   0x000u   /* AT init: AT/CGMI/CGMM/CGMR/CGSN/CPIN/CIMI/CCID/CSQ/CREG/COPS/CEREG */
#define AT_OFF_POWERON_ALT  0x1C0u   /* CPIN= retry variant */
#define AT_OFF_POWEROFF     0x360u
#define AT_OFF_SMS_INIT     0x380u
#define AT_OFF_SMS_READ     0x3E0u
#define AT_OFF_SMS_WRITE    0x460u
#define AT_OFF_CTX_ACT      0x4A0u
#define AT_OFF_CTX_DEACT    0x520u
#define AT_OFF_PING         0x540u
#define AT_OFF_MESSAGE      0x5C0u
#define AT_OFF_LOCATION     0x640u

/* STM32F413 GPIO ports driving the modem power rails. */
#define GPIOA_BASE 0x40020000u
#define GPIOB_BASE 0x40020400u
#define GPIOE_BASE 0x40021000u

/* Modem power-control pins (see hardware.md). */
#define MODEM_PWR_EN_PIN   0x0010u   /* PB4  — main supply enable          */
#define MODEM_LVLSH_PIN    0x8000u   /* PA15 — level-shifter enable        */
#define MODEM_RESET_PIN    0x0040u   /* PE6  — reset (1 = held in reset)    */
#define MODEM_PWRKEY_PIN   0x0001u   /* PB0  — PWR_KEY (pulse to toggle)    */
#define MODEM_AUX_PIN      0x0002u   /* PB1                                 */
#define MODEM_SIM_DET_PIN  0x0400u   /* PE10 — SIM-present detect           */

/* Session/app context pointer (OEM SRAM 0x20000944 -> session_ctx). The modem
 * stores the parsed IMEI/IMSI/ICCID and CSQ into this block. */
#define G_CTX_PTR (*(uint8_t **)0x20000944u)

/* ================================================================== *
 *  AT-command engine
 * ================================================================== */

/* modem_at_response_match (OEM 0x0802F3A0): walk back over the reply buffer to
 * the start of the last line (the second '\r' from the end), then compare it to
 * `expect`. Returns non-zero on a full match. */
int modem_at_response_match(int buf, int len, char *expect)
{
    char cr_left = 2;
    const char *p = (const char *)(buf + len);
    const char *line;

    if (len == 0)
        return 0;

    do {
        line = p;
        if (*line == '\r')
            cr_left--;
        len--;
        p = line - 1;
    } while (cr_left != 0 && len != 0);

    if (cr_left != 0)
        return 0;

    for (;;) {
        line++;
        if (*expect == '\0' || *expect != *line)
            break;
        expect++;
    }
    return *expect == '\0';
}

/*
 * modem_at_exec (OEM 0x0802F9BC): the three-phase AT-command primitive.
 *   state 0 (idle):  arm the response timer, seed the retry count, advance.
 *   state 1 (send):  once the guard timer expires, format + flush the command,
 *                    arm both timers, advance to wait.
 *   state 2 (wait):  drain rx bytes, match `expect`; on match optionally run
 *                    handle_cb. Returns 1 busy / 0 done-ok / 2,3 retry/fail.
 */
int modem_at_exec(void *txbuf, int bufsize, const char *fmt, int num_resp,
                  char *expect, unsigned int timeout1, char retries,
                  unsigned int timeout2, modem_at_cb_t build_cb,
                  modem_at_cb_t handle_cb)
{
    char state = (char)g_modem_ctx[0];
    int  ret;
    short guard;
    char rx;
    void *uart_h = &g_modem_ctx[4];
    /* custom-builder packet: {txbuf, bufsize, fmt} */
    void *pkt[3];
    char resp_scratch[131];

    if (state == 1) {                 /* ---- SEND ---- */
        if (!scheduler_slot_is_idle(g_modem_at_timer[0])) {
            guard = 0x200;
            do { if (modem_uart_rx_byte(&rx) == 0) break; } while (--guard);
            if (guard == 0)
                g_log_func("GSM flush err\r\n");
            ret = 1;
        } else if (g_modem_ctx[1] == 0) {
            ret = (num_resp != 0) ? 3 : 0;
            g_modem_ctx[0] = 0;
        } else {
            g_modem_ctx[1]--;
            if (build_cb == 0) {
                snprintf(txbuf, bufsize, fmt);
            } else {
                pkt[0] = txbuf; pkt[1] = (void *)(intptr_t)bufsize; pkt[2] = (void *)(intptr_t)fmt;
                build_cb(pkt);
            }
            guard = 0x200;
            do { if (modem_uart_rx_byte(&rx) == 0) break; } while (--guard);
            if (guard == 0)
                g_log_func("GSM flush err\r\n");
            modem_uart_flush(txbuf);
            scheduler_start(g_modem_at_timer[0], timeout2, (sched_cb_t)0);
            *(void **)(g_modem_ctx + 4)  = txbuf;
            *(int *)(g_modem_ctx + 8)    = bufsize;
            g_modem_ctx[0xC] = 0; g_modem_ctx[0xD] = 0;
            g_modem_ctx[0xE] = 0; g_modem_ctx[0xF] = 0;
            *(int *)(g_modem_ctx + 0x10) = num_resp;
            scheduler_start(g_modem_at_timer[1], timeout1, (sched_cb_t)0);
            g_modem_ctx[0]++;
            ret = 1;
        }
    } else if (state == 2) {          /* ---- WAIT ---- */
        if (!scheduler_slot_is_idle(g_modem_at_timer[1])) {
            guard = 0x200;
            ret = 1;
            while (modem_uart_rx_byte(&rx) != 0 && (--guard != 0)) {
                int r;
                if (*expect == '>' && rx == '>') {
                    modem_uart_tx_byte((void *)uart_h, '\0');
                    rx = '\r';
                }
                r = modem_uart_tx_byte((void *)uart_h, rx);
                if (r == 0) {
                    memset(resp_scratch, 0, 0x80);
                    modem_at_response_copy(resp_scratch, *(int *)(g_modem_ctx + 4));
                    if (modem_at_response_match(*(int *)(g_modem_ctx + 4),
                                                *(int *)(g_modem_ctx + 0xC), expect) == 0) {
                        g_modem_ctx[0]--;
                    } else if (handle_cb == 0) {
                        g_modem_ctx[0] = 0;
                        ret = 0;
                    } else {
                        pkt[0] = txbuf;
                        ret = handle_cb(pkt);
                        if (ret == 0 || ret == 2) {
                            g_modem_ctx[0] = 0;
                        } else {
                            g_modem_ctx[0]--;
                            ret = 1;
                        }
                    }
                } else if (r == 2) {
                    g_modem_ctx[0]--;
                }
            }
            if (guard == 0)
                g_log_func("GSM process err\r\n");
        } else {
            if (num_resp == 99 &&
                modem_at_response_match(*(int *)(g_modem_ctx + 4),
                                        *(int *)(g_modem_ctx + 0xC), expect) != 0) {
                if (handle_cb == 0) {
                    g_modem_ctx[0] = 0;
                    return 0;
                }
                pkt[0] = txbuf;
                ret = handle_cb(pkt);
                if (ret != 0 && ret != 2) {
                    g_modem_ctx[0]--;
                    return 1;
                }
                g_modem_ctx[0] = 0;
                return ret;
            }
            g_modem_ctx[0]--;
            ret = 1;
        }
    } else if (state == 0) {          /* ---- IDLE/START ---- */
        if (num_resp == 0) {
            g_modem_ctx[1] = 0;
            scheduler_start(g_modem_at_timer[0], timeout1, (sched_cb_t)0);
        } else if (num_resp == 99) {
            g_modem_ctx[1] = 1;
            scheduler_start(g_modem_at_timer[0], 100, (sched_cb_t)0);
        } else {
            g_modem_ctx[1] = retries;
            scheduler_start(g_modem_at_timer[0], 100, (sched_cb_t)0);
        }
        g_modem_ctx[0]++;
        ret = 1;
    } else {
        ret = 1;
    }
    return ret;
}

/* Run AT-script entry `idx` of a per-state command array (the common tail of
 * every simple modem_step_*). */
static int modem_run_at(unsigned int state_off, uint8_t idx)
{
    const modem_at_entry_t *e = &MODEM_AT_TABLE(state_off)[idx];
    return modem_at_exec(&g_modem_ctx[0x18], 0x200, e->fmt, e->num_resp,
                         (char *)e->expect, e->timeout1, (char)e->retries,
                         e->timeout2, (modem_at_cb_t)(uintptr_t)e->build_cb,
                         (modem_at_cb_t)(uintptr_t)e->handle_cb);
}

/* ================================================================== *
 *  Per-state sub-state-machines (OEM addresses noted)
 * ================================================================== */

/* modem_step_poweron (0x0802FDC0): power rails on, PWR_KEY pulse, then walk the
 * AT init handshake (AT, CGMI, CGMM, CGMR, CGSN, CPIN?, CPIN=, CIMI, CCID, CSQ,
 * CREG?, COPS?, CEREG?). Return encodes the discovered SIM/CPIN status. */
unsigned int modem_step_poweron(void)
{
    uint8_t substate = g_modem_ctx[0x14];
    unsigned int ret = substate;

    switch (substate) {
    case 0:
        g_modem_ctx[0x15] = 0;
        log_print_timestamp_prefix();
        g_log_func("Poweron g350\r\n");
        HAL_GPIO_WritePin((void *)GPIOB_BASE, MODEM_PWR_EN_PIN, 1);
        HAL_GPIO_WritePin((void *)GPIOA_BASE, MODEM_LVLSH_PIN, 1);
        HAL_GPIO_WritePin((void *)GPIOE_BASE, MODEM_RESET_PIN, 0);
        HAL_GPIO_WritePin((void *)GPIOB_BASE, MODEM_AUX_PIN, 0);
        HAL_GPIO_WritePin((void *)GPIOB_BASE, MODEM_PWRKEY_PIN, 0);
        if (g_modem_at_timer[0] == SCHED_SLOT_NONE) {
            g_modem_at_timer[0] = scheduler_alloc();
            scheduler_set_timer_name(g_modem_at_timer[0], 1, "timeout_tmr");
            scheduler_start(g_modem_at_timer[0], 1, (sched_cb_t)0);
        }
        if (g_modem_at_timer[1] == SCHED_SLOT_NONE) {
            g_modem_at_timer[1] = scheduler_alloc();
            scheduler_set_timer_name(g_modem_at_timer[1], 0x96, "timeout_tmr");
            scheduler_start(g_modem_at_timer[1], 0x96, (sched_cb_t)0);
        }
        g_modem_ctx[0x14]++;
        ret = 1;
        break;
    case 1:
        if (scheduler_slot_is_idle(g_modem_at_timer[1])) {
            scheduler_start(g_modem_at_timer[1], 0x96, (sched_cb_t)0);
            HAL_GPIO_WritePin((void *)GPIOB_BASE, MODEM_PWRKEY_PIN, 1);  /* PWR_KEY pulse */
            g_modem_ctx[0x14]++;
        }
        break;
    case 2:
        if (!scheduler_slot_is_idle(g_modem_at_timer[1])) {
            ret = 1;
        } else {
            HAL_GPIO_WritePin((void *)GPIOB_BASE, MODEM_PWRKEY_PIN, 0);
            g_modem_ctx[0x14]++;
            ret = 1;
        }
        break;
    case 3: {
        uint8_t idx = g_modem_ctx[0x15];
        /* Per command, pick the normal init table, or the CPIN= retry variant
         * (+0x1C0) once the model response no longer matches "LARA". */
        if (bounded_strncmp(g_modem_model_resp, "LARA", 4) == 0)
            ret = (unsigned int)modem_run_at(AT_OFF_POWERON_OK, idx);
        else
            ret = (unsigned int)modem_run_at(AT_OFF_POWERON_ALT, idx);

        /* On the model substep (cap still 4), set the real AT-init command count
         * from the detected u-blox model: LARA (LTE) = 14, SARA (2G) = 13. */
        if (g_modem_at_timer[2] == 4 && g_modem_model_resp[0] != '\0') {
            if (bounded_strncmp(g_modem_model_resp, "LARA", 4) == 0)
                g_modem_at_timer[2] = 0xE;
            else if (bounded_strncmp(g_modem_model_resp, "SARA", 4) == 0)
                g_modem_at_timer[2] = 0xD;
            else
                g_log_func("GSM Model unknown\r\n");
        }

        /* Remap a fail (3) into the SIM-status codes the outer SM reads. */
        if (idx == 5  && ret == 3) ret = 4;
        if (idx == 9  && ret == 3) ret = 6;
        if (idx == 10 && ret == 3) ret = 5;

        /* On done (0) / retry (2), advance the substep; finished when the new
         * index reaches the command count. Any other code (3/4/5/6) ends the
         * POWERON SM and is returned verbatim to the outer SM. */
        if (ret == 0 || ret == 2) {
            idx = (uint8_t)(idx + ((ret == 0) ? 1 : 2));
            g_modem_ctx[0x15] = idx;
            ret = (idx < g_modem_at_timer[2]) ? 1 : 0;
        }
        if (ret != 1)
            substate = 0;
        g_modem_ctx[0x14] = substate;
        break;
    }
    default:
        ret = 1;
        break;
    }
    return ret;
}

/* modem_step_sms_init (0x080301C4): walk the SMS-setup AT block (cap 3). */
int modem_step_sms_init(void)
{
    if (g_modem_ctx[0x21A] == 0) {
        g_modem_ctx[0x21B] = 0;
        g_modem_ctx[0x21A] = 1;
        return 1;
    }
    if (g_modem_ctx[0x21A] == 1) {
        int ret = modem_run_at(AT_OFF_SMS_INIT, g_modem_ctx[0x21B]);
        if (ret == 0 || ret == 2) {
            uint8_t next = (uint8_t)(g_modem_ctx[0x21B] + ((ret == 0) ? 1 : 2));
            g_modem_ctx[0x21B] = next;
            ret = (next < 3) ? 1 : 0;
        }
        g_modem_ctx[0x21A] = (ret == 1);
        return ret;
    }
    return 1;
}

/* modem_step_sms_read (0x08030264): SMS read/list AT block (cap 4). */
int modem_step_sms_read(void)
{
    if (g_modem_ctx[0x21C] == 0) {
        g_modem_ctx[0x21D] = 0;
        g_modem_ctx[0x21C] = 1;
        return 1;
    }
    if (g_modem_ctx[0x21C] == 1) {
        int ret = modem_run_at(AT_OFF_SMS_READ, g_modem_ctx[0x21D]);
        if (ret == 0 || ret == 2) {
            uint8_t next = (uint8_t)(g_modem_ctx[0x21D] + ((ret == 0) ? 1 : 2));
            g_modem_ctx[0x21D] = next;
            ret = (next < 4) ? 1 : 0;
        }
        g_modem_ctx[0x21C] = (ret == 1);
        return ret;
    }
    return 1;
}

/* modem_step_sms_write (0x08030304): SMS write/send AT block (cap 2). */
int modem_step_sms_write(void)
{
    if (g_modem_ctx[0x21E] == 0) {
        g_modem_ctx[0x21F] = 0;
        g_modem_ctx[0x21E] = 1;
        return 1;
    }
    if (g_modem_ctx[0x21E] == 1) {
        int ret = modem_run_at(AT_OFF_SMS_WRITE, g_modem_ctx[0x21F]);
        if (ret == 0 || ret == 2) {
            uint8_t next = (uint8_t)(g_modem_ctx[0x21F] + ((ret == 0) ? 1 : 2));
            g_modem_ctx[0x21F] = next;
            ret = (next < 2) ? 1 : 0;
        }
        g_modem_ctx[0x21E] = (ret == 1);
        return ret;
    }
    return 1;
}

/* modem_step_ctx_activate (0x080303A4): bring up the PDP/HTTP context (cap 4). */
int modem_step_ctx_activate(void)
{
    if (g_modem_ctx[0x220] == 0) {
        g_modem_ctx[0x221] = 0;
        g_modem_ctx[0x220] = 1;
        return 1;
    }
    if (g_modem_ctx[0x220] == 1) {
        int ret = modem_run_at(AT_OFF_CTX_ACT, g_modem_ctx[0x221]);
        if (ret == 0 || ret == 2) {
            uint8_t next = (uint8_t)(g_modem_ctx[0x221] + ((ret == 0) ? 1 : 2));
            g_modem_ctx[0x221] = next;
            ret = (next < 4) ? 1 : 0;
        }
        g_modem_ctx[0x220] = (ret == 1);
        return ret;
    }
    return 1;
}

/* modem_step_ctx_deactivate (0x08030444): tear the PDP context down. Completes
 * after the command array wraps (the OEM's degenerate `substep == 0` cap). */
unsigned int modem_step_ctx_deactivate(void)
{
    if (g_modem_ctx[0x222] == 0) {
        g_modem_ctx[0x223] = 0;
        g_modem_ctx[0x222] = 1;
        return 1;
    }
    if (g_modem_ctx[0x222] == 1) {
        unsigned int ret = (unsigned int)modem_run_at(AT_OFF_CTX_DEACT, g_modem_ctx[0x223]);
        if (ret == 0 || ret == 2) {
            char next = (char)(g_modem_ctx[0x223] + ((ret == 0) ? 1 : 2));
            g_modem_ctx[0x223] = (uint8_t)next;
            ret = (unsigned int)(next == 0);
        }
        g_modem_ctx[0x222] = (ret == 1);
        return ret;
    }
    return 1;
}

/* modem_step_ping_send (0x080304E0): keep-alive/ping AT block (cap 4). */
int modem_step_ping_send(void)
{
    if (g_modem_ctx[0x224] == 0) {
        g_modem_ctx[0x225] = 0;
        g_modem_ctx[0x224] = 1;
        return 1;
    }
    if (g_modem_ctx[0x224] == 1) {
        int ret = modem_run_at(AT_OFF_PING, g_modem_ctx[0x225]);
        if (ret == 0 || ret == 2) {
            uint8_t next = (uint8_t)(g_modem_ctx[0x225] + ((ret == 0) ? 1 : 2));
            g_modem_ctx[0x225] = next;
            ret = (next < 4) ? 1 : 0;
        }
        g_modem_ctx[0x224] = (ret == 1);
        return ret;
    }
    return 1;
}

/* modem_step_message_send (0x08030580): configure the HTTP profile and POST the
 * telemetry body — AT+UHTTP=0,1,"<host>" / =0,5,<port> / =0,6,1 (TLS) /
 * AT+UHTTPC=0,5,"/bike-message","https","{<body>}",6,"application/json" (cap 4). */
int modem_step_message_send(void)
{
    if (g_modem_ctx[0x226] == 0) {
        g_modem_ctx[0x227] = 0;
        g_modem_ctx[0x226] = 1;
        return 1;
    }
    if (g_modem_ctx[0x226] == 1) {
        int ret = modem_run_at(AT_OFF_MESSAGE, g_modem_ctx[0x227]);
        if (ret == 0 || ret == 2) {
            uint8_t next = (uint8_t)(g_modem_ctx[0x227] + ((ret == 0) ? 1 : 2));
            g_modem_ctx[0x227] = next;
            ret = (next < 4) ? 1 : 0;
        }
        g_modem_ctx[0x226] = (ret == 1);
        return ret;
    }
    return 1;
}

/* modem_step_location_send (0x08030620): cell-location report AT block (cap 9). */
int modem_step_location_send(void)
{
    if (g_modem_ctx[0x228] == 0) {
        g_modem_ctx[0x229] = 0;
        g_modem_ctx[0x228] = 1;
        return 1;
    }
    if (g_modem_ctx[0x228] == 1) {
        int ret = modem_run_at(AT_OFF_LOCATION, g_modem_ctx[0x229]);
        if (ret == 0 || ret == 2) {
            uint8_t next = (uint8_t)(g_modem_ctx[0x229] + ((ret == 0) ? 1 : 2));
            g_modem_ctx[0x229] = next;
            ret = (next < 9) ? 1 : 0;
        }
        g_modem_ctx[0x228] = (ret == 1);
        return ret;
    }
    return 1;
}

/* modem_step_poweroff (0x08030018): AT shutdown, wait for Vgsm to fall below
 * 200 mV (rail collapsed), then drop the power rails and assert reset. */
unsigned int modem_step_poweroff(void)
{
    uint8_t substate = g_modem_ctx[0x218];
    unsigned int ret = substate;
    int idle;
    unsigned int vgsm;

    switch (substate) {
    case 0:
        g_modem_ctx[0x219] = 0;
        log_print_timestamp_prefix();
        g_log_func("Poweroff g350..\r\n");
        g_modem_ctx[0x218]++;
        ret = 1;
        break;
    case 1: {
        int r = modem_run_at(AT_OFF_POWEROFF, g_modem_ctx[0x219]);
        if (r == 0) {
            g_modem_ctx[0x219]++;
            log_print_timestamp_prefix();
            g_log_func("Poweroff g350 ok\r\n");
            g_modem_ctx[0x218] = (g_modem_ctx[0x219] != 0) ? 2 : substate;
        } else if (r == 3) {
            log_print_timestamp_prefix();
            g_log_func("ERR Poweroff g350\r\n");
            g_modem_ctx[0x218]++;
        }
        break;
    }
    case 2:
        scheduler_release(&g_modem_pwroff[1]);   /* drop the AT response timer */
        scheduler_release(&g_modem_pwroff[0]);   /* drop the AT send timer     */
        if (g_modem_pwroff[3] == SCHED_SLOT_NONE) {
            uint8_t slot = scheduler_alloc();
            g_modem_pwroff[3] = slot;
            scheduler_start(slot, 2000, (sched_cb_t)0);             /* 2 s shutdown guard */
            scheduler_set_timer_name(g_modem_pwroff[3], 2000, "timeout_tmr");
        }
        vgsm = adc_read_vgsm();
        if (vgsm < 200) {                          /* rail collapsed -> modem off */
            scheduler_release(&g_modem_pwroff_guard);
            g_modem_ctx[0x218]++;
        }
        idle = scheduler_slot_is_idle(g_modem_pwroff[3]);
        if (idle == 0) {
            ret = 1;
        } else {                                   /* guard expired: give up waiting */
            log_print_timestamp_prefix();
            g_log_func("Vgsm %d\r\n", vgsm);
            scheduler_release(&g_modem_pwroff_guard);
            g_modem_ctx[0x218]++;
            ret = 1;
        }
        break;
    case 3:
        HAL_GPIO_WritePin((void *)GPIOB_BASE, MODEM_PWR_EN_PIN, 0);
        HAL_GPIO_WritePin((void *)GPIOA_BASE, MODEM_LVLSH_PIN, 0);
        HAL_GPIO_WritePin((void *)GPIOE_BASE, MODEM_RESET_PIN, 1);
        HAL_GPIO_WritePin((void *)GPIOB_BASE, MODEM_AUX_PIN, 0);
        ret = 0;
        g_modem_ctx[0x218] = 0;
        break;
    default:
        ret = 1;
        break;
    }
    return ret;
}

/* ================================================================== *
 *  SIM / info helpers
 * ================================================================== */

/* modem_sm_state_name (0x08033070): map a modem_sm_state_t to its log label. */
const char *modem_sm_state_name(uint32_t state)
{
    switch (state) {
    case MODEM_IDLE:           return "IDLE";
    case MODEM_POWERON:        return "POWERON";
    case MODEM_SMS_INIT:       return "SMS_INIT";
    case MODEM_SMS_READ:       return "SMS_READ";
    case MODEM_SMS_WRITE:      return "SMS_WRITE";
    case MODEM_CTX_ACTIVATE:   return "CTX_ACT";
    case MODEM_CTX_DEACTIVATE: return "CTX_DEACT";
    case MODEM_PING_SEND:      return "PING_SEND";
    case MODEM_MESSAGE_SEND:   return "MESSAGE_SEND";
    case MODEM_LOCATION_SEND:  return "LOCATION_SEND";
    case MODEM_POWEROFF:       return "POWEROFF";
    default:                   return "UNKNOWN";
    }
}

/* modem_registration_get (0x0803CDE0): log + return the cached network-
 * registration flag. */
extern uint8_t g_modem_registered;          /* OEM DAT_0803CDF8 */
uint8_t modem_registration_get(void)
{
    g_log_func("\r\n");                      /* OEM logs DAT_0803CDF4 */
    return g_modem_registered;
}

/* modem_info_ready (0x0802AAE8): poll the bus for "modem info valid" (the parsed
 * ICCID/IMSI/IMEI are available). Returns 0 when ready. */
extern int async_request_poll(unsigned int a, unsigned int tag,
                              unsigned int c, unsigned int d, unsigned int e); /* 0x0802A28C */
int modem_info_ready(void)
{
    return async_request_poll(0, 0x7400000u, 0, 0, 0);
}

/*
 * sim_iccid_check (0x0802E328): the SIM lock. At the POWEROFF recycle the
 * firmware prints the firmware version and the PDP counters, then — if the
 * modem info is ready — compares the SIM's ICCID against the VanMoof
 * Vodafone-NL batch prefix "8931440400" (10 digits). On mismatch it raises a
 * bus event and logs "Wrong iccid, <sim|no sim>" using the SIM-present pin
 * (PE10). Always logs the actual ICCID afterwards.
 */
void sim_iccid_check(void)
{
    static const char ICCID_VANMOOF_PREFIX[] = "8931440400";
    uint8_t *ctx = G_CTX_PTR;
    const char *iccid = (const char *)(ctx + 0x3E8 + 0x50);

    console_cmd_ver(0);
    g_log_func("PDOCP %d\r\n", *(uint16_t *)(ctx + 0x498));
    g_log_func("PDSCP %d\r\n", *(uint16_t *)(ctx + 0x49A));

    if (modem_info_ready() == 0 &&
        bounded_strncmp(iccid, ICCID_VANMOOF_PREFIX, 10) != 0) {
        async_request_post(0, 0x1000000);
        log_print_timestamp_prefix();
        if (HAL_GPIO_ReadPin((void *)GPIOE_BASE, MODEM_SIM_DET_PIN) != 0)
            g_log_func("Wrong iccid, %s\r\n", "sim");
        else
            g_log_func("Wrong iccid, %s\r\n", "no sim");
    }
    log_print_timestamp_prefix();
    g_log_func("iccid %s\r\n", iccid);
}
