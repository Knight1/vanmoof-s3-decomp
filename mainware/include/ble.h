#ifndef MAINWARE_BLE_H
#define MAINWARE_BLE_H

/*
 * mainware — BLE app-command surface (bridged from the CC2642 over the bus).
 *
 * Two dispatchers, both big switches on the 16-bit GATT command/char id (which
 * equals the GATT characteristic short id — see docs/ble-uuids.md):
 *   ble_cmd_dispatch          (ble.c)      — writes: app -> bike commands
 *   ble_read_request_dispatch (ble_read.c) — reads:  bike -> app telemetry
 *
 * Maps: docs/ble-commands.md; lock/alarm states: docs/state-machine.md.
 */

/* Write/command dispatcher (OEM 0x08033970). */
void ble_cmd_dispatch(unsigned int cmd, unsigned int p2, unsigned char *payload);

/* Read/telemetry dispatcher (OEM 0x08034D20). */
void ble_read_request_dispatch(unsigned int char_id, unsigned int p2,
                               unsigned char *payload);

/* Telemetry change-interval debounce (OEM 0x0803A538): keeps an 8000-tick
 * one-shot armed in *slot while any watched change bit is set, returns ready
 * once it elapses (or immediately when the bits clear). */
int ble_interval_debounce(unsigned char *slot, unsigned int unused,
                          unsigned int mask_a, unsigned int mask_b,
                          unsigned int test_a, unsigned int test_b);

#endif
