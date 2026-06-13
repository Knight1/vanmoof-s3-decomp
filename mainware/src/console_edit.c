#include <stdint.h>
#include <string.h>

#include "console_edit.h"
#include "scheduler.h"

/* ES3 console command engine + VT100 key decoder. See console_edit.h. */

/* The 49-entry command table in flash: {name, help, handler}, 0xC bytes each.
 * Same table the console.c command handlers belong to (docs/console.md). */
typedef void (*console_handler_t)();
typedef struct {
    const char       *name;
    const char       *help;
    console_handler_t handler;
} console_cmd_t;

#define CONSOLE_CMD_TABLE  ((const console_cmd_t *)0x0804F5C4u)
#define CONSOLE_CMD_COUNT  0x31u                       /* 49 commands */
#define CONSOLE_HIST_BUF   0x20009368u                 /* base of the 9 line buffers */

/* The active-console I/O dispatch table (g_log_func, 0x20009D98): the same
 * 5-slot vector the rest of the firmware prints through. The decoder reads keys
 * via slot [4] (rx_byte). */
typedef struct {
    int  (*printf)(const char *fmt, ...);
    int  (*tx_byte)(uint8_t b);
    void (*puts)(const char *s);
    void (*write)(const uint8_t *buf, uint16_t len);
    int  (*rx_byte)(uint8_t *out);
} console_io_t;
#define g_console_io  ((console_io_t *)0x20009D98u)

/* Escape-decoder state in the session context (0x20009368 + 0x368/0x369) and the
 * scheduler slot id that times the bare-ESC vs CSI disambiguation. */
#define CONSOLE_ESC_STATE  (*(volatile uint8_t *)0x200096D0u)   /* +0x368 */
#define CONSOLE_ESC_PARAM  (*(volatile uint8_t *)0x200096D1u)   /* +0x369 */
#define CONSOLE_ESC_TIMER  (*(volatile uint8_t *)0x20000115u)

extern int bounded_strncmp(const char *a, const char *b, uint32_t n);  /* 0x0802181C: 0 == equal */

/* Does `input` begin with the command `name` followed by end-of-string or a
 * whitespace delimiter? OEM 0x08040904. */
int console_cmd_match(const char *name, const char *input)
{
    char c;

    for (;;) {
        c = *input;
        if (*name != c) {
            break;
        }
        if (*name == '\0') {
            return 1;
        }
        name++;
        input++;
    }
    if (c == ' ' || c == '\f' || c == '\n' || c == '\r' || c == '\t' || c == '\v') {
        return 1;
    }
    return 0;
}

/* Seed the 9-slot history ring: slot i ages to i, points at line buffer i, the
 * newest (age 8) becomes the active edit slot. OEM 0x0804094C. */
void console_history_init(console_edit_t *e)
{
    uint32_t i;

    for (i = 0; i < 9; i++) {
        e->slots[i].age = (uint8_t)i;
        e->slots[i].buf = (char *)(CONSOLE_HIST_BUF + i * 0x51u);
        e->slots[i].len = 0;
        if (i == 8) {
            e->cur = &e->slots[i];
            e->slots[i].len = 0;
        }
    }
    e->nav = 0;
    e->ac_match = 0;
    e->ac_len = 0;
    e->ac_cursor = 0;
}

/* Age every slot by one on Enter; the slot that wraps to age 8 becomes the new
 * (empty) active edit slot. OEM 0x080409A0. */
void console_history_rotate(console_edit_t *e)
{
    uint32_t i;

    for (i = 0; i < 9; i++) {
        uint8_t a = (uint8_t)(e->slots[i].age + 1);
        e->slots[i].age = a;
        if (a > 8) {
            e->slots[i].age = 0;
        }
        if (e->slots[i].age == '\b') {        /* == 8 (newest) */
            e->cur = &e->slots[i];
            e->slots[i].len = 0;
        }
    }
    e->nav = 0;
}

/* Pre-login dispatcher: find the "login" command and run it with the typed line
 * (every line before authentication is a login attempt). OEM 0x08040A00. */
int console_dispatch_login(console_edit_t *e)
{
    uint32_t i = 0;

    while (i < CONSOLE_CMD_COUNT &&
           console_cmd_match(CONSOLE_CMD_TABLE[i].name, "login") == 0) {
        i = (i + 1) & 0xff;
    }
    if (i < CONSOLE_CMD_COUNT) {
        console_handler_t h = CONSOLE_CMD_TABLE[i].handler;
        if (h != 0) {
            h(e->cur->buf);
        }
        return 1;
    }
    return 0;
}

