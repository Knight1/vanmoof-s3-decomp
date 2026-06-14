/* audiotask.c — audio-clip discovery / WAV-header parsing.
 *
 * OEM source: source/tasks/audiotask.c
 *
 * Audio clips are stored in the external SPI NOR flash, one per 512 KiB
 * slot:
 *
 *     clip_base(index) = 0x200000 + index * 0x80000
 *
 * Each clip begins with a 0x1C-byte VanMoof container header whose first
 * eight bytes are the magic "VM_SOUND", immediately followed by a
 * standard 44-byte canonical-PCM RIFF/WAVE header. This module parses and
 * validates that header (`audio_wav_open`), dumps the parsed fields to the
 * monitor console (`audio_clip_dump_one`), and derives a clip's playback
 * duration in milliseconds (`audio_get_clip_duration`).
 *
 * bleware itself does not decode/render audio — that is the motor MCU's
 * job. These routines only inspect the on-flash header.
 *
 * OEM functions:
 *   audio_wav_open            @ 0x0000B164  (334 B; OEM __func__ "f_open")
 *   audio_clip_dump_one       @ 0x0000D5CC  (178 B; OEM __func__ "audio_dump")
 *   audio_get_clip_duration   @ 0x00022FE8  (60 B)
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* Location-aware console logger (FUN_00006D90). Declared in monitor.h,
 * re-declared here to avoid pulling the whole monitor surface into this
 * audio TU. Signature/argument order matches the OEM call frames. */
extern void monitor_log(const char *file, int line, const char *fn, int level,
                        const char *fmt, ...);

/* External-SPI-flash driver (src/extflash.c). `audio_wav_open` uses the
 * no-argument open form (the OEM passes no argument — the driver ignores
 * it and returns 1 when the bus is up). */
extern int  extflash_open(void);             /* FUN_000152FC */
extern void extflash_close(void);            /* FUN_0002758E (empty bx lr) */
extern int  extflash_read(uint32_t addr, uint32_t len, void *dst); /* FUN_0001C5A4 */

/* Byte-wise compare, memcmp semantics: returns 0 when the first `n`
 * bytes match, otherwise the signed difference of the first mismatching
 * byte pair. OEM FUN_00025490 (also used by pakfs magic checks). */
extern int  memcmp(const void *a, const void *b, unsigned int n);

#define AUDIO_REGION_BASE    0x00200000u   /* clip slot 0 base in ext-flash */
#define AUDIO_SLOT_STRIDE    0x00080000u   /* 512 KiB per clip slot         */

#define AUDIO_CONTAINER_HDR  0x1Cu         /* "VM_SOUND" + padding          */
#define AUDIO_WAV_HDR        0x2Cu         /* canonical 44-byte RIFF header  */

#define LOG_LEVEL_HELP       8             /* monitor "help/info" channel    */
#define LOG_LEVEL_ERR        2             /* monitor "error" channel        */

static const char K_FILE[] = "source/tasks/audiotask.c";

/* Magic / chunk-tag literals. The OEM stores these as 4-/8-byte rodata
 * blobs that the compare helper reads; reproduced here as the same byte
 * sequences. The container magic compare is 8 bytes of "VM_SOUND" (note:
 * the OEM rodata is the 9-byte "FVM_SOUND" string compared from offset
 * +1, i.e. the eight bytes "VM_SOUND"). */
static const char k_magic_vm_sound[8] = { 'V', 'M', '_', 'S', 'O', 'U', 'N', 'D' };
static const char k_tag_riff[4]        = { 'R', 'I', 'F', 'F' };
static const char k_tag_wave[4]        = { 'W', 'A', 'V', 'E' };
static const char k_tag_fmt[4]         = { 'f', 'm', 't', ' ' };
static const char k_tag_data[4]        = { 'd', 'a', 't', 'a' };

/* Parsed WAV/RIFF header laid out exactly as the on-flash 44-byte chunk
 * that follows the VanMoof container header. Offsets are validated one by
 * one by `audio_wav_open`; the field names follow canonical PCM WAV. */
