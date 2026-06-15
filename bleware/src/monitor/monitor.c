/* monitor/monitor.c — debug-console input parser / task body.
 *
 * This is the heart of the OEM `source/monitor/monitor.c` translation
 * unit: the per-iteration task body that reads one command line off the
 * console, stashes a heap copy of it (used as the up-arrow history
 * entry), and hands the live line buffer to `monitor_dispatch_loop`
 * (OEM 0x00024B38) for command matching. The dispatch loop and the
 * static command table (OEM 0x0002A0BC) are decoded in dispatcher.c and
 * table.c respectively.
 *
 * The OEM `monitor.c` TU is large — the linker scattered many unrelated
 * functions' `monitor_log` file-string literals across the image, which
 * is why "source/monitor/monitor.c" appears at four flash offsets
 * (0x0000A04C, 0x0000CAA8, 0x00010E24, 0x000139C8). Only the two
 * functions below — the line reader and the dispatch body — are the
 * actual console input parser; the others (power/state/HCI routines that
 * happen to log with the same __FILE__) live in their own TUs.
 *
 * Functions decoded:
 *   monitor_readline        @ 0x00009E84  (~456 B body)
 *   monitor_task_iteration  @ 0x00013924  (164 B body)
 *   monitor_predict_command @ 0x0000C948  (tab-completion; ~344 B body)
 *   monitor_match_len       @ 0x0002621A  (common-prefix helper)
 *
 * Note on discovery: `monitor_task_iteration` has NO static cross-
 * reference and is not reached by any BL or literal-pool address in the
 * image — the monitor task entry installs it indirectly (the same
 * register-indirect pattern as the central GATT write dispatcher). It
 * was pinned by scanning the raw image for the single Thumb `BL` whose
 * computed target is 0x00024B38 (`monitor_dispatch_loop`); that BL lives
 * at 0x00013996, inside this function.
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"
#include "monitor.h"

/* ---- Console-line shared state (RAM 0x2000457C) -----------------------
 *
 * Both functions in this file share a small RAM record:
 *
 *   +0x00  char *history    — heap copy of the last accepted line; the
 *                             up-arrow editor recalls it, and the
 *                             dispatch body refreshes it each line.
 *   +0x04  char  line[]     — the active edit buffer the reader fills
 *                             and the dispatcher consumes (>= 300 B).
 *
 * The OEM addresses this record via a literal pool word; we mirror that
 * with a typed pointer pinned at the same address so the emitted access
 * sequence matches. */
struct monitor_console {
    char *history;          /* +0x00 */
    char  line[1];          /* +0x04 — variable length edit buffer */
};

#define MONITOR_CONSOLE_ADDR  0x2000457Cu
static struct monitor_console *const s_console =
    (struct monitor_console *)MONITOR_CONSOLE_ADDR;

/* Console-tick divisor used to derive the reader's idle-yield interval.
 * The OEM reads a global (flash literal 0x0002BB88 -> RAM-resident
 * config word, observed value 10) and computes (1000 / div) * 1000 µs.
 * It is a system-wide config word touched by many subsystems, so we
 * reference it by its OEM symbol rather than redefining it here. */
extern uint32_t g_console_tick_divisor;     /* *0x0002BB88, == 10 */

/* __FUNCTION__ literals the OEM monitor_log() calls pass for these two
 * functions (flash 0x0002A07C / 0x0002A092). */
static const char MON_FN_TASK[]     = "monitor";
static const char MON_FN_READLINE[] = "monitor_readline";

/* ---- Cross-TU console plumbing (the console-device driver layer) ------
 *
 * The first four live in src/monitor/console.c (the UART/console driver
 * glue); tagged with their OEM addresses. */

/* Read up to `count` bytes into `dst`, polling the console device with a
 * per-byte `timeout_us`. Returns the number of bytes read (-1 if the
 * console is disabled). OEM @ 0x000238A8. */
extern int  monitor_console_read(void *dst, int count, uint32_t timeout_us);

/* Drain the console TX FIFO (returns 0, or -1 if disabled).
 * OEM @ 0x000255D4. */
extern int  monitor_console_flush(void);

/* Post `flag_bits` to the bluetoothtask event-flag set and, when the
 * required mask is satisfied, signal the scheduler. The reader pulses it
 * with bit 0x4 between polls so co-resident tasks keep running.
 * OEM @ 0x000232B8. */
extern void monitor_event_signal(uint32_t flag_bits);

/* Returns 1 while the console session is active, 0 once it has been torn
 * down. In this image it is a constant 1. OEM @ 0x00027742. */
extern int  monitor_console_active(void);

