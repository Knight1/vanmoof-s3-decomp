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
#include "flash.h"  /* config_persist_dual_bank + struct boot_cfg_block (SMS 'key' cmd) */
#include "log.h"
#include "scheduler.h"
#include "util.h"   /* ringbuf_t + ring primitives for the USART2 transport */

/* ------------------------------------------------------------------ *
 *  externs — HAL / libc / cross-module helpers (OEM addresses noted)
 * ------------------------------------------------------------------ */
extern void  HAL_GPIO_WritePin(void *GPIOx, uint16_t pin_mask, int state); /* 0x08026AC6 */
extern int   HAL_GPIO_ReadPin(void *GPIOx, uint16_t pin_mask);             /* 0x08026AB8 */
extern int   snprintf(char *s, unsigned int n, const char *fmt, ...);
extern void *memset(void *s, int c, unsigned int n);

/* The modem UART (the SARA AT channel) ring transport == USART2. The locked
 * RX getter and the string-flush (puts) are sourced at the bottom of this file.
 * modem_uart_tx_byte is a *different* function — despite the name it is a
 * software response-line accumulator (appends one received byte to the reply
 * buffer, tracking the expected line count), not a USART driver — also sourced
 * at the bottom. */
extern int   modem_uart_rx_byte(char *out);          /* 0x08036144: 1 = got a byte */
int          modem_uart_tx_byte(void *h, char b);    /* 0x08035C94: 0/1/2 status    */
extern void  modem_uart_flush(void *buf);            /* 0x08036130 (USART2 puts)    */
extern void  modem_at_response_copy(void *dst, int src); /* 0x0802F404              */

/* Bounded byte compare (OEM 0x0802181C — the GNU word-at-a-time strncmp).
 * Returns 0 when the two buffers are equal over `n` bytes. */
extern int   bounded_strncmp(const void *a, const void *b, unsigned int n);

/* libc string helpers used by the AT response/command callbacks. */
extern char *strstr(const char *hay, const char *needle);   /* 0x08021BDC (match ptr / NULL) */
extern char *strchr(const char *s, int c);                  /* 0x0802133C                    */
extern unsigned strlen(const char *s);                      /* 0x08021740                    */
extern int   parse_leading_decimal(const char *s);          /* 0x08020DDC (atoi of digits)   */

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

/* Parsed modem identity / SIM info — working buffers in SRAM, filled by the AT
 * response handlers (see "AT response/command callbacks" below). They sit in a
 * descending block in the AT-init order CGMI, CGMM, CGMR, CGSN, (CPIN), CIMI,
 * CCID, CSQ. g_modem_model_resp (0x20009CD0) is declared above. */
extern char     g_modem_manufacturer[16]; /* OEM 0x20009CC0 (AT+CGMI) */
extern char     g_modem_revision[16];     /* OEM 0x20009CE0 (AT+CGMR) */
extern char     g_modem_imei[16];         /* OEM 0x20009CF0 (AT+CGSN) */
extern char     g_modem_imsi[16];         /* OEM 0x20009D00 (AT+CIMI) */
extern char     g_modem_iccid[];          /* OEM 0x20009D10 (AT+CCID — the SIM-lock serial) */
extern char     g_modem_csq[3];           /* OEM 0x20009D25 (AT+CSQ RSSI text) */

/* SMS working state. The phone-number and body buffers are reused both ways:
 * recipient/body on send (CMGS), origin/body on read (CMGR). */
extern uint16_t g_modem_sms_index;        /* OEM 0x20009C0C (+CMGL index; CMGR/CMGD operand) */
extern uint8_t  g_modem_sms_read_count;   /* OEM 0x20009D28 (bounded CMGR read progress)     */
extern uint8_t  g_modem_sms_used;         /* OEM 0x20009D8C (+CPMS stored count; read cap)   */
extern char     g_modem_sms_number[];     /* OEM 0x20009D2C (recipient on CMGS / origin on CMGR) */
extern char     g_modem_sms_body[];       /* OEM 0x20009D3C (= g_modem_sms_number + 0x10)        */

/* HTTP request scratch — the %s body substituted into AT+UHTTPC. */
extern char     g_modem_http_payload[];   /* OEM 0x20009C20 */

/* Inbound-SMS remote-command interpreter ("#<code>*<cmd>" bodies → unlock /
 * factory-reset / state change / location report / bell). */
extern void     modem_sms_dispatch_command(char *out, unsigned size, char *body); /* 0x0803D668 */

/* Provisioning string table (flash): [0] = SIM PIN (""), [1] = APN
 * ("m2m.vanmoof.com"). Read indirectly by the CPIN / UPSD callbacks. */
