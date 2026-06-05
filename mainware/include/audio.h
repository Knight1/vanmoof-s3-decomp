#ifndef MAINWARE_AUDIO_H
#define MAINWARE_AUDIO_H

#include <stdint.h>

/* Apply an audio-amp volume level with a supply-voltage (brownout) limiter and
 * push it to the amp over the inter-module bus. When the supply reads below
 * ~25000 (its scaled units) the level is clamped to 0x14 and the brownout line
 * (PD13) is asserted. Returns the bus-TX status (0 = ok). OEM
 * amp_volume_brownout_apply at 0x080391B8 — the helper the `volume` console
 * commands call after parsing a level. */
uint32_t amp_volume_brownout_apply(uint8_t *level);

#endif