struct wav_header {
    char     riff[4];         /* +0x00 "RIFF"                            */
    uint32_t riff_size;       /* +0x04 (not validated)                   */
    char     wave[4];         /* +0x08 "WAVE"                            */
    char     fmt[4];          /* +0x0C "fmt "                            */
    uint32_t fmt_size;        /* +0x10 == 0x10                           */
    uint16_t format;          /* +0x14 == 1 (PCM)                        */
    uint16_t channels;        /* +0x16 == 1 or 2                         */
    uint32_t sample_rate;     /* +0x18 != 0                              */
    uint32_t byte_rate;       /* +0x1C == rate*channels*bits/8           */
    uint16_t block_align;     /* +0x20 == channels*bits/8                */
    uint16_t bits_per_sample; /* +0x22 in (0, 0x20], multiple of 8       */
    char     data[4];         /* +0x24 "data"                            */
    uint32_t data_size;       /* +0x28 PCM byte count                    */
};

/* Output struct populated by `audio_wav_open`:
 *   +0x00  running ext-flash cursor (advanced past both headers)
 *   +0x04  PCM data byte count (== wav_header.data_size)
 *   +0x08  always 0 (reserved / high word of the data-size pair)
 * The OEM stores into out[0], out[1], out[2] as int words. */
struct audio_clip {
    uint32_t cursor;
    uint32_t data_size;
    uint32_t reserved;
};

/* Open + validate the WAV-clip header for `index`. Reads the 0x1C-byte
 * VanMoof container header and the 0x2C-byte RIFF header from ext-flash
 * into `*hdr`, validates every field, and on success stores the data-size
 * and cursor into `*clip` and returns 1. On any failure returns 0 (and,
 * for the header-validation failures, logs the clip index and a negative
 * error code).
 *
 * Error codes (all negative; logged in the "ERR <%d>" field). The values
 * follow the OEM `mvn`/`mov #-1` ladder exactly:
 *    -1  bad "RIFF"            -7  sample_rate == 0
 *    -2  bad "WAVE"            -8  bits_per_sample invalid
 *    -3  bad "fmt "            -9  byte_rate mismatch
 *    -4  fmt_size != 0x10     -10  block_align mismatch
 *    -5  format != 1 (PCM)    -11  bad "data"
 *    -6  channels not 1 or 2
 *
 * OEM @ 0x0000B164 (__func__ "f_open"). */
int audio_wav_open(struct audio_clip *clip, struct wav_header *hdr, int index)
{
    int err;

    clip->cursor = (uint32_t)index * AUDIO_SLOT_STRIDE + AUDIO_REGION_BASE;

    if (extflash_open() == 0) {
        return 0;
    }

    /* Container header: read 0x1C bytes, then compare the first 8 to the
     * "VM_SOUND" magic. The OEM re-uses `hdr`-adjacent stack space for
     * this scratch read; here we read into a small local. */
    uint8_t container[AUDIO_CONTAINER_HDR];
    extflash_read(clip->cursor, AUDIO_CONTAINER_HDR, container);
    clip->cursor += AUDIO_CONTAINER_HDR;
    if (memcmp(container, k_magic_vm_sound, 8) != 0) {
        /* Wrong/absent container magic: close and bail silently (no log). */
        extflash_close();
        return 0;
    }

    /* RIFF header. */
    extflash_read(clip->cursor, AUDIO_WAV_HDR, hdr);
    clip->cursor += AUDIO_WAV_HDR;

    if (memcmp(hdr->riff, k_tag_riff, 4) != 0) {
        err = -1;
    } else if (memcmp(hdr->wave, k_tag_wave, 4) != 0) {
        err = -2;
    } else if (memcmp(hdr->fmt, k_tag_fmt, 4) != 0) {
        err = -3;
    } else if (hdr->fmt_size != 0x10) {
        err = -4;
    } else if (hdr->format != 1) {
        err = -5;
    } else if (hdr->channels != 1 && hdr->channels != 2) {
        err = -6;
    } else if (hdr->sample_rate == 0) {
        err = -7;
    } else if (hdr->bits_per_sample == 0 ||
               hdr->bits_per_sample > 0x20 ||
               (hdr->bits_per_sample & 7) != 0) {
        err = -8;
    } else if (hdr->byte_rate !=
               ((hdr->sample_rate * hdr->channels * hdr->bits_per_sample) >> 3)) {
        err = -9;
    } else if ((uint32_t)hdr->block_align !=
               ((uint32_t)((int)(hdr->channels * hdr->bits_per_sample) >> 3))) {
        /* OEM uses an arithmetic shift (asr) here on the channels*bits
         * product — preserved verbatim. */
        err = -10;
    } else if (memcmp(hdr->data, k_tag_data, 4) != 0) {
        err = -11;
    } else {
        extflash_close();
        clip->data_size = hdr->data_size;
        clip->reserved  = 0;
        return 1;
    }

    extflash_close();
    monitor_log(K_FILE, 0x27F, "f_open", LOG_LEVEL_ERR,
                "audio clip <%d> has an invalid wav-header (ERR <%d>)",
                index, err);
    return 0;
}

