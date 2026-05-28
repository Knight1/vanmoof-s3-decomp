#include "batteryware.h"

/*
 * Modbus command response helpers.
 *
 * These functions are called from the main Modbus command parser
 * (at FUN_0800AFA4, the byte-at-a-time command handler). They operate
 * on the caller's stack frame via frame-relative addressing:
 *   r7+0xBF       : local loop counter (byte)
 *   r7+0xC0       : incoming command data field
 *   DAT externals : command counter / response buffer pointer
 *
 * cmd_counter_inc / _v2 / _v3: extract a byte from r7+0xC0 if < 0x100,
 *   write to a specific SRAM location, increment the command counter,
 *   then call cmd_send_response to output 8 bytes and tail to epilogue.
 *
 * cmd_write_and_inc: extract 2 bytes from r7+0xC0, write to a struct
 *   field at +0x4C, verify via memcmp_verify, inc counter, send response.
 *
 * cmd_send_response / cmd_send_8byte: output 8 bytes from a lookup
 *   table at DAT_0800d8e4 via uart_putchar, then tail to epilogue.
 */

/* Command state globals */
static volatile uint8_t  * const s_cmd_counter = (volatile uint8_t *)0x20002CCC;  /* cmd_counter */
static volatile uint8_t  * const s_cmd_field_a = (volatile uint8_t *)0x20002CF4;  /* v1 dest */
static volatile uint8_t  * const s_cmd_field_b = (volatile uint8_t *)0x20002CF8;  /* v2 dest */
static volatile uint8_t  * const s_cmd_field_c = (volatile uint8_t *)0x20002CD0;  /* v3 dest */
static volatile uint8_t  * const s_cmd_counter2 = (volatile uint8_t *)0x20002CD4;  /* v3 counter */

/* Response data table (8 bytes), SRAM-resident */
static volatile uint8_t  * const s_resp_data    = (volatile uint8_t *)0x20002CE4;
/* Response enable flag */
static volatile uint16_t * const s_resp_enabled = (volatile uint16_t *)0x20002CE8;
/* Response sent flag */
static volatile uint8_t  * const s_resp_done    = (volatile uint8_t *)0x20002CEC;

/*
 * Extract command byte from frame at r7+0xC0 if within range.
 * Equivalent to: if (frame_word < 0x100) *dest = (uint8_t)frame_word;
 */
static void cmd_store_byte(volatile uint8_t *dest, uint16_t frame_word)
{
    if (frame_word < 0x100) {
        *dest = (uint8_t)frame_word;
    }
}

/* Tail-call to epilogue thunk */
extern void epilogue_thunk(void);  /* thunk_e1c4 */

/*
 * Send an 8-byte response from the lookup table.
 * Iterates through s_resp_data[0..7], sending each via uart_putchar,
 * then clears the sent flag and tails to epilogue.
 */
static void cmd_send_8bytes(void)
{
    for (uint8_t i = 0; i < 8; i++) {
        uart_putchar(s_resp_data[i]);
    }
    *s_resp_done = 0;
}

/*
 * cmd_counter_inc: store byte at s_cmd_field_a, inc counter, send response.
 */
void cmd_counter_inc(uint16_t frame_word)
{
    cmd_store_byte(s_cmd_field_a, frame_word);
    *s_cmd_counter += 1;
    cmd_send_8bytes();
    epilogue_thunk();
}

/*
 * cmd_counter_inc_v2: store byte at s_cmd_field_b, inc counter, send response.
 */
void cmd_counter_inc_v2(uint16_t frame_word)
{
    cmd_store_byte(s_cmd_field_b, frame_word);
    *s_cmd_counter += 1;
    cmd_send_8bytes();
    epilogue_thunk();
}

/*
 * cmd_counter_inc_v3: store byte at s_cmd_field_c via alternate counter.
 */
void cmd_counter_inc_v3(uint16_t frame_word)
{
    cmd_store_byte(s_cmd_field_c, frame_word);
    *s_cmd_counter2 += 1;
    cmd_send_8bytes();
    epilogue_thunk();
}

/*
 * cmd_write_and_inc: writes 2 bytes to struct at +0x4C, verifies via
 * memcmp_verify, incs counter, sends response.
 */
