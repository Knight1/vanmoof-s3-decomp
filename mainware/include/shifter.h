#ifndef MAINWARE_SHIFTER_H
#define MAINWARE_SHIFTER_H

#include <stdint.h>

/*
 * shifter.h — VanMoof S3 mainware shifter / drivetrain Modbus master
 * (src/shifter.c).
 *
 * Modbus-RTU master to the eShifter / MT-shifter module (slave 0x20) over the
 * USART3 inter-module bus (9600 baud). The master side of the flagship
 * shifterware: a frame-ring queue, the byte-level RTU transaction engine
 * (func 3 read / 6 write-single / 0x10 write-multiple, CRC-16/0xA001), the link
 * monitor, the drivetrain auto-shift control state machine, the eShifter OTA
 * firmware-update state machine, and the console `shifterstatus` dumps.
 *
 * The state machines operate on the session context (the block battery.c calls
 * G_APP_CTX @ SRAM 0x200083A8, the same block g_app_state.ctx_sub points to);
 * the shifter's reply registers are decoded into it at +0x520.. by the func-3
 * response unpacker. See docs/shifter.md.
 */

/* Enqueue a built Modbus PDU into the shifter bus ring (slave 0x20, USART3).
 * Returns 1 on queue-full/error, else 0. Used by the console s* commands. */
int modbus_shift_submit(void *frame);

/* One-time bring-up: allocate the shifter frame-ring + the queue scheduler
 * slot. Returns true once the slot is allocated. */
int smodbus_queue_timer_init(void);

/* Per-super-loop shifter service: pump the Modbus transaction SM, react to the
 * link result, then run the control + firmware-update state machines.
 * ctx == the session context (G_APP_CTX). */
void modbus_shifter_link_monitor(uint8_t *ctx);

/* Firmware-update pre-check gate: arms a shifter OTA update when the staged
 * PACK at flash 0x08010000 validates and matches the attached hardware. */
void shifter_firmware_update_step(uint8_t *ctx);

/* Shifter control-SM step accessors (g_shifter_sm+1). */
uint8_t shifter_sm_get_step(void);
void    shifter_sm_set_step_3(void);    /* step = 3  (begin bring-up)       */
void    shifter_sm_set_step_10(void);   /* step = 10 (apply gear)           */
void    shifter_sm_set_step_13(void);   /* step = 0x0D (MT calibration)     */

/* Shifter subsystem flag / update accessors (g_shifter_ctx). */
uint8_t shifter_get_active_flag(void);     /* +4 link-alive flag            */
void    shifter_update_request(uint8_t arm);/* +0 update-FSM step           */
uint8_t shifter_update_status_get(void);   /* +2 update result byte         */

/* Console `shifterstatus` scheduler dump callbacks (armed by
 * console_cmd_shifterstatus): v200 for the standard eShifter, v201 for MT. */
void shifterstatus_dump_v200(void);
void shifterstatus_dump_v201(void);

#endif
