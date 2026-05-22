/* monitor/cmd_audio.c — audio-clip monitor commands.
 *
 * OEM entries translated here:
 *   0x0000F19C  audio_play
 *   0x0001CE3C  audio_stop
 *   0x0001522C  audio_dump
 *   0x0000CAE0  audio_upload
 *   0x0000C614  audio_volume_set_all
 *
 * Audio clips live in the external SPI NOR at ext-flash address
 *     0x200000 + index * 0x80000
 * for index = 0..0x7A (123 clips max). The slot stride (0x80000 =
 * 512 KiB) matches the OAD data-file stride documented elsewhere.
 *
 * Playback is decoupled — bleware does NOT decode audio itself.
 * Each `play` request is `module_forward_async(0x5571, index)` to the
 * motor MCU which owns the I²S DAC.
 */

#include "monitor.h"

#include <stdint.h>

#define LOG_LEVEL_HELP 8

#define AUDIO_CLIP_COUNT  0x7B           /* indices 0..0x7A inclusive */
#define AUDIO_REGION_BASE 0x200000u
#define AUDIO_SLOT_STRIDE 0x80000u
#define AUDIO_YMODEM_CAP  0x300000u      /* per-receive size cap */

static const char K_FILE[] = "source/monitor/cmd_audio.c";

extern int   extflash_open(void);
extern void  extflash_close(void);
extern int   ymodem_receive(uint32_t dst, uint32_t max_size);   /* OEM FUN_000101B0 */
extern int   module_forward_async(uint32_t cmd_id, uint8_t arg);
extern int   module_publish_command(uint16_t cmd, const uint8_t *payload, unsigned int len);
extern void  audio_clip_dump_one(uint8_t index);                /* OEM FUN_0000D5CC */
extern void  audio_player_play(uint8_t index);                  /* OEM FUN_000275B8 */
extern void  audio_player_stop_or_pause(int mode);              /* OEM FUN_00027630 */

/* YModem-receive target: bleware stages the next clip into ext-flash
 * by writing this address into the YModem state struct before invoking
 * the receive loop. Lives at OEM `*DAT_0000CC70`. */
extern uint32_t *g_ymodem_target_offset;                        /* OEM DAT_0000CC70 */

int cmd_audio_play(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    uint32_t index = 0xFFFFFFFFu;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "audio_play", 0x0b);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("audio_play <index>",
                                "play audio bound to the specified index");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    int rc = monitor_sscanf((const char *)p2, "audio_play %d", &index);
    if (rc < 1) {
        return 2;
    }

    if (index == 0xFFFFFFFFu) {
        return 0;
    }
    if (index >= AUDIO_CLIP_COUNT) {
        monitor_log(K_FILE, 0xdf, "cmd_audio_play", LOG_LEVEL_HELP,
                    "audio file index should be in range [0,%d)",
                    AUDIO_CLIP_COUNT);
        return 0;
    }

    monitor_log(K_FILE, 0xe3, "cmd_audio_play", LOG_LEVEL_HELP,
                "Playing audio clip <%d>", index);
    audio_player_play((uint8_t)index);
    return 0;
}

int cmd_audio_stop(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "audio_stop", 0x0b);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("audio_stop",
                                "stop playing the current audio file");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "audio_stop") != 1) {
        return 2;
    }

    audio_player_stop_or_pause(1);
    return 0;
}

int cmd_audio_dump(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "audio_dump", 0x0b);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("audio_dump",
                                "dump all audio files in external flash");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "audio_dump") == 0) {
        return 2;
    }

    if (extflash_open() == 0) {
        monitor_log(K_FILE, 0x12a, "cmd_audio_dump", LOG_LEVEL_HELP,
                    "could not open flash");
        return 2;
    }
    extflash_close();

    /* The OEM closes the bus handle BEFORE iterating — each
     * audio_clip_dump_one re-opens the bus per clip. Preserved
     * verbatim. */
    for (uint32_t i = 0; i < AUDIO_CLIP_COUNT; i++) {
        audio_clip_dump_one((uint8_t)i);
    }
    return 0;
}

int cmd_audio_upload(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    uint32_t index = 0xFFFFFFFFu;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "audio_upload", 0x0d);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("audio_upload <index>",
                                "upload audio binary using Y-Modem");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    int rc = monitor_sscanf((const char *)p2, "audio_upload %d", &index);
    if (rc != 1) {
        return 2;
    }

    if (index != 0xFFFFFFFFu) {
        if (index < AUDIO_CLIP_COUNT) {
            *g_ymodem_target_offset = AUDIO_REGION_BASE + index * AUDIO_SLOT_STRIDE;
            monitor_log(K_FILE, 0x4d, "cmd_audio_upload", LOG_LEVEL_HELP,
                        "YModem audio file request...");
            ymodem_receive(*g_ymodem_target_offset, AUDIO_YMODEM_CAP);
        } else {
            monitor_log(K_FILE, 0x47, "cmd_audio_upload", LOG_LEVEL_HELP,
                        "audio file index should be in range [0,%d)",
                        AUDIO_CLIP_COUNT);
        }
    }

    /* Fire the motorware notification AFTER the YModem completes (or
     * after the range-error log, which matches the OEM unconditional
     * fire — likely intentional so motorware always sees the cue). */
    module_forward_async(0x5571, (uint8_t)index);
    return 0;
}

int cmd_audio_volume_set_all(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    uint32_t level = 0xFFFFFFFFu;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "audio_volume_set_all", 0x15);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("audio_volume_set_all <level>",
                                "set audio level of all audio clips");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "audio_volume_set_all") == 0) {
        return 2;
    }

    int rc = monitor_sscanf((const char *)p2, "audio_volume_set_all %d", &level);
    if (rc != 1) {
        monitor_log(K_FILE, 0x77, "cmd_audio_volume_set_all", LOG_LEVEL_HELP,
                    "Could not derive audio level from input");
        return 0;
    }

    /* `level` is an enum [0..3] selecting which 4-byte slot in a
     * 12-byte mask gets set to 0xFFFFFFFF (the other slots stay 0).
     * The OEM compiler emits an odd off-by-one offset chain that we
     * preserve verbatim: writing past slot 3 corrupts the next stack
     * frame's lower word — but the bounds check above guarantees that
     * never happens at runtime. */
    if ((int32_t)level < 0 || level >= 4) {
        monitor_log(K_FILE, 0x7c, "cmd_audio_volume_set_all", LOG_LEVEL_HELP,
                    "Invalid level <%d>, should be in [0..3]", level);
        return 0;
    }

    uint8_t  payload[12];
    uint32_t *slots = (uint32_t *)payload;
    slots[0] = 0;
    slots[1] = 0;
    slots[2] = 0;
    if (level > 0) {
        slots[level] = 0xFFFFFFFFu;
    }

    /* Modbus cmd 0x5572 = "audio volume per-channel mask". The motor
     * MCU consumes this and applies it to its DAC volume registers. */
    module_publish_command(0x5572, payload, sizeof payload);
    return 0;
}