/* Command-prediction / tab-completion: expand the partial command in
 * `line` (already `typed_len` chars long) in place, listing candidates
 * when the match is ambiguous, and return the number of characters
 * appended. Defined at the bottom of this file (OEM @ 0x0000C948, OEM
 * __func__ "monitor_predict_command"). `monitor_match_len` is its
 * common-prefix helper (OEM @ 0x0002621A). */
static int monitor_predict_command(char *line, int typed_len);
static int monitor_match_len(const char *a, const char *b);

/* The registered command table (src/monitor/dispatcher.c + table.c). */
extern const monitor_cmd_handler_t g_monitor_commands[];

/* memcmp — TI CGT runtime clone (src/runtime.c, FUN_00025490). */
extern int memcmp(const void *a, const void *b, unsigned int n);

/* Up-arrow recall buffer length / cursor (RAM 0x2000457C alias used by
 * the editor's history walk — same record as s_console, read as a raw
 * word). OEM literal 0x0000A078. */
extern uint32_t *g_monitor_history_word;    /* == (uint32_t *)0x2000457C */

/* monitor_log — location-aware console logger. OEM @ 0x00006D90.
 * Declared in monitor.h as monitor_log(file, line, fn, level, fmt, ...). */

/* C runtime helpers (TI CGT). */
extern void  *memcpy(void *dst, const void *src, unsigned int n);  /* 0x00018654 */
extern char  *strcpy(char *dst, const char *src);                  /* 0x00026AF4 */
extern int    strlen(const char *s);                               /* 0x00028058 */
/* monitor_alloc / monitor_free — TI-RTOS heap, declared in bleware.h
 * (OEM 0x00013470 / 0x00021B88). */

/* cmd_reset — table.c entry, OEM 0x0001D8E4. The reader force-resets the
 * device after five consecutive bare-ESC presses. */
extern int    cmd_reset(int verb, void *p2, void *p3, uint32_t p4);

/* monitor.c source-path literal used as the monitor_log __FILE__ arg by
 * both functions (flash 0x0000A04C / 0x000139C8 — identical content). */
static const char MON_FILE[] = "source/monitor/monitor.c";

/* Single-character echo format ("%c", flash 0x0000A068). */
static const char FMT_CHAR[] = "%c";

/* ----------------------------------------------------------------------
 * monitor_readline — assemble one console input line.
 *
 * Reads characters one at a time from the console, echoing and editing,
 * until a CR/LF terminator or the buffer fills. Handles:
 *
 *   ESC '[' 'A'  up-arrow: recall the previous command from the history
 *                slot, re-echoing it character by character
 *   ESC ESC...   five bare ESCs in a row -> cmd_reset(2, "reset")
 *   '\t'         tab completion (monitor_predict_command)
 *   '\b'         backspace: erase one char, emit "\b \b" when echoing
 *   '\r' / '\n'  line terminator: NUL-terminate and return
 *   other        append to buffer, echo via "%c"
 *
 * Between polls that return no data it yields to the scheduler
 * (monitor_event_signal(4)). Returns the number of characters placed in
 * `buf` (excluding the terminator), or 0xFFFFFFFF if the console was torn
 * down mid-read.
 *
 * `buf`           destination (may be NULL to discard input)
 * `max_len`       buffer capacity; the line is capped at max_len - 1
 * `echo`          non-zero to echo edits (backspace erase sequence)
 * `idle_yield_us` per-byte console poll timeout, in microseconds
 *
 * OEM @ 0x00009E84.
 * -------------------------------------------------------------------- */