extern const char *const g_modem_provision[]; /* OEM 0x0804F440 */

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
#define MODEM_PWR_EN_PIN   0x0004u   /* PB2  — main supply enable          */
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
                    /* echo the '>' prompt itself into the accumulator (it passes
                     * the printable filter); the OEM passes rx here, not 0. */
                    modem_uart_tx_byte((void *)uart_h, rx);
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
            scheduler_set_timer_name(g_modem_at_timer[0], 1, "interval_tmr");
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
    /* ctx+0x3E8 holds a POINTER to the modem identity block; the ICCID is at
     * +0x50 within it (OEM ldr [ctx,#0x3E8] then +0x50 — an inner deref). */
    const char *iccid = *(char **)(ctx + 0x3E8) + 0x50;

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

/* ================================================================== *
 *  AT response-parsing primitives
 * ================================================================== */

/* modem_skip_to_cr (0x0802F1DC): advance to the char just past the next '\r';
 * NULL if the string ends ('\0') first. */
char *modem_skip_to_cr(char *p)
{
    for (;;) {
        if (*p == '\r')
            return p + 1;
        if (*p == '\0')
            return (char *)0;
        p++;
    }
}

/* modem_skip_to_space (0x0802F1F0): advance to the char just past the next ' ';
 * NULL on '\0'. Used to step past the "+CMD:" tag to the value. */
char *modem_skip_to_space(char *p)
{
    for (;;) {
        if (*p == ' ')
            return p + 1;
        if (*p == '\0')
            return (char *)0;
        p++;
    }
}

/* modem_extract_field (0x0802F204): copy the next response field from `src`
 * into `dst[size]`. Skips a leading "+CMD " token and an opening quote, copies
 * up to the closing quote / comma / CR / NUL, then steps past a trailing comma.
 * dst == NULL → locate-only (no copy). Returns the cursor past the field (just
 * past a trailing comma if present), or NULL if `src` begins at CR/NUL. */
char *modem_extract_field(char *dst, unsigned size, char *src)
{
    char c = *src;

    if (c == '\r')
        return (char *)0;
    if (c == '\0')
        return (char *)0;

    if (c == '+') {                       /* skip the "+CMD" token to its space */
        while ((c = *src) != ' ' && c != '\r' && c != '\0')
            src++;
        if (c == ' ')
            src++;
    }

    if (*src == '\"')                     /* strip an opening quote */
        src++;

    if (dst == (char *)0) {
        while ((c = *src) != '\"' && c != ',' && c != '\r' && c != '\0')
            src++;
    } else {
        while (size > 1 && (c = *src) != '\"' && c != ',' && c != '\r' && c != '\0') {
            *dst++ = c;
            size--;
            src++;
        }
        *dst = '\0';
    }

    if (*src == '\"')                     /* strip a closing quote */
        src++;

    if (*src == ',')                      /* step past a field separator */
        return src + 1;
    return src;
}

/* modem_at_response_copy (0x0802F404): copy 0x80 bytes src→dst, translating CR
 * to space; the dst index is masked to 8 bits each step. modem_at_exec calls
 * this to normalize the raw reply before matching. */
void modem_at_response_copy(void *dst, int src)
{
    char *d = (char *)dst;
    const char *s = (const char *)src;
    unsigned di = 0;
    int si;

    for (si = 0; si < 0x80; si++) {
        d[di] = (s[si] == '\r') ? ' ' : s[si];
        di = (di + 1) & 0xff;
    }
}

/* ================================================================== *
 *  AT response/command callbacks
 *
 *  These are the .build_cb / .handle_cb function-pointer targets in the flash
 *  AT-script table (modem_at_entry_t). modem_at_exec invokes them with
 *  pkt = {buf-or-response, bufsize, fmt}:
 *    handle_cb: pkt[0] = received response text; return 0 done-OK, 2 alt-OK,
 *               3 fail/no-match.
 *    build_cb:  pkt[0]=txbuf, pkt[1]=bufsize, pkt[2]=fmt; snprintf the command;
 *               return 0 ok / 3 fail.
 * ================================================================== */

/* --- identity handlers: skip the echo + blank line, copy the value field --- */

/* modem_parse_manufacturer (0x0802F36C): AT+CGMI → g_modem_manufacturer. */
int modem_parse_manufacturer(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *q = modem_skip_to_cr(modem_skip_to_cr(resp));
    return modem_extract_field(g_modem_manufacturer, 0x10, q) ? 0 : 3;
}

/* modem_parse_model (0x0802F338): AT+CGMM → g_modem_model_resp (matched vs
 * "SARA"/"LARA" by modem_step_poweron). */
int modem_parse_model(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *q = modem_skip_to_cr(modem_skip_to_cr(resp));
    return modem_extract_field(g_modem_model_resp, 0x10, q) ? 0 : 3;
}

/* modem_parse_revision (0x0802F304): AT+CGMR → g_modem_revision. */
int modem_parse_revision(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *q = modem_skip_to_cr(modem_skip_to_cr(resp));
    return modem_extract_field(g_modem_revision, 0x10, q) ? 0 : 3;
}

