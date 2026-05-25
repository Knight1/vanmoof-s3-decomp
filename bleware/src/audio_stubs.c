/* audio_stubs.c — audio clip playback stubs.
 *
 * The audiotask (source/tasks/audiotask.c) manages BLE-audio streaming.
 * These stubs are no-ops until the audiotask is fully decoded. */

#include <stdint.h>

void audio_clip_dump_one(uint32_t index) { (void)index; }
void audio_player_play(uint32_t index) { (void)index; }
void audio_player_stop_or_pause(int action) { (void)action; }
