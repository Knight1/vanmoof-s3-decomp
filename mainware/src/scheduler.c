#include <stdint.h>

#include "panic.h"
#include "scheduler.h"

/* The Muco 48-slot one-shot scheduler. The OEM pins this table at SRAM
 * 0x200004C0 through literal pools; we let the linker place it — only the
 * table's effect is observable, not its address (behaviour-equivalent). */
struct scheduler g_scheduler;

/* Source path the scheduler hands to muco_assert_fail when it runs out of
 * slots (OEM rodata 0x08050CBC). The Muco timer module is "src/time.c". */
static const char k_sched_src_file[] = "src/time.c";

int scheduler_init(void)
{
    /* Mark every slot free. The OEM clears the two 6-byte bitmaps (and the
     * first callback word, with the same run of stores); we mirror the effect. */
    for (unsigned i = 0; i < SCHED_BITMAP_BYTES; i++) {
        g_scheduler.allocated[i] = 0;
        g_scheduler.armed[i] = 0;
    }
    g_scheduler.callbacks[0] = 0;
    return 1;
}

void scheduler_tick(void)
{
    for (uint8_t slot = 0; slot < SCHED_SLOTS; slot = (uint8_t)(slot + 1)) {
        if (((g_scheduler.armed[slot >> 3] >> (slot & 7)) & 1u) == 0) {
            continue;
        }
        if (g_scheduler.counters[slot] != 0) {
            g_scheduler.counters[slot]--;
        }
        if (g_scheduler.counters[slot] == 1) {
            sched_cb_t cb = g_scheduler.callbacks[slot];
            if (cb != 0) {
                cb();
            }
        }
    }
}

uint8_t scheduler_alloc(void)
{
    uint8_t byte_idx;

    for (byte_idx = 0;
         byte_idx < SCHED_BITMAP_BYTES && g_scheduler.allocated[byte_idx] == 0xFF;
         byte_idx = (uint8_t)(byte_idx + 1)) {
    }

    if (byte_idx < SCHED_BITMAP_BYTES) {
        uint8_t bit_idx = 0;
        uint8_t mask = 1;

        while (bit_idx < 8 && (g_scheduler.allocated[byte_idx] & mask) != 0) {
            bit_idx = (uint8_t)(bit_idx + 1);
            mask = (uint8_t)(mask << 1);
        }

        uint8_t slot = (uint8_t)(bit_idx + (byte_idx << 3));
        g_scheduler.counters[slot] = 0;
        g_scheduler.callbacks[slot] = 0;
        g_scheduler.allocated[byte_idx] |= mask;

        /* slot is 0..47, so it can never equal the 0xFA sentinel — the OEM
         * keeps this guard anyway and falls through to the assert if it ever
         * trips. */
        if (slot != SCHED_SLOT_NONE) {
            return slot;
        }
    }

    /* Every slot is taken (or the impossible sentinel collision). The OEM
     * treats running out of timers as a fatal runtime error. */
    muco_assert_fail(k_sched_src_file, 0x7F);
}

int scheduler_release(uint8_t *slot_ref)
{
    uint8_t slot = *slot_ref;

    if (slot >= SCHED_SLOTS) {
        *slot_ref = SCHED_SLOT_NONE;
        return 0;
    }

    g_scheduler.armed[slot >> 3]     &= (uint8_t)~(1u << (slot & 7));
    g_scheduler.allocated[slot >> 3] &= (uint8_t)~(1u << (slot & 7));
    g_scheduler.counters[slot]  = 0;
    g_scheduler.callbacks[slot] = 0;
    *slot_ref = SCHED_SLOT_NONE;
    return 1;
}

int scheduler_start(uint8_t slot, uint32_t ticks, sched_cb_t cb)
{
    if (slot >= SCHED_SLOTS) {
        return 0;
    }

    g_scheduler.counters[slot]  = ticks;
    g_scheduler.callbacks[slot] = cb;
    g_scheduler.armed[slot >> 3] |= (uint8_t)(1u << (slot & 7));
    return 1;
}

int scheduler_slot_is_idle(uint8_t slot)
{
    if (slot >= SCHED_SLOTS) {
        return 0;
    }
    return g_scheduler.counters[slot] == 0;
}

/* Timer/task name-register hook (OEM scheduler_set_timer_name, 0x08029B70).
 * Compiled out to a `bx lr` stub in this release build; the ~49 callers each
 * follow it with scheduler_start(slot, ticks, NULL). The name (a rodata literal
 * like "ssp_show_tmr") would be recorded for tracing in a debug build. */
void scheduler_set_timer_name(uint8_t slot, uint32_t ticks, const char *name)
{
    (void)slot;
    (void)ticks;
    (void)name;
}