/* modem_parse_imei (0x0802F2D0): AT+CGSN → g_modem_imei. */
int modem_parse_imei(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *q = modem_skip_to_cr(modem_skip_to_cr(resp));
    return modem_extract_field(g_modem_imei, 0x10, q) ? 0 : 3;
}

/* modem_parse_imsi (0x0802F29C): AT+CIMI → g_modem_imsi. */
int modem_parse_imsi(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *q = modem_skip_to_cr(modem_skip_to_cr(resp));
    return modem_extract_field(g_modem_imsi, 0x10, q) ? 0 : 3;
}

/* --- status handlers: locate the URC tag, classify/extract the value --- */

/* modem_handle_creg (0x0802F434): AT+CREG? — accept registered/connected
 * classes (leading digit ∈ {1,5,6,7,9}). */
int modem_handle_creg(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *field;

    if (strstr(resp, "+CREG:") == (char *)0)
        return 3;
    field = modem_extract_field((char *)0, 0, modem_skip_to_space(resp));
    switch (field[0]) {
    case '1': case '5': case '6': case '7': case '9':
        return 0;
    default:
        return 3;
    }
}

/* modem_handle_ccid (0x0802F494): AT+CCID → g_modem_iccid (the SIM serial the
 * SIM lock checks). */
int modem_handle_ccid(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];

    if (strstr(resp, "+CCID:") == (char *)0)
        return 3;
    if (modem_extract_field(g_modem_iccid, 0x15, modem_skip_to_space(resp)) == (char *)0)
        return 3;
    return 0;
}

/* modem_handle_cpin (0x0802F4D4): AT+CPIN? — "READY" → 2 (no PIN needed),
 * "SIM PIN" → 0 (enter PIN), else 3. */
int modem_handle_cpin(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *field;

    if (strstr(resp, "+CPIN:") == (char *)0)
        return 3;
    field = modem_skip_to_space(resp);
    if (bounded_strncmp(field, "READY", 5) == 0)
        return 2;
    if (bounded_strncmp(field, "SIM PIN", 7) == 0)
        return 0;
    return 3;
}

/* modem_handle_cmgf (0x0802F524): AT+CMGF? — '0' PDU → 0, '1' text → 2. */
int modem_handle_cmgf(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *field;

    if (strstr(resp, "+CMGF:") == (char *)0)
        return 3;
    field = modem_skip_to_space(resp);
    if (field[0] == '0')
        return 0;
    if (field[0] == '1')
        return 2;
    return 3;
}

/* modem_handle_upsnd (0x0802F560): AT+UPSND=0,8 — second CSV field is the PSD
 * activation flag ('0' → 0, '1' → 2). */
int modem_handle_upsnd(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *p, *field;

    if (strstr(resp, "+UPSND:") == (char *)0)
        return 3;
    p     = modem_extract_field((char *)0, 0, modem_skip_to_space(resp));
    field = modem_extract_field((char *)0, 0, p);
    if (field[0] == '0')
        return 0;
    if (field[0] == '1')
        return 2;
    return 3;
}

/* modem_handle_uuhttpcr (0x0802F5B0): +UUHTTPCR URC — accept HTTP result code
 * digit '0' or '1' (third CSV field). */
int modem_handle_uuhttpcr(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *cur;

    cur = strstr(resp, "+UUHTTPCR:");
    if (cur == (char *)0)
        return 3;
    cur = modem_skip_to_space(cur);
    cur = modem_extract_field((char *)0, 0, cur);
    cur = modem_extract_field((char *)0, 0, cur);
    return ((uint8_t)(*cur - '0') < 2) ? 0 : 3;
}

/* modem_handle_csq (0x0802F5FC): AT+CSQ → g_modem_csq; accept RSSI < 99 (99 =
 * "not detectable"). */
int modem_handle_csq(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *cur;

    cur = strstr(resp, "+CSQ:");
    if (cur == (char *)0)
        return 3;
    cur = modem_skip_to_space(cur);
    modem_extract_field(g_modem_csq, 3, cur);
    return (parse_leading_decimal(g_modem_csq) < 99) ? 0 : 3;
}

/* modem_handle_cpms (0x0802F644): AT+CPMS? → stored-SMS count into
 * g_modem_sms_used; always OK once the tag is seen. */
int modem_handle_cpms(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *cur;

    cur = strstr(resp, "+CPMS:");
    if (cur == (char *)0)
        return 3;
    cur = modem_extract_field((char *)0, 0, modem_skip_to_space(resp));
    g_modem_sms_used = (uint8_t)parse_leading_decimal(cur);
    return 0;
}

/* modem_handle_cmgl (0x0802F684): AT+CMGL → SMS index into g_modem_sms_index;
 * accept index in 1..0x130. */