/* Copy the history slot whose age == nav into the active line. OEM 0x08041184. */
void console_history_recall(console_edit_t *e)
{
    uint32_t i = 0;

    while (i <= 8) {
        if (e->slots[i].age == e->nav) {
            break;
        }
        i = (i + 1) & 0xff;
    }
    if (i > 8) {
        return;
    }
    memcpy(e->cur->buf, e->slots[i].buf, e->slots[i].len);
    e->cur->len = e->slots[i].len;
}

/* History up (older). OEM 0x080411CA. */
void console_history_prev(console_edit_t *e)
{
    console_history_recall(e);
    if (e->cur->len == 0) {
        e->nav--;
        console_history_recall(e);
    }
    if (e->nav < 7) {
        e->nav++;
    }
}

/* History down (newer). OEM 0x080411FA. */
void console_history_next(console_edit_t *e)
{
    if (e->cur->len != 0) {
        if (e->nav != 0) {
            e->nav--;
        }
        console_history_recall(e);
        if (e->cur->len == 0) {
            e->nav++;
            console_history_recall(e);
        }
    }
}

/* Commit a staged tab-completion into the active line. OEM 0x08041232. */
void console_autocomplete_apply(console_edit_t *e)
{
    if (e->ac_match != 0) {
        memcpy(e->cur->buf, e->ac_match, e->ac_len);
        e->cur->len = e->ac_len;
        e->ac_match = 0;
    }
    e->ac_cursor = 0;
}

/* Scan the command table from the saved cursor for a name whose prefix matches
 * the typed line; stage the first hit. OEM 0x0804125C. */
int console_autocomplete_search(console_edit_t *e)
{
    uint8_t k;

    e->ac_match = 0;
    for (k = 0; e->ac_match == 0 && k < CONSOLE_CMD_COUNT; k++) {
        uint8_t idx = e->ac_cursor;
        const char *s = CONSOLE_CMD_TABLE[idx].name;
        uint32_t next;

        if (bounded_strncmp(s, e->cur->buf, e->cur->len) == 0) {
            e->ac_match = s;
            e->ac_len = (uint8_t)strlen(s);
        }
        next = (uint32_t)(idx + 1);
        e->ac_cursor = (uint8_t)next;
        if ((next & 0xff) > 0x30) {
            e->ac_cursor = 0;
        }
    }
    return e->ac_match != 0;
}

/* Logged-in dispatcher: match the typed line against a command name, split args
 * at the first space, invoke the handler. OEM 0x080426BC. */
int console_dispatch_command(console_edit_t *e)
{
    uint32_t i = 0;

    while (i < CONSOLE_CMD_COUNT &&
           console_cmd_match(CONSOLE_CMD_TABLE[i].name, e->cur->buf) == 0) {
        i = (i + 1) & 0xff;
    }
    if (i < CONSOLE_CMD_COUNT) {
        console_handler_t h = CONSOLE_CMD_TABLE[i].handler;
        if (h != 0) {
            (void)strchr(e->cur->buf, ' ');   /* OEM splits the arg list here */
            h();
        }
        return 1;
    }
    return 0;
}

/* VT100/ANSI escape decoder. Reads one key via g_console_io->rx_byte and folds
 * CSI / SS3 sequences into internal key codes (0x80..0x98 for the editing keys
 * the line editor handles). Returns 1 when *out holds a key, 0 while a sequence
 * is still being assembled or no byte is ready; a lone ESC is emitted once the
 * disambiguation timer expires. OEM 0x080431A4. */
