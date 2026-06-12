#ifndef MAINWARE_COMM_H
#define MAINWARE_COMM_H

#include <stdint.h>

/*
 * comm.h — inter-module comm-buffer registry.
 *
 * A fixed 16-slot pool (OEM SRAM 0x2000069C) records {buffer, size} for every
 * per-link RX/TX ring the firmware owns. Each registrant gets back a pointer to
 * its slot (stashed in a field of the owning link context).
 * comm_buffers_register_all wires all 16 at boot.
 *
 * OEM: comm_register_buffer 0x080317F4, comm_buffers_register_all 0x08035D0C.
 */

/* One pool slot (0xC bytes). buf == NULL marks the slot free. */
typedef struct {
    void    *buf;      /* +0x00  registered buffer base                 */
    uint16_t size;     /* +0x04  buffer length                         */
    uint16_t _r6;      /* +0x06  zeroed at registration (cursor pair?) */
    uint16_t _r8;      /* +0x08  zeroed at registration                */
    uint16_t _ra;      /* +0x0A  zeroed at registration                */
} comm_buf_slot_t;

/* Register {buf,size} into the first free pool slot; store the slot pointer in
 * *out_slot and return 1. Returns 0 if buf or out_slot is NULL, or the pool is
 * full. The first call lazily zero-inits the pool. */
int comm_register_buffer(void *buf, uint16_t size, void **out_slot);

/* Register all 16 inter-module comm buffers (called once at boot). */
void comm_buffers_register_all(void);

#endif