void cmd_write_and_inc(uint16_t frame_word, volatile uint8_t *struct_base, volatile uint8_t *verify_buf)
{
    *(volatile uint16_t *)(struct_base + 0x4C) = frame_word;
    memcmp_verify((char *)struct_base, 2, (char *)verify_buf);
    *s_cmd_counter2 += 1;
    cmd_send_8bytes();
    epilogue_thunk();
}

/*
 * cmd_send_response: conditionally sends 8-byte response if
 * s_resp_enabled is set, then tails to epilogue.
 */
void cmd_send_response(void)
{
    if (*s_cmd_counter2 != 0) {
        cmd_send_8bytes();
    }
    *s_resp_done = 0;
    epilogue_thunk();
}

/*
 * cmd_send_8byte: unconditionally sends 8-byte response.
 */
void cmd_send_8byte(void)
{
    cmd_send_8bytes();
    epilogue_thunk();
}

/*
 * command_parser — "KEY=VALUE" Modbus/UART command dispatcher.
 *
 * OEM: FUN_08009AC4 (820 B). Receives a parsed frame body, extracts
 * a name token, optionally extracts a hex/digit value after `=`, and
 * dispatches to a flash-resident function pointer table.
 *
 * State machine (`parse_state`, [r7+0x67]):
 *   0  reading name
 *   3  saw `=`, expect value
 *   4  name truncated (control char or high-bit, or > 20 chars)
 *   5  value complete
 *   6  invalid value char or value > 20 chars
 *
 * Name extraction: accepts byte if `c > 0x1F && c < 0x80 && c != '='`,
 *   storing into `name_buf[name_pos]` (cap 20). `=` flips to state 3.
 *
 * Value extraction (state 3): accepts only ASCII `'0'..'9'` and
 *   `'A'..'F'` (uppercase hex), storing into `value_buf[value_pos]`
 *   (cap 20). Anything else → state 6.
 *
 * Matching (first-match-wins prefix scan):
 *   For each table entry, count chars of name_buf that equal the
 *   entry's name[0..name_pos-1]. If `match_count == name_pos`, take
 *   the entry's `idx` byte and break (first match wins).
 *   Failed match calls `veneer_a6aa()` then continues anyway — that
 *   thunk is OEM logging, not a fatal abort.
 *
 * Dispatch: `jump_table[best_action](...)`. OEM jump table lives at
 *   0x08017FD8 — past the end of this .bin (image size 0x15610), so
 *   we stub the dispatch with NULL pointers. The 23 entries here
 *   exactly mirror the OEM 47-byte slots at 0x08012B9C..0x08012FD5.
 */
static const cmd_entry_t s_cmd_table[] = {
    {  1,  4, cmd_who             },  /* 0x08012b9c */
    {  2,  4, cmd_now             },  /* 0x08012bcb */
    {  3,  2, cmd_pf              },  /* 0x08012bfa */
    {  4,  9, cmd_reset_bms       },  /* 0x08012c29 */
    {  5,  2, cmd_df              },  /* 0x08012c58 */
    {  6, 10, cmd_upgrade_ap      },  /* 0x08012c87 */
    {  7, 10, cmd_upgrade_bl      },  /* 0x08012cb6 */
    {  8, 15, cmd_into_bootloader },  /* 0x08012ce5 */
    {  9,  7, cmd_chg_cal_set     },  /* 0x08012d14 */
    { 10,  8, cmd_chg_cal_get     },  /* 0x08012d43 */
    { 11,  7, cmd_dsg_cal_set     },  /* 0x08012d72 */
    { 12,  8, cmd_dsg_cal_get     },  /* 0x08012da1 */
    { 13,  9, cmd_reset_esn       },  /* 0x08012dd0 */
    { 14,  9, cmd_log_clear       },  /* 0x08012dff */
    { 15,  3, cmd_ts0_set         },  /* 0x08012e2e */
    { 16,  3, cmd_ts1_set         },  /* 0x08012e5d */
    { 17,  3, cmd_ts2_set         },  /* 0x08012e8c */
    { 18,  4, cmd_ts0_get         },  /* 0x08012ebb */
    { 19,  4, cmd_ts1_get         },  /* 0x08012eea */
    { 20,  4, cmd_ts2_get         },  /* 0x08012f19 */
    { 21,  8, cmd_ts_reset        },  /* 0x08012f48 */
    { 22,  3, cmd_fcc             },  /* 0x08012f77 */
    { 23,  3, cmd_soc             },  /* 0x08012fa6 */
};

