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
 * Command parser (command_parser) — "KEY=VALUE" string command
 * dispatcher.
 *
 * Receives a command frame. Parses the command name, searches
 * a dispatch table of 24 commands in flash, and jumps to the
 * appropriate handler via a jump table.
 *
 * Command table at 0x08012085 (flash): 24 entries × 47 bytes.
 *   Offset 0-1: padding/reserved
 *   Offset 2:   command index (0-23)
 *   Offset 3:   name length
 *   Offset 4+:  command name (ASCII)
 *
 * Jump table at 0x0800A080: 24 function pointers (4 bytes each).
 *
 * Protocol:
 *   Frame: [0xAA] [cmd_byte] [data_lo] [data_hi] [data_lo2] [data_hi2] [CRC16_lo] [CRC16_hi]
 *   Data bytes 2-5 carry the command parameter value (uint16 pairs).
 *
 * Commands that take a "=VALUE" suffix use the data bytes after '=':
 *   CHG CAL=1234  → parser extracts "CHG CAL" as name, "1234" as hex value
 *   PF            → parser extracts "PF" as name, no value
 */

/*
 * Command table entry: index, name length, name string.
 * In the OEM binary this is at 0x08012085 (0x12B9A for entries 1-23).
 * We replicate it here as a compile-time constant array.
 */
/* 24 command entries at 47 bytes each = 1128 bytes */
/* Jump table: 24 function pointers at 4 bytes each = 96 bytes */

void command_parser(uint32_t buf_addr, int buf_len, uint8_t cmd_byte)
{
    /* Command name table — 24 entries from OEM flash at 0x08012085 */
    static const cmd_entry_t s_cmd_table[24] = {
        {{0x00,0x00}, 0, 10, "MOS Failure Mode"},
        {{0x00,0x08}, 1,  4, "Who?"},
        {{0x00,0x00}, 2,  4, "Now?"},
        {{0x00,0x00}, 3,  2, "PF"},
        {{0x00,0x00}, 4,  9, "Reset BMS"},
        {{0x00,0x00}, 5,  2, "DF"},
        {{0x00,0x00}, 6, 10, "Upgrade AP"},
        {{0x00,0x00}, 7, 10, "Upgrade BL"},
        {{0x00,0x00}, 8, 15, "Into BootLoader"},
        {{0x00,0x00}, 9,  7, "CHG CAL"},
        {{0x00,0x00},10,  8, "CHG CAL?"},
        {{0x00,0x00},11,  7, "DSG CAL"},
        {{0x00,0x00},12,  8, "DSG CAL?"},
        {{0x00,0x00},13,  9, "Reset ESN"},
        {{0x00,0x00},14,  9, "Log Clear"},
        {{0x00,0x00},15,  3, "TS0"},
        {{0x00,0x00},16,  3, "TS1"},
        {{0x00,0x00},17,  3, "TS2"},
        {{0x00,0x00},18,  4, "TS0?"},
        {{0x00,0x00},19,  4, "TS1?"},
        {{0x00,0x00},20,  4, "TS2?"},
        {{0x00,0x00},21,  8, "TS Reset"},
        {{0x00,0x00},22,  3, "FCC"},
        {{0x00,0x00},23,  3, "SOC"},
    };

    /* Jump table — populated at runtime, stored in SRAM at 0x20002CE0 */
    static void (*s_jump_table[24])(uint32_t, int, uint8_t);

    const uint8_t *cmd = (const uint8_t *)buf_addr;
    char     cmd_name[24];
    uint8_t  name_len;
    uint8_t  i;
    bool     found;

    (void)cmd_byte;

    /* Extract command name (up to '=' or control chars) */
    name_len = 0;
    for (i = 0; i < (uint8_t)buf_len && i < 23; i++) {
        uint8_t c = cmd[i];
        if (c == '=' || c == '\0' || c == '\r' || c == '\n' || c < 0x20) {
            break;
        }
        cmd_name[i] = (char)c;
    }
    name_len = i;
    cmd_name[name_len] = '\0';

    /* Search the command table (24 entries) */
    found = false;
    for (i = 0; i < 24; i++) {
        if (s_cmd_table[i].name_len == name_len) {
            uint8_t j;
            bool match = true;
            for (j = 0; j < name_len; j++) {
                if (s_cmd_table[i].name[j] != cmd_name[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                uint8_t action = s_cmd_table[i].idx;
                if (action < 24 && s_jump_table[action] != NULL) {
                    s_jump_table[action](buf_addr, buf_len, action);
                }
                found = true;
                break;
            }
        }
    }

    if (!found) {
        extern void veneer_a6aa(void);
        veneer_a6aa();
    }
}
