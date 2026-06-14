/* xs3_app.c — top-level application glue (audio-playback dispatch).
 *
 * OEM source file: source/xs3_app.c (path string embedded at flash
 * 0x0001BAA4). The only function carved out of this TU so far is the
 * audio-playback entry that the monitor `audio_play` command and the
 * audiotask use to start a clip.
 *
 * Two OEM symbols share one body:
 *
 *   audio_player_play       @ 0x000275B8  (8-byte veneer)
 *   play_sound_repeatedly   @ 0x0001BA6C  (136-byte body)
 *
 * The veneer at 0x275B8 is `uxtb r0,r0; movs r1,#1; b.w 0x1ba6c`: it
 * narrows the index argument to a byte and hard-wires the repeat count
 * to 1, then tail-calls the real body. Ghidra folds the two into a
 * single function; we keep them split so the public entry matches the
 * caller-visible symbol (`audio_player_play`, referenced from
 * src/monitor/cmd_audio.c) while the body keeps the OEM `__func__`
 * name ("play_sound_repeatedly", stored at flash 0x0002A568 and passed
 * to the error log).
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* Location-aware monitor logger — same prototype used across the codebase
 * (see src/gatt_write.c). `fn` is the OEM `__func__` string; `level` is a
 * severity byte. OEM @ 0x00006D90 (FUN_00006D90). */
extern void monitor_log(const char *file, int line, const char *fn,
                        int level, const char *fmt, ...);

/* Audio-clip metadata helper (suspected audio_get_clip_duration).
 * Parses the WAV header for `index` and returns the clip duration in ms,
 * or 0xFFFFFFFF if the index has no valid clip. We only use it here as a
 * validity probe. OEM @ 0x00022FE8 (FUN_00022FE8). */
extern uint32_t audio_get_clip_duration(uint32_t index);  /* src/audiotask.c */

/* Audiotask trigger. Called with 0 to (re)kick playback of the clip whose
 * index/repeat fields were just written into the audio-player state block.
 * OEM @ 0x000237C8 (FUN_000237C8). */
extern void audiotask_kick(int action);

/* Audio-player state block at RAM 0x20005670 (DAT_0001BAE0). Only two
 * fields are touched on the play path:
 *   +0x04  uint8_t  clip index to play
 *   +0x14  uint32_t remaining repeat count
 * The audiotask consumes both; writing +0x14 = repeat and +0x04 = index
 * then kicking the task starts the clip. */
#define AUDIO_PLAYER_STATE_BASE   0x20005670u
#define AUDIO_PLAYER_OFF_INDEX    0x04u
#define AUDIO_PLAYER_OFF_REPEAT   0x14u

static const char k_src_file[] = "source/xs3_app.c";

/* Start playback of audio clip `index`, looping it `repeat` times.
 *
 * Probes the clip via audio_clip_duration_ms(): a 0xFFFFFFFF result means
 * there is no valid WAV at that index, in which case we log the failure
 * and return without touching the player state. On a hit we stash the
 * repeat count and index into the audio-player state block and kick the
 * audiotask.
 *
 * OEM @ 0x0001BA6C. The OEM `__func__` string "play_sound_repeatedly"
 * (flash 0x0002A568) is passed verbatim to monitor_log on the error path,
 * so we reproduce that name here. */
static void play_sound_repeatedly(uint8_t index, uint32_t repeat)
{
    if (audio_get_clip_duration(index) == 0xFFFFFFFFu) {
        monitor_log(k_src_file, 0x12d, "play_sound_repeatedly", 1,
                    "Couldn't find audio with index %d", index);
        return;
    }

    volatile uint32_t *state = (volatile uint32_t *)AUDIO_PLAYER_STATE_BASE;
    state[AUDIO_PLAYER_OFF_REPEAT / 4] = repeat;
    ((volatile uint8_t *)AUDIO_PLAYER_STATE_BASE)[AUDIO_PLAYER_OFF_INDEX] =
        index;

    audiotask_kick(0);
}

/* Public entry: play clip `index` exactly once.
 *
 * OEM @ 0x000275B8 — an 8-byte veneer (`uxtb r0,r0; movs r1,#1;
 * b.w 0x1ba6c`) that narrows the index to a byte and forwards to
 * play_sound_repeatedly with repeat count 1. The `(uint8_t)` cast below
 * reproduces the `uxtb`. Called from cmd_audio_play
 * (src/monitor/cmd_audio.c) and the audiotask.
 *
 * The parameter is `uint32_t` to match the bleware.h declaration (and the
 * weak stub in src/audio_stubs.c it supersedes); only the low byte is
 * used, exactly as the OEM veneer guarantees. */
void audio_player_play(uint32_t index)
{
    play_sound_repeatedly((uint8_t)index, 1u);
}