int modem_handle_cmgl(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *cur;
    char field[4];
    uint16_t idx;

    cur = strstr(resp, "+CMGL:");
    if (cur == (char *)0)
        return 3;
    modem_extract_field(field, 4, modem_skip_to_space(cur));
    idx = (uint16_t)parse_leading_decimal(field);
    g_modem_sms_index = idx;
    return ((uint16_t)(idx - 1) < 0x131) ? 0 : 3;
}

/* modem_handle_cmgr_sms (0x0802FC30): AT+CMGR — store the originating address
 * into g_modem_sms_number, then hand the body (g_modem_sms_body) to the inbound
 * remote-command interpreter. Cursors are threaded from the strstr match and
 * the extract_field returns (NOT the response start), as the OEM does. */
int modem_handle_cmgr_sms(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *m, *cursor;

    m = strstr(resp, "+CMGR:");
    if (m == (char *)0)
        return 3;
    cursor = modem_extract_field((char *)0, 0, modem_skip_to_space(m));
    cursor = modem_extract_field(g_modem_sms_number, 0x10, cursor);
    modem_sms_dispatch_command(g_modem_sms_body, 0x50, modem_skip_to_cr(cursor));
    return 0;
}

/* modem_handle_ugsrv (0x0802FC84): +UGSRV AssistNow server config — 2 = both
 * hosts + token already match the provisioned values, 0 = a field differs
 * (reconfigure), 3 = tag absent. */
int modem_handle_ugsrv(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *q;

    q = strstr(resp, "+UGSRV:");
    if (q == (char *)0)
        return 3;
    q = strchr(q, '"');
    if (bounded_strncmp(q + 1, "ublox1.vanmoof.com", 0x12) != 0)
        return 0;
    q = strchr(q + 1, ',');
    q = strchr(q + 1, '"');
    if (bounded_strncmp(q + 1, "ublox1.vanmoof.com", 0x12) != 0)
        return 0;
    q = strchr(q + 1, ',');
    q = strchr(q + 1, '"');
    if (bounded_strncmp(q + 1, "PBNjh0V46Eev8CcfS4LPJg", 0x16) == 0)
        return 2;
    return 0;
}

/* modem_handle_ugaop (0x0802FD0C): +UGAOP AssistNow aiding config — 2 = host
 * == ublox1.vanmoof.com AND next field == "46434", else 0, 3 = tag absent. */
int modem_handle_ugaop(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *q;

    q = strstr(resp, "+UGAOP:");
    if (q == (char *)0)
        return 3;
    q = strchr(q, '"');
    if (bounded_strncmp(q + 1, "ublox1.vanmoof.com", 0x12) != 0)
        return 0;
    q = strchr(q + 1, ',');
    if (bounded_strncmp(q + 1, "46434", 5) == 0)
        return 2;
    return 0;
}

/* modem_handle_upsd (0x0802FD68): +UPSD PSD profile — 2 = the quoted APN field
 * matches the provisioned APN (g_modem_provision[1]), else 0, 3 = tag absent.
 * The cursor is threaded from the strstr match, as the OEM does. */
int modem_handle_upsd(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *m, *q;
    const char *apn;

    m = strstr(resp, "+UPSD:");
    if (m == (char *)0)
        return 3;
    q = modem_extract_field((char *)0, 0, modem_skip_to_space(m));
    q = strchr(q, '"');
    apn = g_modem_provision[1];
    return (bounded_strncmp(q + 1, apn, strlen(apn)) == 0) ? 2 : 0;
}

/* --- command builders: snprintf the outgoing AT command --- */

/* modem_build_cpin (0x0802F6D8): AT+CPIN="%s" with the provisioned SIM PIN. */
int modem_build_cpin(void *pkt)
{
    void **p = (void **)pkt;
    int n = snprintf((char *)p[0], (unsigned)(uintptr_t)p[1],
                     (const char *)p[2], g_modem_provision[0]);
    if (n < 1)
        return 3;
    return ((unsigned)n < (unsigned)(uintptr_t)p[1]) ? 0 : 3;
}

/* modem_build_cmgd (0x0802F710): AT+CMGD=%d — delete the current SMS slot. */
int modem_build_cmgd(void *pkt)
{
    void **p = (void **)pkt;
    int n = snprintf((char *)p[0], (unsigned)(uintptr_t)p[1],
                     (const char *)p[2], (int)g_modem_sms_index);
    if (n < 1)
        return 3;
    return ((unsigned)n < (unsigned)(uintptr_t)p[1]) ? 0 : 3;
}

/* modem_build_cmgr (0x0802F748): AT+CMGR=%d — read the current SMS slot;
 * advances the bounded read counter toward its limit first. */
int modem_build_cmgr(void *pkt)
{
    void **p = (void **)pkt;
    int n;

    if (g_modem_sms_read_count < g_modem_sms_used)
        g_modem_sms_read_count++;
    n = snprintf((char *)p[0], (unsigned)(uintptr_t)p[1],
                 (const char *)p[2], (int)g_modem_sms_index);
    if (n < 1)
        return 3;
    return ((unsigned)n < (unsigned)(uintptr_t)p[1]) ? 0 : 3;
}

