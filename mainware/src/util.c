#include <stdint.h>

#include "util.h"

/* Packed-BCD <-> binary byte converters. The OEM uses these for the RTC /
 * clock fields; both are pure and were transcribed directly from the OEM
 * arithmetic (0x0802311C / 0x08022F2E). */

uint8_t bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)((bcd & 0x0Fu) + (bcd >> 4) * 10u);
}

uint8_t bin_to_bcd(uint8_t bin)
{
    uint8_t tens = 0;

    while (bin > 9u) {
        bin = (uint8_t)(bin - 10u);
        tens++;
    }
    return (uint8_t)(bin | (uint8_t)(tens << 4));
}

/* Generic byte FIFO enqueue (OEM ringbuf_push_byte, 0x08031874). The push twin
 * of ringbuf_get_byte: advance `head` and store it back, write the byte, wrap
 * at `cap`, bump `count`. Full when count has reached cap. Returns 1 on success,
 * 0 if rb is NULL or the buffer is full. */
uint32_t ringbuf_push_byte(ringbuf_t *rb, uint8_t b)
{
    uint16_t head;

    if (rb == 0) {
        return 0;
    }
    if ((uint16_t)rb->count >= rb->cap) {
        return 0;                          /* full */
    }

    head = rb->head;
    rb->head = (uint16_t)(head + 1);
    rb->data[head] = b;

    if (rb->head >= rb->cap) {
        rb->head = 0;
    }
    rb->count = (int16_t)(rb->count + 1);
    return 1;
}

/* Generic byte FIFO dequeue (OEM 0x080318AE). The OEM advances `tail` and
 * stores it back before reading the byte and wrap-checking — preserved here. */
uint32_t ringbuf_get_byte(ringbuf_t *rb, uint8_t *out)
{
    uint16_t tail;

    if (rb == 0 || out == 0 || rb->count == 0) {
        return 0;
    }

    tail = rb->tail;
    rb->tail = (uint16_t)(tail + 1);
    *out = rb->data[tail];

    if (rb->tail >= rb->cap) {
        rb->tail = 0;
    }
    rb->count = (int16_t)(rb->count - 1);
    return 1;
}

/* Bytes of free space left in the ring (cap - count); 0 if rb is NULL. OEM
 * ringbuf_free_space at 0x080318EC. console_printf busy-waits on this reaching
 * cap (the FIFO fully drained) to push the next byte synchronously. */
uint32_t ringbuf_free_space(const ringbuf_t *rb)
{
    if (rb == 0) {
        return 0;
    }
    return (uint32_t)((int16_t)rb->cap - rb->count);
}

/* Linear-map v from input range [a,b] onto output range [c,d], clamping v to
 * [a,b] first (OEM telemetry_map_clamp, 0x0803a588). Used by the BLE 0x5541 SoC
 * scale (telemetry_map_clamp(soc, 0, 0x61, 0, 100)) and matrix_draw_speed. The
 * clamp comparisons and the divide are UNSIGNED in the OEM (bcs/bls/udiv); only
 * (d-c) and the final + c are signed. Pure leaf — no globals, no callees. */
uint8_t telemetry_map_clamp(uint8_t v, uint32_t a, uint32_t b, int32_t c, int32_t d)
{
    uint32_t x = v;

    if (x < a) {
        x = a;
    }
    if (x > b) {
        x = b;
    }
    return (uint8_t)((uint32_t)((d - c) * (int32_t)(x - a)) / (b - a) + (uint32_t)c);
}
