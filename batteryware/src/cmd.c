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