/* modem_build_sms_body (0x0802F798): "%s\x1a" — the SMS text + Ctrl-Z that
 * terminates the message after the ">" prompt. */
int modem_build_sms_body(void *pkt)
{
    void **p = (void **)pkt;
    int n = snprintf((char *)p[0], (unsigned)(uintptr_t)p[1],
                     (const char *)p[2], g_modem_sms_body);
    if (n < 1)
        return 3;
    return ((unsigned)n < (unsigned)(uintptr_t)p[1]) ? 0 : 3;
}

/* modem_build_cmgs (0x0802F7CC): AT+CMGS="%s" — start an SMS to the recipient
 * number (the entry's expect string is the ">" body prompt). */
int modem_build_cmgs(void *pkt)
{
    void **p = (void **)pkt;
    int n = snprintf((char *)p[0], (unsigned)(uintptr_t)p[1],
                     (const char *)p[2], g_modem_sms_number);
    if (n < 1)
        return 3;
    return ((unsigned)n < (unsigned)(uintptr_t)p[1]) ? 0 : 3;
}

/* modem_build_upsd_apn (0x0802F800): AT+UPSD=0,1,"%s" — set the PDP APN. */
int modem_build_upsd_apn(void *pkt)
{
    void **p = (void **)pkt;
    int n = snprintf((char *)p[0], (unsigned)(uintptr_t)p[1],
                     (const char *)p[2], g_modem_provision[1]);
    if (n < 1)
        return 3;
    return ((unsigned)n < (unsigned)(uintptr_t)p[1]) ? 0 : 3;
}

/* modem_build_uhttpc_payload (0x0802F838): AT+UHTTPC POST/GET whose %s is the
 * pre-built request body g_modem_http_payload. */
int modem_build_uhttpc_payload(void *pkt)
{
    void **p = (void **)pkt;
    int n = snprintf((char *)p[0], (unsigned)(uintptr_t)p[1],
                     (const char *)p[2], g_modem_http_payload);
    if (n < 1)
        return 3;
    return ((unsigned)n < (unsigned)(uintptr_t)p[1]) ? 0 : 3;
}

/* modem_build_uhttp_port (0x0802F86C): AT+UHTTP=0,5,%s — HTTPS server port. */
int modem_build_uhttp_port(void *pkt)
{
    void **p = (void **)pkt;
    int n = snprintf((char *)p[0], (unsigned)(uintptr_t)p[1],
                     (const char *)p[2], "443");
    if (n < 1)
        return 3;
    return ((unsigned)n < (unsigned)(uintptr_t)p[1]) ? 0 : 3;
}

/* modem_handle_uuloc (0x0802F8D4): +UULOC URC — build the location report body
 * "imei=<IMEI>&rmc=$<+UULOC field>" into g_modem_http_payload. Returns 3 if the
 * tag is absent or the assembled length is out of 1..0x7f. */
int modem_handle_uuloc(void *pkt)
{
    char *resp = (char *)((void **)pkt)[0];
    char *field;
    int len;

    field = strstr(resp, "+UULOC:");
    if (field == (char *)0)
        return 3;
    len = snprintf(g_modem_http_payload, 0x80, "imei=%s&rmc=$", g_modem_imei);
    if ((unsigned)(len - 1) >= 0x7f)
        return 3;
    for (;;) {
        char c = *(++field);
        if (c == '\r' || c == '\0')
            break;
        g_modem_http_payload[len++] = c;
    }
    g_modem_http_payload[len] = '\0';
    return ((unsigned)(len - 1) < 0x7f) ? 0 : 3;
}

/* modem_build_ugaop (0x0802F980): AT+UGAOP="%s",%s,1000,0 — AssistNow aiding
 * server (host "ublox1.vanmoof.com", port "46434"). */
int modem_build_ugaop(void *pkt)
{
    void **p = (void **)pkt;
    int n = snprintf((char *)p[0], (unsigned)(uintptr_t)p[1],
                     (const char *)p[2], "ublox1.vanmoof.com", "46434");
    if (n < 1)
        return 3;
    return ((unsigned)n < (unsigned)(uintptr_t)p[1]) ? 0 : 3;
}

/* ------------------------------------------------------------------ *
 *  USART2 — the GSM/SARA AT-channel byte transport
 *
 *  The modem hangs off USART2. Its register-block handle is reached through a
 *  pointer-to-pointer at 0x200099A4 (set by usart2_init); the TX ring handle is
 *  at ctx+0x410 and the RX ring at +0x414 (the ctx block at 0x2000094C is shared
 *  with USART3 — the offsets differ). The locked TX/RX primitives mask the
 *  relevant CR1 interrupt-enable bit (TXEIE 0x80 / RXNEIE 0x20) behind a DSB/ISB
 *  before touching the ring, then re-enable it. ABI quirk preserved: the ring
 *  status that ringbuf_push_byte / ringbuf_get_byte leave in r0 survives the
 *  trailing re-enable and is the implicit return value (1 = byte moved).
 * ------------------------------------------------------------------ */