int console_vt100_decode_key(uint8_t *out)
{
    uint8_t key;
    int got = g_console_io->rx_byte(&key);

    if (got == 0) {
        int idle = scheduler_slot_is_idle(CONSOLE_ESC_TIMER);
        if (idle == 0) {
            return 0;
        }
        if (CONSOLE_ESC_STATE != 1) {
            return 0;
        }
        *out = 0x1b;
        CONSOLE_ESC_STATE = 0;
        return idle;
    }

    switch (CONSOLE_ESC_STATE) {
    case 0:
        if (key == 0x1b) {
            scheduler_start(CONSOLE_ESC_TIMER, 10, (sched_cb_t)0);
            CONSOLE_ESC_STATE = 1;
            got = 0;
        } else {
            *out = key;
        }
        break;

    case 1:                                   /* seen ESC */
        if (key == 'O') {
            CONSOLE_ESC_STATE = 2;
            got = 0;
        } else if (key == '[') {
            CONSOLE_ESC_STATE = 3;
            got = 0;
        } else {
            got = 0;
            CONSOLE_ESC_STATE = 0;
        }
        break;

    case 2:                                   /* ESC O -> F1..F4 */
        switch (key) {
        case 'P': *out = 0x95; CONSOLE_ESC_STATE = 0; break;
        case 'Q': *out = 0x96; CONSOLE_ESC_STATE = 0; break;
        case 'R': *out = 0x97; CONSOLE_ESC_STATE = 0; break;
        case 'S': *out = 0x98; CONSOLE_ESC_STATE = 0; break;
        default:  got = 0;     CONSOLE_ESC_STATE = 0; break;
        }
        break;

    case 3:                                   /* ESC [ */
        switch (key) {
        case '1': CONSOLE_ESC_PARAM = 0x8c; CONSOLE_ESC_STATE = 4; got = 0; break;
        case '2': CONSOLE_ESC_PARAM = 0x8d; CONSOLE_ESC_STATE = 5; got = 0; break;
        case '4': CONSOLE_ESC_PARAM = 0x8e; CONSOLE_ESC_STATE = 6; got = 0; break;
        case '5': CONSOLE_ESC_PARAM = 0x8f; CONSOLE_ESC_STATE = 6; got = 0; break;
        case '6': CONSOLE_ESC_PARAM = 0x90; CONSOLE_ESC_STATE = 6; got = 0; break;
        case 'A': *out = 0x91; CONSOLE_ESC_STATE = 0; break;
        case 'B': *out = 0x92; CONSOLE_ESC_STATE = 0; break;
        case 'C': *out = 0x93; CONSOLE_ESC_STATE = 0; break;
        case 'D': *out = 0x94; CONSOLE_ESC_STATE = 0; break;
        default:  got = 0;     CONSOLE_ESC_STATE = 0; break;
        }
        break;

    case 4:                                   /* ESC [ 1 <n> ~ -> 0x80..0x87 */
        if (key == '~') {
            *out = CONSOLE_ESC_PARAM;
            CONSOLE_ESC_STATE = 0;
            return got;
        } else {
            uint8_t kc = 0;
            switch (key) {
            case '1': kc = 0x80; break;
            case '2': kc = 0x81; break;
            case '3': kc = 0x82; break;
            case '4': kc = 0x83; break;
            case '5': kc = 0x84; break;
            case '7': kc = 0x85; break;
            case '8': kc = 0x86; break;
            case '9': kc = 0x87; break;
            default:  break;
            }
            if (kc != 0) {
                CONSOLE_ESC_PARAM = kc;
                CONSOLE_ESC_STATE = 6;
            } else {
                CONSOLE_ESC_STATE = 0;
            }
            got = 0;
        }
        break;

    case 5:                                   /* ESC [ 2 <n> ~ -> 0x88..0x8B */
        if (key == '~') {
            *out = CONSOLE_ESC_PARAM;
            CONSOLE_ESC_STATE = 0;
            return got;
        } else {
            uint8_t kc = 0;
            switch (key) {
            case '0': kc = 0x88; break;
            case '1': kc = 0x89; break;
            case '3': kc = 0x8a; break;
            case '4': kc = 0x8b; break;
            default:  break;
            }
            if (kc != 0) {
                CONSOLE_ESC_PARAM = kc;
                CONSOLE_ESC_STATE = 6;
            } else {
                CONSOLE_ESC_STATE = 0;
            }
            got = 0;
        }
        break;

    case 6:                                   /* wait for the terminating ~ */
        if (key == '~') {
            *out = CONSOLE_ESC_PARAM;
            CONSOLE_ESC_STATE = 0;
        } else {
            got = 0;
            CONSOLE_ESC_STATE = 0;
        }
        break;

    default:
        got = 0;
        break;
    }
    return got;
}

/* ── The VT100 line editor (OEM 0x080434F8) ───────────────────────────────── */

/* Re-arm the console activity/idle timer on each accepted command line. */
extern void console_activity_timer_rearm(void);   /* 0x08029FE8 */

