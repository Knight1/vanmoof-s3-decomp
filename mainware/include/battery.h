#ifndef MAINWARE_BATTERY_H
#define MAINWARE_BATTERY_H

#include <stdint.h>

/*
 * battery.h — mainware battery / BMS Modbus driver (src/battery.c).
 *
 * Modbus-RTU master to the battery BMS (slave 0xAA, func 3 read / func 6 write)
 * over the inter-module bus. Telemetry registers are unpacked into the app
 * context (reg N -> *(uint16_t*)(g_app_ctx + 0x3F2 + N*2)). See docs/battery.md.
 *
 * Context: g_bat_modbus_ctx @ SRAM 0x20006E90 (frame at +0xE7C = 0x20007D0C).
 */

/* Build + submit a func-3 (read holding registers) PDU to BMS slave 0xAA. */
void bms_modbus_read(uint16_t reg, uint8_t count);

/* Build + submit a func-6 (write single register) PDU to BMS slave 0xAA. */
void bms_modbus_write(uint16_t reg, uint32_t value);

/* Write 1 to BMS register 8, then read it back (poll/confirm). */
void bms_write_reg8_and_poll(void);

/* Kick the periodic BMS telemetry burst (read regs 0..0x30 and 0x47..0x56). */
void battery_request_telemetry(void);

/* Per-super-loop service: pump the battery Modbus transaction SM, then run the
 * telemetry/charge/state processors. */
void modbus_bat_service_step(uint32_t a, uint32_t b, uint32_t c, uint32_t d);

/* Battery presence detect (GPIO PC10): advance the substate when the pack is in. */
void battery_on_detect_step(int force);

/* Advance the battery substate 0x0C -> 9 (shipping LiPo-charge transition). */
void battery_substate_advance(void);

/* Read the battery telemetry FSM state byte (G_BAT_STATE+3) — polled by
 * status_process (OEM battery_telemetry_state_get, 0x0803E5F0). */
uint8_t battery_telemetry_state_get(void);

/* batteryware (BMS firmware) update hooks. */
void    batteryware_update_arm(void);
void    batteryware_update_set_pending(void);
uint8_t batteryware_update_status_get(void);

/* Inter-module "Manchester" announce decoder + staging. */
const char *wst_status_to_string(uint32_t wst);                       /* 0x080330E4 */
void        manchester_announce_decode(const uint8_t *msg, int len, uint8_t *cache); /* 0x08043B28 */
uint16_t    staged_msg_crc16(const uint8_t *buf, int len);            /* 0x08043AF0 */
void        tim10_announce_period_cb(void *htim);                     /* 0x08043DE0 */
void        exti4_app_hook(void);                                     /* 0x08043CEC */

#endif
