#include <stdint.h>

#include "comm.h"

/*
 * comm.c — inter-module comm-buffer registry (OEM 0x080317F4 / 0x08035D0C).
 *
 * The pool lives at fixed SRAM: a one-byte "initialized" flag at 0x2000069C
 * followed by 16 comm_buf_slot_t records at 0x200006A0 (flag + pad = +4). The
 * OEM addresses the pool by absolute pointer (DAT_08031870 = 0x2000069C);
 * reproduced verbatim. The five link-context base regions wired by
 * comm_buffers_register_all are the per-port RX/TX ring contexts.
 */

#define COMM_POOL_BASE   0x2000069Cu

static volatile uint8_t *const s_pool_ready = (volatile uint8_t *)COMM_POOL_BASE;
static comm_buf_slot_t  *const s_pool       = (comm_buf_slot_t *)(COMM_POOL_BASE + 4u);

int comm_register_buffer(void *buf, uint16_t size, void **out_slot)
{
    if (out_slot == 0) {
        return 0;
    }
    *out_slot = 0;
    if (buf == 0) {
        return 0;
    }

    /* lazily zero the occupancy field of every slot on the first registration */
    if (*s_pool_ready == 0) {
        for (unsigned i = 0; i < 16u; i++) {
            s_pool[i].buf = 0;
        }
        *s_pool_ready = 1;
    }

    for (unsigned i = 0; i < 16u; i++) {
        if (s_pool[i].buf == 0) {
            s_pool[i].buf  = buf;
            s_pool[i].size = size;
            s_pool[i]._r6  = 0;
            s_pool[i]._r8  = 0;
            s_pool[i]._ra  = 0;
            *out_slot = &s_pool[i];
            return 1;
        }
    }
    return 0;   /* pool full */
}

void comm_buffers_register_all(void)
{
    uint8_t *b0 = (uint8_t *)0x2000094Cu;
    uint8_t *b1 = (uint8_t *)0x20001A44u;
    uint8_t *b2 = (uint8_t *)0x20002B3Cu;
    uint8_t *b3 = (uint8_t *)0x20003C34u;

    comm_register_buffer(b0 + 0x00c, 0x0400, (void **)(b0 + 0x004));
    comm_register_buffer((void *)0x20004DB4u, 0x1000, (void **)(b0 + 0x008));
    comm_register_buffer(b0 + 0x418, 0x0400, (void **)(b0 + 0x410));
    comm_register_buffer(b0 + 0x818, 0x0400, (void **)(b0 + 0x414));
    comm_register_buffer(b0 + 0xc24, 0x0400, (void **)(b0 + 0xc1c));
    comm_register_buffer(b1 - 0x0d4, 0x0400, (void **)(b0 + 0xc20));
    comm_register_buffer(b1 + 0x338, 0x0400, (void **)(b1 + 0x330));
    comm_register_buffer(b1 + 0x738, 0x0400, (void **)(b1 + 0x334));
    comm_register_buffer(b1 + 0xb44, 0x0800, (void **)(b1 + 0xb3c));
    comm_register_buffer(b2 + 0x24c, 0x0800, (void **)(b1 + 0xb40));
    comm_register_buffer(b2 + 0xa58, 0x0400, (void **)(b2 + 0xa50));
    comm_register_buffer(b2 + 0xe58, 0x0400, (void **)(b2 + 0xa54));
    comm_register_buffer(b3 + 0x16c, 0x0400, (void **)(b3 + 0x164));
    comm_register_buffer(b3 + 0x56c, 0x0400, (void **)(b3 + 0x168));
    comm_register_buffer(b3 + 0x978, 0x0400, (void **)(b3 + 0x970));
    comm_register_buffer(b3 + 0xd78, 0x0400, (void **)(b3 + 0x974));
}
