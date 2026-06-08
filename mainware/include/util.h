#ifndef MAINWARE_UTIL_H
#define MAINWARE_UTIL_H

#include <stdint.h>

/* Packed-BCD <-> binary byte converters (RTC / time-field helpers). */

/* Decode one packed-BCD byte (high nibble = tens, low nibble = units) to its
 * binary value 0..99+. OEM bcd_to_bin at 0x0802311C. */
uint8_t bcd_to_bin(uint8_t bcd);

/* Encode a binary byte (0..99) to packed BCD. OEM bin_to_bcd at 0x08022F2E. */
uint8_t bin_to_bcd(uint8_t bin);

/* Generic byte ring/FIFO descriptor — the dequeue primitive behind ssp_rx_byte
 * and ~15 other consumers. Field offsets match the OEM layout. */
typedef struct {
    uint8_t  *data;   /* +0x00  backing buffer */
    uint16_t  cap;    /* +0x04  capacity (tail wraps to 0 at cap) */
    int16_t   count;  /* +0x06  bytes currently queued */
    uint16_t  head;   /* +0x08  write index (untouched by the getter) */
    uint16_t  tail;   /* +0x0A  read index */
} ringbuf_t;

/* Push one byte. Returns 1 on success, 0 if rb is NULL or the buffer is full.
 * OEM ringbuf_push_byte at 0x08031874 (push twin of the getter; behind
 * uart_send_byte and others). */
uint32_t ringbuf_push_byte(ringbuf_t *rb, uint8_t b);

/* Pop one byte into *out. Returns 1 on success, 0 if rb/out is NULL or the
 * buffer is empty. OEM ringbuf_get_byte at 0x080318AE. */
uint32_t ringbuf_get_byte(ringbuf_t *rb, uint8_t *out);

/* Linear-map v in [a,b] onto [c,d], clamping first (unsigned clamp + divide).
 * OEM telemetry_map_clamp 0x0803a588 (BLE 0x5541 SoC scale, speed bar). */
uint8_t telemetry_map_clamp(uint8_t v, uint32_t a, uint32_t b, int32_t c, int32_t d);

#endif