/* Parse the clip header for `index` and dump its discovered location and
 * PCM parameters (sample rate, bits-per-sample, channel count, sample
 * count) to the monitor console. Returns 0 on success, -1 if the header
 * could not be parsed.
 *
 * OEM @ 0x0000D5CC (__func__ "audio_dump"). The OEM signature returns
 * undefined4 (0 / -1); every call site discards it, so the canonical
 * cross-TU prototype is `void` (see bleware.h). Kept observably
 * equivalent: the parse-fail path is a silent early-return. */
void audio_clip_dump_one(uint32_t index)
{
    struct audio_clip clip;
    struct wav_header hdr;

    if (audio_wav_open(&clip, &hdr, (int)index) == 0) {
        return;
    }

    /* num_samples = data_size / block_align, where the OEM recomputes the
     * block align as (channels * bits) >> 3 (arithmetic shift) rather than
     * reading hdr.block_align — preserved. */
    uint32_t block_align = (uint32_t)((int)(hdr.channels * hdr.bits_per_sample) >> 3);
    uint32_t num_samples = hdr.data_size / block_align;

    monitor_log(K_FILE, 0x247, "audio_dump", LOG_LEVEL_HELP,
                "WAV file discovered at index <%d> @ 0x%08x\r\n",
                index, (uint32_t)index * AUDIO_SLOT_STRIDE + AUDIO_REGION_BASE);
    monitor_log(K_FILE, 0x248, "audio_dump", LOG_LEVEL_HELP,
                "samplerate .......... : %d\r\n", hdr.sample_rate);
    monitor_log(K_FILE, 0x249, "audio_dump", LOG_LEVEL_HELP,
                "bits-per-sample ..... : %d\r\n", hdr.bits_per_sample);
    monitor_log(K_FILE, 0x24A, "audio_dump", LOG_LEVEL_HELP,
                "channel-count ....... : %d\r\n", hdr.channels);
    monitor_log(K_FILE, 0x24B, "audio_dump", LOG_LEVEL_HELP,
                "num-samples ......... : %d\r\n", num_samples);
}

/* Return the playback duration of clip `index` in milliseconds, or
 * 0xFFFFFFFF if the clip header cannot be parsed.
 *
 *   duration_ms = (num_samples * 1000) / sample_rate
 *   num_samples = data_size / ((channels * bits_per_sample) >> 3)
 *
 * OEM @ 0x00022FE8 — called from the audio-play path in xs3_app.c. */
uint32_t audio_get_clip_duration(uint32_t index)
{
    struct audio_clip clip;
    struct wav_header hdr;

    if (audio_wav_open(&clip, &hdr, (int)index) == 0) {
        return 0xFFFFFFFFu;
    }

    uint32_t block_align = (uint32_t)((int)(hdr.channels * hdr.bits_per_sample) >> 3);
    uint32_t num_samples = hdr.data_size / block_align;

    return (num_samples * 1000u) / hdr.sample_rate;
}
