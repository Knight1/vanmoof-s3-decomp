#ifndef MAINWARE_SSP_H
#define MAINWARE_SSP_H

#include <stdint.h>

/* BLE / inter-module "SSP" transport. The bleware MCU bridges the phone-app BLE
 * link onto the inter-module bus; mainware receives those messages here. The
 * wire format is SLIP framing (RFC 1055 bytes) with a CRC-16 trailer; the
 * de-framed message is then dispatched by type (and, for data messages, by
 * command id into ble_cmd_dispatch). This is the entry path for OTA firmware
 * packets as well as every BLE app command (lock/unlock, region/speed, etc.). */

/* SLIP control byte values (RFC 1055). */
#define SLIP_END      0xC0u   /* frame delimiter */
#define SLIP_ESC      0xDBu   /* escape */
#define SLIP_ESC_END  0xDCu   /* escaped 0xC0 */
#define SLIP_ESC_ESC  0xDDu   /* escaped 0xDB */

/* slip_rx_packet de-framer states (the byte at g_ble_ssp.slip_state). */
#define SLIP_IDLE      0u   /* waiting for a frame-start 0xC0 */
#define SLIP_IN_FRAME  1u   /* collecting frame bytes */
#define SLIP_AFTER_ESC 2u   /* previous byte was 0xDB */

/* BLE message types (byte [1] of a de-framed message). */
#define BLE_MSG_COMMAND 5u   /* control command (id in byte [2]) */
#define BLE_MSG_PREPARE 6u   /* prepare / length announce */
#define BLE_MSG_DATA    7u   /* data: command id [3..4] + payload [7..] */

/* Per-receiver SLIP reassembly control (the OEM struct at SRAM 0x200000F4). */
struct slip_rx {
    uint8_t *buf;   /* reassembly buffer */
    uint32_t cap;   /* buffer capacity */
    uint32_t len;   /* bytes collected so far */
};

/* Feed one received byte into the SLIP de-framer. Returns 0 when a complete,
 * CRC-valid frame is ready in `ctrl->buf` (length `ctrl->len`), 2 on a CRC or
 * framing error, 1 while still receiving. OEM slip_rx_packet at 0x0803F5A4. */
char slip_rx_packet(struct slip_rx *ctrl);

/* Pull + dispatch one BLE/SSP message from the bus (called each super-loop
 * iteration). Returns 1 if a message was processed. OEM ble_ssp_dispatch at
 * 0x0803F8FC. */
int ble_ssp_dispatch(void);

/* Enqueue an outbound packet (cmd + payload) into the 128-slot BLE/SSP TX
 * queue. Returns the slot index 0..0x7F, 0xFD if len > 0x100, 0xFF if full.
 * OEM ssp_ble_enqueue_tx_packet at 0x0803F9CC. */
uint8_t ssp_ble_enqueue_tx_packet(uint16_t cmd, uint16_t len,
                                  const void *payload, uint8_t flags);

/* SLIP-frame + transmit a payload with a CRC-16 trailer (the TX counterpart of
 * slip_rx_packet). Returns 0 on success, 2 on TX error. OEM slip_send_frame at
 * 0x0803F4F0. */
uint32_t slip_send_frame(const uint8_t *buf, int len);

#endif
