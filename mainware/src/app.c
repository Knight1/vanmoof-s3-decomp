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