#define GSM_HANDLE   (*(volatile uint32_t * volatile *)0x200099A4u)
#define GSM_CTX      0x2000094Cu
#define GSM_TX_RING  (*(ringbuf_t * volatile *)(GSM_CTX + 0x410u))
#define GSM_RX_RING  (*(ringbuf_t * volatile *)(GSM_CTX + 0x414u))

/* Clear a latched USART error flag the RM0430 way: if `flag` (PE 0x1 / FE 0x2 /
 * NE 0x4 / ORE 0x8) is set in SR, read SR then DR — that read sequence clears the
 * sticky error bit (and discards the offending byte). Reading DR is what actually
 * de-asserts ORE, so dropping it would wedge RX on an overrun. */
static void usart_clear_error_flag(volatile uint32_t *d, uint32_t flag)
{
    volatile uint32_t tmp;
    if ((d[0] & flag) != 0) {
        tmp = 0;
        tmp = d[0];   /* SR */
        tmp = d[1];   /* DR */
        (void)tmp;
    }
}

/* Locked single-byte TX into the USART2 TX ring (OEM 0x080360F8). */
int modem_uart_putc(uint8_t b)
{
    volatile uint32_t *dev = GSM_HANDLE;

    dev[0xC / 4] &= ~0x80u;                        /* mask TXEIE */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    uint32_t rc = ringbuf_push_byte(GSM_TX_RING, b);

    dev = GSM_HANDLE;                              /* OEM re-loads the handle */
    dev[0xC / 4] |= 0x80u;                         /* re-enable TXEIE */
    return (int)rc;
}

/* Transmit a NUL-terminated string over USART2 (OEM modem_uart_flush, 0x08036130). */
void modem_uart_flush(void *buf)
{
    const char *s = (const char *)buf;

    for (; *s != '\0'; s++) {
        modem_uart_putc((uint8_t)*s);
    }
}

/* Locked single-byte RX from the USART2 RX ring (OEM 0x08036144).
 * Returns 1 when a byte was dequeued into *out, 0 when the ring was empty. */
int modem_uart_rx_byte(char *out)
{
    volatile uint32_t *dev = GSM_HANDLE;

    dev[0xC / 4] &= ~0x20u;                        /* mask RXNEIE */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    uint32_t rc = ringbuf_get_byte(GSM_RX_RING, (uint8_t *)out);

    dev = GSM_HANDLE;                              /* OEM re-loads the handle */
    dev[0xC / 4] |= 0x20u;                         /* re-enable RXNEIE */
    return (int)rc;
}

/* Response-line accumulator (OEM 0x08035C94). `h` points to a 4-word state
 * record {char *buf; int cap; int count; int lines_remaining}; appends one
 * received byte `b`. Printable bytes (0x20..0x7E) are stored; a CR is stored and
 * decrements lines_remaining; any other byte is ignored. Returns 2 if the buffer
 * was already full (resets count), 0 once the last expected line's CR arrives
 * (NUL-terminates), else 1. modem_at_exec feeds every RX byte through this. */
int modem_uart_tx_byte(void *h, char b)
{
    int *st = (int *)h;
    uint32_t count = (uint32_t)st[2];

    if ((uint32_t)st[1] <= count) {
        st[2] = 0;
        return 2;
    }
    if ((uint32_t)(((uint8_t)b - 0x20) & 0xff) < 0x5f) {
        st[2] = (int)(count + 1);
        ((char *)(uintptr_t)st[0])[count] = b;
        return 1;
    }
    if (b != '\r') {
        return 1;
    }
    st[2] = (int)(count + 1);
    ((char *)(uintptr_t)st[0])[count] = '\r';
    {
        int rem = st[3];
        st[3] = rem - 1;
        if (rem - 1 == 0) {
            ((char *)(uintptr_t)st[0])[st[2]] = '\0';
            return 0;
        }
    }
    return 1;
}

/* USART2 RX/TX byte-pump ISR (OEM 0x0803617C), invoked via a thin vector
 * trampoline. On RXNE with no parity/framing/noise/overrun error (SR bits 0-3
 * clear) and RXNEIE set, push DR into the RX ring; then clear any latched
 * PE/FE/NE/ORE error (read SR+DR); on TXE with TXEIE set, pop the next TX byte
 * and write it to DR, disabling TXEIE when the ring drains. The RX/TX gates use
 * SR/CR1 sampled once; the error-clear block and TX writes re-load the handle. */
