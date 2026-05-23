/* ti_rtos_heap.c — first-fit dual-pool block allocator used by the
 * bleware application (a thin reimplementation of the TI-RTOS / ICall
 * heap interface).
 *
 * OEM symbols:
 *   `monitor_alloc` @ 0x00013470  (also reachable through the local
 *                                   thunk_FUN_00013470 at 0x000276AE)
 *   `monitor_free`  @ 0x00021B88
 *
 * Heap state struct (singleton at RAM 0x20004E0C, held in flash literal
 * pool at 0x0001355C / 0x00021BC8 — both literal slots point at the
 * same struct):
 *
 *   +0x02 u16  alloc_failure_count
 *   +0x10 u32  last_cs_key            (saved across the alloc body —
 *                                       used by the panic path)
 *   +0x14 u32  small_walk_start       (start the free-list walk here
 *                                       when needed <= 16 B)
 *   +0x18 u32  large_walk_start       (start here when needed > 16 B)
 *   +0x1C u32  peak_block_count
 *   +0x20 i32  block_count            (total alloc'd + free; +1 on
 *                                       split alloc, -1 on coalesce)
 *   +0x24 i32  free_block_count
 *   +0x28 u32  bytes_in_use
 *   +0x2C u32  peak_bytes_in_use
 *   +0x30 u32  high_water_offset
 *   +0x34 i32  free_bytes
 *   +0x3C u32  heap_base              (used by the offset bookkeeping)
 *
 * Free-list format: a flat array of blocks. Each block starts with a
 * u32 header (`size` in low 31 bits, bit 31 = in-use). Free blocks are
 * walked by `current = current + (size >> 0)` stepping. A header of
 * zero marks the end of the heap. There are no explicit prev/next
 * pointers — coalescing happens implicitly when two adjacent walked
 * blocks are both free.
 *
 * Returned pointer is one word past the header — i.e., `block + 4`.
 */

#include <stddef.h>
#include <stdint.h>

#include "bleware.h"

/* Combined Hwi+Task disable / restore. */
extern uint32_t icall_cs_enter(void);   /* FUN_00024DB8 */
extern void     icall_cs_exit(uint32_t key); /* FUN_000266B2 */

struct heap_state {
    uint16_t  reserved0;
    uint16_t  alloc_failure_count;       /* +0x02 */
    uint32_t  reserved4[3];              /* +0x04..0x0F */
    uint32_t  last_cs_key;               /* +0x10 */
    uint32_t *small_walk_start;          /* +0x14 */
    uint32_t *large_walk_start;          /* +0x18 */
    uint32_t  peak_block_count;          /* +0x1C */
    int32_t   block_count;               /* +0x20 */
    int32_t   free_block_count;          /* +0x24 */
    uint32_t  bytes_in_use;              /* +0x28 */
    uint32_t  peak_bytes_in_use;         /* +0x2C */
    uint32_t  high_water_offset;         /* +0x30 */
    int32_t   free_bytes;                /* +0x34 */
    uint32_t  reserved38;                /* +0x38 */
    uint32_t  heap_base;                 /* +0x3C */
};

extern struct heap_state * const g_heap_state;   /* flash 0x0001355C / 0x00021BC8 */

#define IN_USE_BIT  0x80000000u

void *monitor_alloc(unsigned int size)
{
    struct heap_state *h = g_heap_state;
    uint32_t  needed;
    uint32_t  cs_key;
    uint32_t *cur;
    uint32_t *chosen = NULL;
    uint32_t  cur_size;
    int       coalescing = 0;

    /* Round (size + 4-byte header) up to a multiple of 4. */
    needed = size + 4u;
    if (needed & 3u) {
        needed = (needed & ~3u) + 4u;
    }

    cs_key = icall_cs_enter();
    h->last_cs_key = cs_key;

    cur = (needed > 0x10u) ? h->large_walk_start : h->small_walk_start;
    cur_size = *cur;

    while (cur_size != 0u) {
        if ((int32_t)cur_size < 0) {
            /* In-use block — skip past it. */
            cur_size &= ~IN_USE_BIT;
            coalescing = 0;
        } else if (coalescing) {
            /* Adjacent free block: merge into `chosen`. */
            h->block_count--;
            h->free_block_count--;
            *chosen = *chosen + *cur;
            if (needed <= *chosen) {
                cur_size = *chosen;
                goto take;
            }
        } else {
            chosen = cur;
            cur_size = *cur;
            if (needed <= cur_size) {
                goto take;
            }
            coalescing = 1;
        }
        cur = (uint32_t *)((uint8_t *)cur + cur_size);
        cur_size = *cur;
    }
    chosen = NULL;

take:
    if (chosen == NULL) {
        h->alloc_failure_count++;
    } else {
        uint32_t base = h->heap_base;
        uint32_t bytes_in_use;
        uint32_t new_high;
        uint32_t leftover = cur_size - needed;

        if (leftover < 4u) {
            /* Take the whole block — no split. */
            h->bytes_in_use     += *chosen;
            h->free_block_count--;
            cur_size             = *chosen;
            new_high             = (uint32_t)chosen + (cur_size - base);
            if (new_high > h->high_water_offset) {
                h->high_water_offset = new_high;
                cur_size = *chosen;     /* OEM reloads here; preserve. */
            }
            h->free_bytes -= cur_size;
            *chosen       |= IN_USE_BIT;
            bytes_in_use   = h->bytes_in_use;
        } else {
            /* Split: write the leftover header, mark chosen in-use. */
            *(uint32_t *)((uint8_t *)chosen + needed) = leftover;
            *chosen = needed | IN_USE_BIT;
            h->block_count++;
            if ((uint32_t)h->block_count > h->peak_block_count) {
                h->peak_block_count = (uint32_t)h->block_count;
            }
            h->bytes_in_use += needed;
            new_high         = (uint32_t)chosen + (needed - base);
            if (new_high > h->high_water_offset) {
                h->high_water_offset = new_high;
            }
            h->free_bytes -= (int32_t)needed;
            bytes_in_use   = h->bytes_in_use;
        }
        if (bytes_in_use > h->peak_bytes_in_use) {
            h->peak_bytes_in_use = bytes_in_use;
        }
        chosen = (uint32_t *)((uint8_t *)chosen + 4);
    }

    icall_cs_exit(cs_key);
    return chosen;
}

void monitor_free(void *p)
{
    struct heap_state *h = g_heap_state;
    uint32_t           cs_key;
    uint32_t          *hdr;
    uint32_t           sz;

    cs_key = icall_cs_enter();
    h->last_cs_key = cs_key;

    hdr  = (uint32_t *)((uint8_t *)p - 4);
    sz   = *hdr & ~IN_USE_BIT;
    *hdr = sz;

    h->free_block_count++;
    h->bytes_in_use -= sz;
    h->free_bytes   += (int32_t)*hdr;

    if ((uint32_t)hdr < (uint32_t)h->small_walk_start) {
        h->small_walk_start = hdr;
    }

    icall_cs_exit(cs_key);
}