#define CMD_TABLE_LEN  (sizeof(s_cmd_table) / sizeof(s_cmd_table[0]))

/* Jump table — OEM stores 24 function pointers at flash 0x08017FD8,
 * indexed by entry.idx. That address is past the end of this .bin
 * (0x15610), so the table contents are unknown to this decomp.
 * Indices 1..23 are valid; 0 is unused. */
static void (* const s_jump_table[24])(uint32_t, int, uint8_t) = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};

extern void veneer_a6aa(void);  /* OEM parse-error log hook */
extern void veneer_a6e0(void);  /* OEM dispatch-error log hook */

void command_parser(uint32_t buf_addr, int buf_len, uint8_t cmd_byte)
{
    const uint8_t *buf = (const uint8_t *)(uintptr_t)buf_addr;

    char     name_buf[21];
    char     value_buf[21];
    uint8_t  name_pos    = 0;
    uint8_t  value_pos   = 0;
    uint8_t  read_idx    = 0;
    uint8_t  parse_state = 0;
    uint8_t  i;

    (void)cmd_byte;
    (void)value_buf;

    if (buf_len == 0) {
        veneer_a6aa();
        return;
    }

    /* --- Name extraction ------------------------------------ */
    while (read_idx < (uint8_t)buf_len && parse_state == 0) {
        uint8_t c = buf[read_idx];

        if (c == '=') {
            read_idx++;
            parse_state = 3;
            break;
        }
        /* OEM: reject c <= 31 or c with high bit set (sxtb < 0). */
        if (c <= 31 || (int8_t)c < 0) {
            parse_state = 4;
            break;
        }
        name_buf[name_pos++] = (char)c;
        if (name_pos > 20) {
            parse_state = 4;
            break;
        }
        read_idx++;
        if (read_idx >= (uint8_t)buf_len && parse_state == 0) {
            /* OEM falls through to state 3 check; with state still 0
             * the matching phase proceeds with whatever name has so far. */
            break;
        }
    }

    /* --- Value extraction (only if we saw '=') -------------- */
    if (parse_state == 3) {
        parse_state = 0;
        if (read_idx >= (uint8_t)buf_len) {
            parse_state = 5;
        }
        while (read_idx < (uint8_t)buf_len && parse_state == 0) {
            uint8_t c = buf[read_idx];

            /* Accept '0'..'9' OR 'A'..'F'. */
            int is_digit = (c >= '0' && c <= '9');
            int is_hex   = (c >= 'A' && c <= 'F');
            if (!(is_digit || is_hex)) {
                parse_state = 6;
                break;
            }
            value_buf[value_pos++] = (char)c;
            if (value_pos > 20) {
                parse_state = 6;
                break;
            }
            read_idx++;
            if (read_idx >= (uint8_t)buf_len) {
                parse_state = 5;
                break;
            }
        }

        /* OEM: if state != 5 after the value loop, log the error,
         * but continue into matching anyway. */
        if (parse_state != 5) {
            veneer_a6aa();
        }
    }

    /* --- Matching (first prefix match wins) ----------------- */
    uint8_t best_action = 0;
    uint8_t matched     = 0;
    for (i = 0; i < CMD_TABLE_LEN; i++) {
        uint8_t score = 0;
        for (uint8_t j = 0; j < name_pos; j++) {
            if (j < s_cmd_table[i].name_len &&
                name_buf[j] == s_cmd_table[i].name[j]) {
                score++;
            }
        }
        if (score == name_pos) {
            best_action = s_cmd_table[i].idx;
            matched = 1;
            break;
        }
    }

    /* --- Dispatch ------------------------------------------- */
    if (!matched) {
        veneer_a6aa();
    }
    if (best_action > 23) {
        veneer_a6e0();
    } else if (s_jump_table[best_action] != NULL) {
        s_jump_table[best_action](buf_addr, buf_len, best_action);
    }
}