void usart2_irq_handler(void)
{
    volatile uint32_t *dev = GSM_HANDLE;
    uint32_t sr  = dev[0];
    uint32_t cr1 = dev[0xC / 4];

    if ((sr & 0xFu) == 0 && (sr & 0x20u) != 0 && (cr1 & 0x20u) != 0) {
        ringbuf_push_byte(GSM_RX_RING, (uint8_t)dev[1]);
    }

    volatile uint32_t *d = GSM_HANDLE;
    usart_clear_error_flag(d, 0x1u);
    usart_clear_error_flag(d, 0x2u);
    usart_clear_error_flag(d, 0x4u);
    usart_clear_error_flag(d, 0x8u);

    if ((sr & 0x80u) != 0 && (cr1 & 0x80u) != 0) {
        uint8_t b;
        if (ringbuf_get_byte(GSM_TX_RING, &b) == 0) {
            GSM_HANDLE[0xC / 4] &= ~0x80u;
        } else {
            GSM_HANDLE[1] = b;
        }
    }
}

/* SMS info-tracking init latch (OEM 0x0803D648 / 0x0803D65C). A one-shot flag at
 * SRAM 0x2000839C guards the SMS info-tracking state machine: latch_once returns
 * 0 the first time (arming the flag) and 1 thereafter; _get reads it back. */
#define SMS_TRACK_FLAG  (*(volatile uint8_t *)0x2000839Cu)

int sms_tracking_latch_once(void)
{
    if (SMS_TRACK_FLAG == 0) {
        SMS_TRACK_FLAG = 1;
        return 0;
    }
    return 1;
}

uint8_t sms_tracking_get(void)
{
    return SMS_TRACK_FLAG;
}

/* ── inbound-SMS remote-command interpreter (the SMS half of the anti-theft
 *    surface; the BLE half is ble_cmd_dispatch) ─────────────────────────────── */

extern int  save_state_record_to_eeprom();                  /* 0x0803E2CC (K&R: 4 reg + 11-word tail) */
extern void maybe_set_state_if_unlocked(char state);        /* 0x08029B88 — set bike state when unlocked */
extern void app_ctx_clear_field_328(void);                  /* 0x0803DBB8 */
extern void amp_volume_brownout_apply(unsigned char *vol);  /* audio-amp volume apply */
extern unsigned char ssp_ble_enqueue_tx_packet(unsigned int id, unsigned int kind,
                                               unsigned char *buf, unsigned char last);

/* The modem/SMS dispatch state struct (OEM SRAM 0x2000839C): byte[0] is the
 * sms-tracking one-shot latch (see sms_tracking_*), +4 caches the session-context
 * pointer, +8 is a flags byte (bit0 = ping reported, bit2 = tracking armed). */
#define SMS_STATE_BASE   0x2000839Cu
#define SMS_STATE_CTX    (*(uint8_t * volatile *)(SMS_STATE_BASE + 4))
#define SMS_STATE_FLAGS  (*(volatile uint8_t *)(SMS_STATE_BASE + 8))

/* modem_sms_dispatch_command (OEM 0x0803D668). Parse "#<8-char code>*<command>"
 * from an inbound SMS body and act on it: `key` (clear the owner backup code +
 * persist config), `TrackingOn`/`TrackingOff` (arm/disarm GPS tracking + persist
 * the state record), `ping` (build a JSON status report into the HTTP payload),
 * `makeNoise` (ring the bell). On any matched command an "#<code>*ack#" reply is
 * written to `out`. A body that doesn't start with '#' yields an empty reply. */