unsigned int monitor_readline(char *buf, int max_len, int echo,
                              uint32_t idle_yield_us)
{
    char     ch;             /* single read byte (sp+0xC) */
    char     esc[3];         /* ESC-sequence tail (sp+0x8) */
    char    *out   = buf;
    uint32_t count = 0;      /* chars accepted so far */
    uint32_t esc_run = 0;    /* consecutive bare-ESC counter */

    for (;;) {
        /* Block for one byte, yielding to the scheduler while the
         * console stays active but empty. */
        int n;
        while ((n = monitor_console_read(&ch, 1, idle_yield_us)) < 1) {
            if (monitor_console_active() != 1) {
                break;
            }
            monitor_event_signal(4);
        }
        monitor_event_signal(4);
        if (monitor_console_active() == 0) {
            return 0xFFFFFFFFu;
        }

        if (ch == 0x1B) {                       /* ESC */
            n = monitor_console_read(esc, 3, 100);
            if (n == 2) {
                char **hist = (char **)g_monitor_history_word;
                char  *prev = *hist;
                if (esc[0] == '[' && esc[1] == 'A' && prev != NULL) {
                    /* Up-arrow: rub out the current line, then recall
                     * and re-echo the stored history string. The OEM
                     * passes the recalled line itself as the monitor_log
                     * format string, so it is re-emitted verbatim. */
                    while (count != 0) {
                        monitor_log(MON_FILE, 0x135, MON_FN_READLINE, 8,
                                    FMT_CHAR, 8);
                        count--;
                        prev = *hist;
                    }
                    monitor_log(MON_FILE, 0x137, MON_FN_READLINE, 8, prev);
                    /* OEM copies into the ORIGINAL buf base, then sets the
                     * cursor to buf + strlen. The rub-out loop above zeroed
                     * `count` but never moved `out` back, so `out` is still
                     * at buf+N here — copying to it (and `out += count`)
                     * would overrun. */
                    strcpy(buf, *hist);
                    count = (uint32_t)strlen(*hist);
                    monitor_free(*hist);
                    out  = buf + count;
                    *hist = NULL;       /* OEM clears the recall slot (stores 0) */
                }
            } else if (n == 1) {
                /* Bare ESC: five in a row triggers a reset. */
                if (++esc_run > 4) {
                    cmd_reset(2, (void *)"reset", NULL, 0);
                }
            }
        } else if (ch == '\r' || ch == '\n') {
            ch = (char)0;                       /* terminate */
            if (out != NULL) {
                *out = ch;
            }
            break;
        } else if (ch == '\t') {
            esc_run = 0;   /* OEM reloads the bare-ESC counter on any non-ESC char */
            int added = monitor_predict_command(buf, (int)count);
            out   += added;
            count += (uint32_t)added;
        } else if (ch == '\b') {
            esc_run = 0;
            if (count != 0) {
                if (out != NULL) {
                    out--;
                }
                count--;
                if (echo != 0) {
                    /* Emit the "\b \b" rub-out: backspace, space, backspace. */
                    monitor_log(MON_FILE, 0x172, MON_FN_READLINE, 8, FMT_CHAR, 8);
                    monitor_log(MON_FILE, 0x173, MON_FN_READLINE, 8, FMT_CHAR, 0x20);
                    monitor_log(MON_FILE, 0x174, MON_FN_READLINE, 8, FMT_CHAR, 8);
                }
            }
        } else {
            esc_run = 0;
            monitor_log(MON_FILE, 0x17A, MON_FN_READLINE, 8, FMT_CHAR, ch);
            if (out != NULL) {
                *out++ = ch;
            }
            count++;
        }

        if ((uint32_t)(max_len - 1) <= count) {
            break;
        }
    }

    monitor_console_flush();
    return count;
}

/* ----------------------------------------------------------------------
 * monitor_task_iteration — read one command line and dispatch it.
 *
 * One pass of the monitor task's main loop:
 *
 *   1. Read a line into the shared edit buffer (300-char cap, echo on,
 *      ~100 ms per-byte poll derived from the console tick divisor).
 *   2. Bail out entirely if the console session was torn down.
 *   3. On a non-empty line: free any previous history copy, allocate a
 *      fresh `len + 1` byte copy, memcpy the line in (NUL-terminated),
 *      and stash it as the new history entry.
 *   4. Log a blank line, then run monitor_dispatch_loop on the live
 *      buffer. If it returns != 1 (no command matched), log
 *      "Bad command <%s>." against the entered text.
 *   5. Re-emit the "\r\n> " prompt before returning.
 *
 * The dispatcher is handed the live edit buffer (s_console->line), not
 * the heap copy — the copy exists only for up-arrow recall.
 *
 * OEM @ 0x00013924. No static caller (installed indirectly by the
 * monitor task entry).
 * -------------------------------------------------------------------- */
void monitor_task_iteration(void)
{
    char    *line = s_console->line;
    uint32_t timeout_us = (1000u / g_console_tick_divisor) * 1000u;

    int read_len = (int)monitor_readline(line, 300, 1, timeout_us);

    if (monitor_console_active() == 0) {
        return;
    }

    if (read_len > 0) {
        if (s_console->history != NULL) {
            monitor_free(s_console->history);
            s_console->history = NULL;
        }

        char *copy = (char *)monitor_alloc((unsigned int)(read_len + 1) & 0xFFFFu);
        s_console->history = copy;
        if (copy != NULL) {
            memcpy(copy, line, (unsigned int)(read_len + 1));
            copy[read_len] = '\0';
        }

        monitor_log(MON_FILE, 0x1AE, MON_FN_TASK, 8, "\r\n");

        if (monitor_dispatch_loop(line) != 1) {
            monitor_log(MON_FILE, 0x1B4, MON_FN_TASK, 8,
                        "Bad command <%s>.\r\n", line);
        }
    }

    monitor_log(MON_FILE, 0x1B8, MON_FN_TASK, 8, "\r\n> ");
}

