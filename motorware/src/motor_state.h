/* motorware L3-RAM control/state map (S.0.00.22).
 *
 * The application's working set lives in L3 DPSARAM (0x9000-0x9FFF); these are
 * the fields recovered from the disassembly so far. Word addresses (C28x: 1
 * word = 16 bits). Tags: [V]=verified from code, [~]=strong inference.
 * Cross-refs: docs/protocol.md (register map), docs/hardware.md (control loop).
 *
 * The firmware reaches peripherals through the HAL object via the pointer at
 * 0x903E, MotorWare-style. Without TI's cl2000 this header documents the map;
 * it is not yet linked.
 */
#ifndef MOTORWARE_MOTOR_STATE_H
#define MOTORWARE_MOTOR_STATE_H
#include <stdint.h>

/* ---- central handles --------------------------------------------------- */
#define L3_HAL_OBJ        0x903E  /* [V] HAL object pointer (124 refs); +0xC0 SciaRegs,
                                          +0xD2 ScicRegs, +0x78 PieVectTable, +0x82 EPwm4 */
#define L3_CTRL_OBJ       0x9024  /* [~] second handle (CTRL/EST, 56 refs) */

/* ---- status / control flags (0x9000-0x901F) ---------------------------- */
#define L3_STATUS_FLAGS   0x9017  /* [V] fault/status word (reported by read-reg 12,
                                          which then clears the low 2 acked bits). Each bit
                                          is set at a distinct detection point: */
/* status/fault bits and where the firmware sets them (function @addr). The
 * exact physical fault per bit is [~] inferred; the bit + setter are [V]. */
#define FAULT_INIT_A      0x0001  /* sub_3EE07C (startup self-check) */
#define FAULT_RX_ERR      0x0002  /* sub_3F3472 — SLIP receive error */
#define FAULT_BIT_0004    0x0004  /* sub_3EE791 */
#define FAULT_BIT_0008    0x0008  /* ISR sub_3F2444 */
#define FAULT_BIT_0010    0x0010  /* ISR sub_3F2444 */
#define FAULT_BIT_0020    0x0020  /* main loop sub_3EE894 */
#define FAULT_BIT_0040    0x0040  /* main loop sub_3EE894 */
#define FAULT_INIT_B      0x0100  /* sub_3EE07C */
#define FAULT_BIT_0200    0x0200  /* sub_3EE2FD */
#define FAULT_BIT_0400    0x0400  /* sub_3EE2FD */
#define FAULT_BIT_0800    0x0800  /* ISR sub_3F2444 */
#define FAULT_BIT_1000    0x1000  /* sub_3EE842 */
#define FAULT_BIT_2000    0x2000  /* sub_3EE34B / main loop */
#define STATUS_RUN_REQ    0x8000  /* write-reg 20 bit0 — run requested */
#define L3_ENABLE_FLAG    0x9013  /* [V] enable, from write-reg 20 frame[1].bit7 */
#define L3_STAT_BYTE_18   0x9018  /* [V] status byte (read-reg 12) */
#define L3_STAT_BYTE_19   0x9019  /* [V] status byte (read-reg 12) */
#define L3_STAT_BYTE_1A   0x901A  /* [V] status byte (read-reg 12) */
#define L3_CTRL_BYTE_07   0x9007  /* [~] control byte (FOC ISR) */
#define L3_CTRL_BYTE_0C   0x900C  /* [~] control byte */
#define L3_CTRL_BYTE_1C   0x901C  /* [~] control byte (FOC ISR) */

/* ---- measurements / parameters (0x9040-0x90Bx), IQ24 unless noted ------- */
#define L3_MEAS_9056      0x9056  /* [~] measurement/param */
#define L3_MEAS_905C      0x905C  /* [V] read-reg 13 value B (scaled) */
#define L3_MEAS_9066      0x9066  /* [V] read-reg 12 measurement (scaled x625) */
#define L3_MEAS_9068      0x9068  /* [~] measurement */
#define L3_MEAS_906A      0x906A  /* [V] read-reg 13 value A (scaled x125 >>17) */
#define L3_VAR_9072       0x9072  /* [~] control variable (29 refs) */
#define L3_VAR_907C       0x907C  /* [~] control variable (21 refs) */
#define L3_VERSION_ID     0x90A2  /* [V] identity block: read-reg 10 returns
                                          {(90A4<<8)|90A5, (90A2<<8)|90A3} */
#define L3_SENTINEL       0x90A6  /* [V] 0xDEADBEEF + version word (0x16A1) */

/* ---- control state (0x9180-0x91Dx) ------------------------------------- */
#define L3_CTRL_9181      0x9181  /* [~] control state/flag (24 refs) */
#define L3_CTRL_919A      0x919A  /* [~] control mode (27 refs; read-reg 12 gate) */
#define L3_CTRL_91A0      0x91A0  /* [~] control variable (27 refs) */
#define L3_MEAS_91D6      0x91D6  /* [V] read-reg 12 measurement */

/* ---- control timebase --------------------------------------------------- */
#define L3_PWM_TICK       0x9009  /* [V] ePWM4-ISR tick counter (0x3F2826) — the
                                          background-FOC timebase */

/* ---- comm state -------------------------------------------------------- */
#define L3_RESP_QUEUE     0x9440  /* [V] 8 x 13-word response ring (reliable delivery) */
#define L3_TX_RING_HEAD   0x94BB  /* [V] SCI-A TX ring write index */
#define L3_TX_RING_COUNT  0x94BD  /* [V] SCI-A TX ring fill (0..64) */
#define L3_TX_RING_BUF    0x94C0  /* [V] SCI-A TX ring (64 words) */
#define L3_DBG_RX_STATE   0x9580  /* [V] SCI-C debug-console RX buffer/state (ISR 0x3F252C) */
#define L3_RX_FRAME_BUF   0x958A  /* [V] SLIP RX decode buffer (passed to sub_3F33E6) */
#define L3_CMD_SP_958C    0x958C  /* [V] commanded setpoint A (write-reg 21) */
#define L3_CMD_SP_958E    0x958E  /* [V] commanded setpoint (write-reg 23/24) */
#define L3_CMD_SP_9590    0x9590  /* [V] commanded setpoint (write-reg 23/24) */
#define L3_SETPOINT_95A4  0x95A4  /* [V] setpoint written by write-reg 25 (x1/10) */
#define L3_RX_SLIP_STATE  0x95B8  /* [V] SLIP RX state (0 idle / 1 in-frame / 2 after-ESC) */
#define L3_RESP_SEQ       0x95B9  /* [V] response queue sequence counter */
#define L3_RX_FRAME_PTR   0x95BA  /* [V] completed RX frame pointer */

#endif /* MOTORWARE_MOTOR_STATE_H */