void modem_sms_dispatch_command(char *out, unsigned int size, char *body)
{
    char code[12];   /* the 8-char authentication code + NUL */

    if (body[0] != '#') {
        *out = '\0';
        return;
    }

    for (unsigned int i = 0; i < 8; i++) {   /* code = body[1..8] */
        code[i] = body[i + 1];
    }
    code[8] = '\0';

    if (body[9] != '*') {
        return;
    }

    char    *cmd = body + 10;
    uint8_t *ctx = SMS_STATE_CTX;

    if (bounded_strncmp(cmd, "key", 3) == 0) {
        *(uint16_t *)(ctx + 0x100) = 0xFF;                 /* backup code -> "not set" */
        g_log_func("GSM rem bu\r\n");
        config_persist_dual_bank(*(uint32_t *)(ctx + 0xF4), *(uint32_t *)(ctx + 0xF8),
                                 *(uint32_t *)(ctx + 0xFC), *(uint32_t *)(ctx + 0x100),
                                 *(struct boot_cfg_block *)(ctx + 0x104));
        snprintf(out, size, "#%s*ack#", code);
    } else if (bounded_strncmp(cmd, "TrackingOn", 10) == 0) {
        if (HAL_GPIO_ReadPin((void *)0x40020800u, 4) == 0) {   /* GPIOB pin 2 */
            log_print_timestamp_prefix();
            g_log_func("SMS received tracking on\r\n");
            ctx[0x310] = 4;
            if (save_state_record_to_eeprom(
                    *(uint32_t *)(ctx + 0x310), *(uint32_t *)(ctx + 0x314),
                    *(uint32_t *)(ctx + 0x318), *(uint32_t *)(ctx + 0x31C),
                    *(uint32_t *)(ctx + 0x320), *(uint32_t *)(ctx + 0x324),
                    *(uint32_t *)(ctx + 0x328), *(uint32_t *)(ctx + 0x32C),
                    *(uint32_t *)(ctx + 0x330), *(uint32_t *)(ctx + 0x334),
                    *(uint32_t *)(ctx + 0x338), *(uint32_t *)(ctx + 0x33C),
                    *(uint32_t *)(ctx + 0x340), *(uint32_t *)(ctx + 0x344),
                    *(uint32_t *)(ctx + 0x348)) != 0) {
                g_log_func(" ERROR Save values\r\n");
            }
            maybe_set_state_if_unlocked(4);
            app_ctx_clear_field_328();
            SMS_STATE_FLAGS |= 4;
        } else {
            log_print_timestamp_prefix();
            g_log_func("Ignore SMS received tracking on\r\n");
        }
        snprintf(out, size, "#%s*ack#", code);
    } else if (bounded_strncmp(cmd, "TrackingOff", 0xB) == 0) {
        if (ctx[0x310] != 0x0B) {
            ctx[0x310] = 0x0B;
            if (save_state_record_to_eeprom(
                    *(uint32_t *)(ctx + 0x310), *(uint32_t *)(ctx + 0x314),
                    *(uint32_t *)(ctx + 0x318), *(uint32_t *)(ctx + 0x31C),
                    *(uint32_t *)(ctx + 0x320), *(uint32_t *)(ctx + 0x324),
                    *(uint32_t *)(ctx + 0x328), *(uint32_t *)(ctx + 0x32C),
                    *(uint32_t *)(ctx + 0x330), *(uint32_t *)(ctx + 0x334),
                    *(uint32_t *)(ctx + 0x338), *(uint32_t *)(ctx + 0x33C),
                    *(uint32_t *)(ctx + 0x340), *(uint32_t *)(ctx + 0x344),
                    *(uint32_t *)(ctx + 0x348)) != 0) {
                g_log_func(" ERROR Save values\r\n");
            }
        }
        maybe_set_state_if_unlocked(0x0E);
        log_print_timestamp_prefix();
        g_log_func("SMS received tracking off\r\n");
        SMS_STATE_FLAGS &= 0xFB;
        snprintf(out, size, "#%s*ack#", code);
    } else if (bounded_strncmp(cmd, "ping", 4) == 0) {
        char    *guid = cmd + 4;                            /* body + 14 */
        char     guid_buf[36];
        for (unsigned int i = 0; i < 0x20; i++) {
            guid_buf[i] = guid[i];
        }
        guid_buf[0x20] = '\0';

        /* MAC string (6 bytes at ctx+0x390) -> scratch @ 0x20009CAC */
        snprintf((char *)0x20009CACu, 0x12, "%02X:%02X:%02X:%02X:%02X:%02X",
                 ctx[0x390], ctx[0x391], ctx[0x392], ctx[0x393], ctx[0x394], ctx[0x395]);

        /* firmware version word @ flash 0x08020004 -> "maj.min.pat" @ 0x20009C10 */
        uint32_t ver = *(volatile uint32_t *)0x08020004u;
        snprintf((char *)0x20009C10u, 0xF, "%d.%02d.%02d",
                 (int)(ver >> 0x18), (int)((ver >> 0x10) & 0xFF), (int)((ver >> 8) & 0xFF));

        int16_t batt_raw = *(int16_t *)(ctx + 0x3D2);
        int32_t dist_raw = *(int32_t *)(ctx + 0x31C);
        /* JSON status report -> g_modem_http_payload @ 0x20009C20 (signed /10 on
         * batt and dist, exactly the OEM 0x66666667 magic-multiply). */
        snprintf((char *)0x20009C20u, 0x8A,
                 "'guid':'%s','statistics':{'batt':%d,'mac':'%s','swv':'%s','dist':%d}",
                 guid_buf, (int)(int16_t)(batt_raw / 10),
                 (char *)0x20009CACu, (char *)0x20009C10u, dist_raw / 10);

        SMS_STATE_FLAGS |= 1;
        snprintf(out, size, "#%s*ack#", code);
    } else if (bounded_strncmp(cmd, "makeNoise", 9) == 0) {
        HAL_GPIO_WritePin((void *)0x40020C00u, 0x20, 0);    /* GPIOD pin 5 low */
        unsigned char pkt[5];
        pkt[0] = '&';                                       /* amp volume 0x26 */
        amp_volume_brownout_apply(pkt);
        pkt[1] = 0x1A;
        pkt[2] = 0x28;
        if (ssp_ble_enqueue_tx_packet(200, 2, pkt + 1, 0) > 0x80) {
            g_log_func("  ERROR SSPB place\r\n");
        }
        snprintf(out, size, "#%s*ack#", code);
    }
}
