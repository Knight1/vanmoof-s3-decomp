#ifndef MAINWARE_MOTOR_H
#define MAINWARE_MOTOR_H

#include <stdint.h>

/* Mainware side of the motor-controller firmware-update path. The motor MCU is a
 * TI F2806x (TMS320F28054F, C28x DSP); the OEM translation unit is literally
 * `src/F2806/f2806x.c` (its assert filename string). The DSP is reprogrammed
 * over the inter-module ("SSPM") bus with a YMODEM-like block protocol driven by
 * motor_fw_update_fsm_step (0x08030FF4).
 *
 * These are its four non-blocking byte/buffer transfer pumps: each advances at
 * most one byte per super-loop tick and is bounded by a shared scheduler-timeout
 * slot, so the caller polls until done. All four return:
 *   1 = busy (call again), 0 = transfer complete, 2 = timed out. */

/* Send `count` bytes from `src` over the SSPM bus (OEM 0x08030B70). */
int motor_dl_send_buf_step(const uint8_t *src, int count, uint32_t timeout_ms);

/* Receive `count` bytes into `dst` (OEM 0x080309C4). */
int motor_dl_recv_buf_step(uint8_t *dst, int count, uint32_t timeout_ms);

/* Receive one byte into `*out` (OEM 0x08030A50). */
int motor_dl_recv_byte_step(uint8_t *out, uint32_t timeout_ms);

/* Receive a little-endian u16 into `*out` (OEM 0x08030ADC). */
int motor_dl_recv_u16_step(uint16_t *out, uint32_t timeout_ms);

/* ---------------------------------------------------------------------------
 * Transaction layer — the handshake/transfer state machines that the download
 * FSM (motor_fw_update_fsm_step) drives on top of the pumps above. They share
 * the same download context (0x20000654) at higher offsets. Each returns
 * 1 = busy / 0 = transfer complete / 2 = error or timeout.
 * ------------------------------------------------------------------------- */

/* Send `count` bytes from `buf`, verifying each byte is echoed back unchanged
 * (the C2000 SCI bootloader echoes every received byte). OEM 0x08030BFC. */
int motor_dl_send_verify_step(const uint8_t *buf, int count);

/* Autobaud lock: send 'A' (0x41) and expect 'A' echoed back, logging
 * "Autobaud ok" / "Err Autobaud [..]" / "Autobaud no answer". OEM 0x08030CEC. */
int motor_dl_autobaud_step(void);

/* Stream a length-prefixed firmware block with a running 16-bit checksum,
 * verified against the DSP's echoed checksum per block / periodically. The block
 * length is parsed from the first two streamed bytes. OEM 0x08030D88. */
int motor_dl_stream_block_step(const uint8_t *buf, int start);

/* The top-level motor-controller (F2806x) firmware-update FSM, ticked from the
 * super-loop. 14 states: reset-pulse the DSP into its serial bootloader, SCI
 * autobaud, upload a handshake payload + the staged motor pack (validating its
 * magic/type/version header), then release the DSP. OEM motor_fw_update_fsm_step
 * at 0x08030FF4. Returns 0 = done/idle, 1 = busy, 2 = failed, 3 = waiting. */
unsigned int motor_fw_update_fsm_step(void);

#endif
