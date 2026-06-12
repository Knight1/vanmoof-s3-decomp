#ifndef MAINWARE_CONSOLE_EDIT_H
#define MAINWARE_CONSOLE_EDIT_H

#include <stdint.h>

/* Interactive front-end of the ES3 debug console: the command dispatchers over
 * the 49-entry command table (flash 0x0804F5C4), the 9-slot input-history ring,
 * tab-autocomplete, the command/whitespace token matcher, and the VT100
 * escape-sequence -> internal-keycode decoder. The VT100 line editor proper
 * (OEM 0x080434F8) drives these and is sourced separately.
 *
 * The editor state lives in the per-session console context at SRAM 0x20009368:
 * nine 0x51-byte history line buffers at +i*0x51, this ring-control block at
 * +0x2E4, the active-line pointer at +0x350, the in-line cursor offset at +0x36A
 * and the escape-decoder state at +0x368/+0x369. */

/* One history slot (OEM 0xC-byte record). */
typedef struct {
    uint8_t  age;       /* +0x00  0 = newest .. 8, advanced on each Enter */
    uint8_t  _r1[3];
    char    *buf;       /* +0x04  line buffer (0x20009368 + slot*0x51) */
    uint8_t  len;       /* +0x08  stored length */
    uint8_t  _r2[3];
} console_hist_slot_t;

/* Ring-control block (OEM ctx +0x2E4). */
typedef struct {
    console_hist_slot_t  slots[9];  /* +0x00 */
    console_hist_slot_t *cur;       /* +0x6C  the active edit slot */
    uint8_t  nav;                   /* +0x70  history-recall index */
    uint8_t  _r70[7];
    const char *ac_match;           /* +0x78  staged autocomplete name */
    uint8_t  ac_len;                /* +0x7C  its length */
    uint8_t  _r7c[3];
    uint8_t  ac_cursor;             /* +0x80  autocomplete scan cursor */
    uint8_t  _r80[3];
} console_edit_t;

int  console_cmd_match(const char *name, const char *input);    /* 0x08040904 */
void console_history_init(console_edit_t *e);                   /* 0x0804094C */
void console_history_rotate(console_edit_t *e);                 /* 0x080409A0 */
int  console_dispatch_login(console_edit_t *e);                 /* 0x08040A00 */
void console_history_recall(console_edit_t *e);                 /* 0x08041184 */
void console_history_prev(console_edit_t *e);                   /* 0x080411CA */
void console_history_next(console_edit_t *e);                   /* 0x080411FA */
void console_autocomplete_apply(console_edit_t *e);            /* 0x08041232 */
int  console_autocomplete_search(console_edit_t *e);          /* 0x0804125C */
int  console_dispatch_command(console_edit_t *e);             /* 0x080426BC */
int  console_vt100_decode_key(uint8_t *out);                  /* 0x080431A4 */
void console_line_editor(void);                              /* 0x080434F8 */

#endif