/* ----------------------------------------------------------------------
 * monitor_match_len — length of the leading run where `a` (a NUL-
 * terminated string) matches `b` byte-for-byte. Used to fold candidate
 * command names down to their longest common prefix.
 *
 * OEM @ 0x0002621A.
 * -------------------------------------------------------------------- */
static int monitor_match_len(const char *a, const char *b)
{
    int i = 0;
    while (a[i] != '\0' && b[i] == a[i]) {
        i++;
    }
    return i;
}

/* ----------------------------------------------------------------------
 * monitor_predict_command — tab-completion over the command table.
 *
 * Walks all 25 registered commands (g_monitor_commands), asking each for
 * its name (verb MON_CMD_FILL_NAME) and keeping those whose name starts
 * with the `typed_len` bytes already in `line`:
 *
 *   0 matches  -> return 0, `line` untouched.
 *   1 match    -> append the rest of that command's name, echo just that
 *                 appended suffix ("%s" of line + typed_len), and return
 *                 the number of characters appended.
 *   >1 matches -> list the candidates in columns (one "%-20s " each, with
 *                 a newline every fourth), append the longest common
 *                 prefix they share, re-emit the "\r\n> %s" prompt, and
 *                 return the characters appended.
 *
 * The first match is not printed when found; it is emitted retroactively
 * (alongside the second) once a second match appears — hence the listing
 * branches keyed on the running match count.
 *
 * OEM @ 0x0000C948 (OEM __func__ "monitor_predict_command"). The console
 * name buffers are 64 bytes each, matching the OEM stack frame.
 * -------------------------------------------------------------------- */
static const char MON_FN_PREDICT[] = "monitor_predict_command";

static int monitor_predict_command(char *line, int typed_len)
{
    if (typed_len == 0) {
        return 0;
    }

    char name[64];        /* candidate command name (verb FILL_NAME)      */
    char best[64];        /* last accepted name / longest common prefix   */
    int  matches = 0;     /* number of names matching the typed prefix     */
    int  common  = 0;     /* common-prefix length across all matches       */

    for (int i = 0; i < 25; i++) {
        if (g_monitor_commands[i](MON_CMD_FILL_NAME, name, NULL, 0) != 0) {
            continue;     /* empty slot */
        }
        if (memcmp(name, line, (unsigned int)typed_len) != 0) {
            continue;     /* name does not start with the typed text */
        }

        if (matches == 1) {
            /* second match: newline, then list the first (held in `best`)
             * and this one, and seed the common prefix from the pair. */
            monitor_log(MON_FILE, 0xDA, MON_FN_PREDICT, 8, "\r\n");
            monitor_log(MON_FILE, 0xDB, MON_FN_PREDICT, 8, "%-20s ", best);
            monitor_log(MON_FILE, 0xDC, MON_FN_PREDICT, 8, "%-20s ", name);
            common = monitor_match_len(best, name);
        } else if (matches >= 2) {
            int n = monitor_match_len(best, name);
            if (n < common) {
                common = n;
            }
            monitor_log(MON_FILE, 0xE9, MON_FN_PREDICT, 8, "%-20s ", name);
        }

        if (matches > 0 && (matches & 3) == 0) {
            monitor_log(MON_FILE, 0xEF, MON_FN_PREDICT, 8, "\r\n");
        }

        strcpy(best, name);
        matches++;
    }

    int appended = 0;
    if (matches == 1) {
        /* unique completion: append the remainder of the command name and
         * echo only that appended suffix (the OEM prints line + typed_len,
         * not the whole line — r6 = line+typed_len at the 0xFE log site). */
        strcpy(line + typed_len, best + typed_len);
        appended = strlen(best + typed_len);
        line[typed_len + appended] = '\0';
        monitor_log(MON_FILE, 0xFE, MON_FN_PREDICT, 8, "%s", line + typed_len);
    } else if (matches >= 2) {
        /* ambiguous: complete up to the longest common prefix */
        best[common] = '\0';
        strcpy(line + typed_len, best + typed_len);
        appended = common - typed_len;
        line[typed_len + appended] = '\0';
        monitor_log(MON_FILE, 0x108, MON_FN_PREDICT, 8, "\r\n> %s", line);
    } else {
        return 0;
    }

    return appended;
}
