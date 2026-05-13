#ifndef SHIFTER_PROTOCOL_H
#define SHIFTER_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "shifter.h"

/* Frame format (preliminary):
 *   0xA5  SOF
 *   ID    message id
 *   LEN   payload length (0..16)
 *   PL[]  payload bytes
 *   CRC8  Dallas/Maxim CRC8 over ID, LEN, PL[]
 */
#define PROTO_SOF              0xA5u
#define PROTO_MAX_PAYLOAD      16u
#define PROTO_FRAME_OVERHEAD   4u

typedef enum {
    MSG_PING        = 0x01u,
    MSG_PONG        = 0x02u,
    MSG_SHIFT       = 0x10u,    /* payload[0] = target gear */
    MSG_STATE       = 0x11u,    /* payload[0] = shifter_state_t, [1] = gear */
    MSG_SET_PARAM   = 0x20u,
    MSG_GET_PARAM   = 0x21u,
    MSG_VERSION     = 0x30u,
    MSG_FAULT       = 0xF0u,
} proto_msg_id_t;

typedef struct {
    uint8_t id;
    uint8_t len;
    uint8_t payload[PROTO_MAX_PAYLOAD];
} proto_frame_t;

void   protocol_init(void);
void   protocol_poll(void);

bool   protocol_send(uint8_t id, const uint8_t *payload, uint8_t len);
bool   protocol_recv(proto_frame_t *out);   /* non-blocking */

uint8_t proto_crc8(const uint8_t *data, size_t len);

#endif /* SHIFTER_PROTOCOL_H */
