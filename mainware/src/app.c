#include <stdint.h>

#include "app.h"

/* Subsystem firmware-update mode (OEM: byte at SRAM 0x20000076 — offset +1 of
 * a small update-control block at 0x20000075). The idle/resting value is 2; a
 * request only takes hold from idle. */
uint8_t g_update_mode;

/* Broadcast/announce dirty-flags block (OEM at SRAM 0x20000288). The two
 * channel request flags live at +5 and +6; the loop clears them after it has
 * broadcast the corresponding value. */
struct announce_state {
    uint8_t _pad0[5];      /* +0x00..+0x04 */
    uint8_t ch_dirty[2];   /* +0x05 = channel 0, +0x06 = channel 1 */
};
struct announce_state g_announce;

void update_mode_request(uint8_t mode)
{
    if (g_update_mode == 2) {
        g_update_mode = mode;
    }
}

void announce_mark(int channel)
{
    if (channel == 0) {
        g_announce.ch_dirty[0] = 1;
        return;
    }
    if (channel != 1) {
        return;
    }
    g_announce.ch_dirty[1] = 1;
}

/* Guarded FSM state-byte setter (OEM maybe_set_state_if_unlocked, 0x08029B88).
 * The state byte is at +4 of a small state object at SRAM 0x20000029 (accessed
 * by absolute address, matching the OEM literal). States 6 and 7 are
 * sticky/locked and cannot be overwritten through this path. */
void maybe_set_state_if_unlocked(uint8_t new_state)
{
    volatile uint8_t *state = (volatile uint8_t *)(0x20000029u + 4u);

    if ((uint8_t)(*state - 6u) > 1u) {   /* current state is neither 6 nor 7 */
        *state = new_state;
    }
}

/* Read the bike FSM state byte (OEM maybe_get_bike_state, 0x08029BA0) — the
 * getter twin of maybe_set_state_if_unlocked (both on SRAM 0x2000002D). */
uint8_t maybe_get_bike_state(void)
{
    return *(volatile uint8_t *)(0x20000029u + 4u);
}

/* Latch a requested value and raise its pending flag (OEM maybe_set_pending_request,
 * 0x0803B738). The flag is only ever set here; the servicing code clears it. */
struct request_ctx {
    uint8_t _pad0[0x130];
    uint8_t pending;      /* +0x130 */
    uint8_t _pad1[3];     /* +0x131..+0x133 */
    int32_t value;        /* +0x134 */
};
extern struct request_ctx g_request_ctx;   /* SRAM 0x20008230 */

void maybe_set_pending_request(int32_t value)
{
    g_request_ctx.value = value;
    if (value != 0) {
        g_request_ctx.pending = 1;
    }
}

/* Resolve a channel's status (0..3, from three per-channel bitmasks in the app
 * context) and forward (id, status) to the notify emitter — drives the display
 * + enqueues a BLE notify. OEM channel_notify_with_status at 0x0802A2F0. */
extern uint32_t channel_resolve_status(uint32_t channel_id);           /* 0x0802A2B0 */
extern void     channel_notify_emit(uint32_t channel_id, int status);  /* 0x0802A064 */

void channel_notify_with_status(uint32_t channel_id)
{
    int status = (int)channel_resolve_status(channel_id);
    channel_notify_emit(channel_id, status);
}
