/* ssp_relay.c — tiny wrappers that publish an integer-valued command
 * onto the inter-module SSP/Modbus bus.
 *
 * Both helpers follow the same shape used by the GATT-write central
 * dispatcher when it forwards a BLE-side change to a peer module: if
 * the bike currently has any authenticated BLE connection, blink the
 * activity LED (so an attached display can show "talking to phone"),
 * then build a little-endian byte buffer of the integer payload and
 * post it as a single SSP frame.
 *
 *   `ssp_relay_u32` @ 0x00021884  — 4-byte LE payload (e.g. svc 0x5500
 *                                    op 0x02 carrying a 24-bit value
 *                                    on `cmd 0x5503`, svc 0x5560 op
 *                                    0x06 carrying a 32-bit epoch on
 *                                    `cmd 0x5567`)
 *   `ssp_relay_u16` @ 0x00023204  — 2-byte LE payload (svc 0x5580 op
 *                                    0x03 → `cmd 0x5584`)
 *
 * Neither helper inspects the publish return code; per-module reply
 * handling happens out-of-band via the SSP RX path.
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

extern int  ble_authenticated_connection_count(void);
extern void ble_activity_led_pulse(void);

struct ssp_master_state;
extern struct ssp_master_state g_ssp_master;
extern int  ssp_queue_publish_frame(struct ssp_master_state *master,
                                    uint16_t                 cmd_id,
                                    const void              *payload,
                                    uint32_t                 payload_len,
                                    uint32_t                 ctx1,
                                    uint32_t                 ctx2,
                                    uint8_t                 *seq_out);

static void pulse_led_if_any_conn(void)
{
    if (ble_authenticated_connection_count() != 0) {
        ble_activity_led_pulse();
    }
}

void ssp_relay_u32(uint16_t cmd_id, uint32_t value)
{
    uint8_t le[4];

    pulse_led_if_any_conn();
    le[0] = (uint8_t)(value      );
    le[1] = (uint8_t)(value >>  8);
    le[2] = (uint8_t)(value >> 16);
    le[3] = (uint8_t)(value >> 24);
    ssp_queue_publish_frame(&g_ssp_master, cmd_id, le, 4, 0, 0, NULL);
}

void ssp_relay_u16(uint16_t cmd_id, uint16_t value)
{
    uint8_t le[2];

    pulse_led_if_any_conn();
    le[0] = (uint8_t)(value     );
    le[1] = (uint8_t)(value >> 8);
    ssp_queue_publish_frame(&g_ssp_master, cmd_id, le, 2, 0, 0, NULL);
}