/* Terminal control sequences echoed while editing (resolved from flash). */
static const char ESC_ERASE_LINE[] = "\x1b[2K";   /* erase whole line */
static const char ESC_LEFT[]       = "\x1b[D";    /* cursor left */
static const char ESC_ERASE_EOL[]  = "\x1b[K";    /* erase to end of line */
static const char ESC_LEFT1[]      = "\x1b[1D";   /* cursor left one */
static const char ESC_RIGHT1[]     = "\x1b[1C";   /* cursor right one */
static const char LOGIN_PROMPT[]   = "\r\nLogin: ";
static const char UNKNOWN_CMD[]    = "'%s' is not recognized as an internal command.\r\n";
/* The pre-login path uses a separate copy with the British "recognised" spelling. */
static const char UNKNOWN_CMD_LOGIN[] = "'%s' is not recognised as an internal command.\r\n";
static const char CRLF[]           = "\r\n";

#define SESS_BASE  0x20009368u
#define ED_RING    ((console_edit_t *)(SESS_BASE + 0x2e4u))   /* the ring-control block */
#define CUR_BUF    (ED_RING->cur->buf)                        /* active line buffer */
#define CUR_LEN    (ED_RING->cur->len)                        /* active line length */
#define ED_CURSOR  (*(volatile uint8_t *)(SESS_BASE + 0x36au))  /* chars right of the cursor */
#define ED_LOGGED  (*(volatile uint8_t *)(SESS_BASE + 0x2d9u))  /* 0 = pre-login (echo '*') */
#define ED_FLAG354 (*(volatile uint8_t *)(SESS_BASE + 0x354u))

/* The interactive line editor: pull one decoded key and apply it to the active
 * line (insert/overwrite, backspace/delete with mid-line redraw, cursor and
 * Home/End movement, history up/down, tab-completion, and Enter -> dispatch).
 * Pre-login each line is a login attempt and is echoed as '*'. OEM 0x080434F8. */
void console_line_editor(void)
{
    uint8_t key;
    int i;

    if (console_vt100_decode_key(&key) == 0) {
        return;
    }

    if (key < 0x1c) {
        if (key > 7) {
            switch (key) {
            case 8:                                   /* Backspace */
                if (CUR_LEN == 0) {
                    return;
                }
                console_autocomplete_apply(ED_RING);
                g_console_io->puts(ESC_LEFT);
                if (ED_CURSOR != 0) {
                    for (i = CUR_LEN - ED_CURSOR; i < CUR_LEN; i++) {
                        g_console_io->tx_byte((uint8_t)CUR_BUF[i]);
                        CUR_BUF[i - 1] = CUR_BUF[i];
                    }
                }
                g_console_io->puts(ESC_ERASE_EOL);
                for (i = CUR_LEN - ED_CURSOR; i < CUR_LEN; i++) {
                    g_console_io->puts(ESC_LEFT1);
                }
                CUR_LEN = (uint8_t)(CUR_LEN - 1);
                return;

            case 9:                                   /* Tab — autocomplete */
                if (ED_LOGGED == 0 || CUR_LEN == 0) {
                    return;
                }
                if (console_autocomplete_search(ED_RING) == 0) {
                    return;
                }
                g_console_io->puts(ESC_ERASE_LINE);
                g_console_io->tx_byte('\r');
                /* echo the staged completion (the matched name), not the line */
                g_console_io->write((const uint8_t *)ED_RING->ac_match, ED_RING->ac_len);
                return;

            case 0x0d:                                /* Enter */
                ED_CURSOR = 0;
                if (ED_LOGGED == 0) {
                    g_console_io->puts(LOGIN_PROMPT);
                    console_autocomplete_apply(ED_RING);
                    CUR_BUF[CUR_LEN] = '\0';
                    if (console_dispatch_login(ED_RING) == 0) {
                        g_console_io->printf(UNKNOWN_CMD_LOGIN, CUR_BUF);
                    }
                    ED_FLAG354 = 0;
                    CUR_BUF[0] = '\0';
                    CUR_LEN = 0;
                    return;
                }
                console_activity_timer_rearm();
                g_console_io->puts(CRLF);
                if (CUR_LEN == 0) {
                    return;
                }
                console_autocomplete_apply(ED_RING);
                CUR_BUF[CUR_LEN] = '\0';
                if (console_dispatch_command(ED_RING) == 0) {
                    g_console_io->printf(UNKNOWN_CMD, CUR_BUF);
                }
                console_history_rotate(ED_RING);
                return;

            case 0x1b:                                /* Esc — clear line */
                ED_CURSOR = 0;
                if (CUR_LEN == 0) {
                    return;
                }
                console_autocomplete_apply(ED_RING);
                g_console_io->puts(ESC_ERASE_LINE);
                g_console_io->tx_byte('\r');
                CUR_LEN = 0;
                return;
            }
        }
    } else if (key < 0x95 && key > 0x7e) {
        switch (key) {
        case 0x7f:                                    /* Delete (forward) */
            if (CUR_LEN == 0) {
                return;
            }
            console_autocomplete_apply(ED_RING);
            if (ED_CURSOR != 0) {
                i = CUR_LEN - ED_CURSOR;
                while (i + 1 < CUR_LEN) {
                    g_console_io->tx_byte((uint8_t)CUR_BUF[i + 1]);
                    CUR_BUF[i] = CUR_BUF[i + 1];
                    i++;
                }
            }
            g_console_io->puts(ESC_ERASE_EOL);
            for (i = CUR_LEN - ED_CURSOR + 1; i < CUR_LEN; i++) {   /* OEM repositions one fewer than Backspace */
                g_console_io->puts(ESC_LEFT1);
            }
            CUR_LEN = (uint8_t)(CUR_LEN - 1);
            ED_CURSOR = (uint8_t)(ED_CURSOR - 1);
            return;

        case 0x8c:                                    /* Home */
            for (i = 0; i < CUR_LEN - ED_CURSOR; i++) {
                g_console_io->puts(ESC_LEFT1);
            }
            ED_CURSOR = CUR_LEN;
            return;

        case 0x8e:                                    /* End */
            if (ED_CURSOR == 0) {
                return;
            }
            for (i = CUR_LEN - ED_CURSOR; i < CUR_LEN; i++) {
                g_console_io->puts(ESC_RIGHT1);
            }
            ED_CURSOR = 0;
            return;

        case 0x91:                                    /* Up — history prev */
            ED_CURSOR = 0;
            if (ED_LOGGED == 0) {
                return;
            }
            console_history_prev(ED_RING);
            g_console_io->puts(ESC_ERASE_LINE);
            g_console_io->tx_byte('\r');
            g_console_io->write((const uint8_t *)CUR_BUF, CUR_LEN);
            return;

        case 0x92:                                    /* Down — history next */
            ED_CURSOR = 0;
            if (ED_LOGGED == 0) {
                return;
            }
            console_history_next(ED_RING);
            g_console_io->puts(ESC_ERASE_LINE);
            g_console_io->tx_byte('\r');
            g_console_io->write((const uint8_t *)CUR_BUF, CUR_LEN);
            return;

        case 0x93:                                    /* Right arrow */
            if (ED_CURSOR == 0) {
                return;
            }
            ED_CURSOR = (uint8_t)(ED_CURSOR - 1);
            g_console_io->puts(ESC_RIGHT1);
            return;

        case 0x94:                                    /* Left arrow */
            if (CUR_LEN - ED_CURSOR < 1) {
                return;
            }
            ED_CURSOR = (uint8_t)(ED_CURSOR + 1);
            g_console_io->puts(ESC_LEFT1);
            return;
        }
    }

    /* Printable character — insert at the cursor (capacity 0x50). */
    if (CUR_LEN < 0x50 && key > 0x1f && key < 0x7f) {
        console_autocomplete_apply(ED_RING);
        if (ED_LOGGED == 0) {
            g_console_io->tx_byte('*');
        } else {
            g_console_io->tx_byte(key);
        }
        if (ED_CURSOR == 0) {
            CUR_BUF[CUR_LEN] = (char)key;
        } else {
            int dst;
            CUR_LEN = (uint8_t)(CUR_LEN + 1);
            dst = CUR_LEN - ED_CURSOR;
            for (i = CUR_LEN; i >= dst; i--) {
                CUR_BUF[i] = CUR_BUF[i - 1];
            }
            CUR_BUF[dst - 1] = (char)key;
            for (i = CUR_LEN - ED_CURSOR; i < CUR_LEN; i++) {
                g_console_io->tx_byte((uint8_t)CUR_BUF[i]);
            }
            for (i = CUR_LEN - ED_CURSOR; i < CUR_LEN; i++) {
                g_console_io->puts(ESC_LEFT1);
            }
        }
        if (ED_CURSOR == 0) {
            CUR_LEN = (uint8_t)(CUR_LEN + 1);
        }
    }
}
