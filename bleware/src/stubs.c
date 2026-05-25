/* stubs.c — lightweight implementations for remaining weak stubs.
 *
 * These are called by already-decoded VanMoof custom code (cmd_*
 * handlers, state machine, monitor infrastructure) but the OEM
 * bodies are either vendor-stock TI-RTOS wrappers or complex
 * enough to defer. Minimal implementations let the VanMoof code
 * link and test without the full TI SDK.
 *
 * When the TI SDK is vendored (see vendor/README.md), these weak
 * symbols are overridden by the real implementations.
 */

#include <stdint.h>
#include <stddef.h>

#include "bleware.h"

/* ---- Monitor helpers ------------------------------------------------ */

void monitor_sleep(uint32_t ticks)
{
    (void)ticks;
}

int monitor_strlen(const char *s)
{
    int n = 0;
    while (*s++) n++;
    return n;
}

void monitor_yield_ticks(void)
{
    /* no-op in stub build */
}

uint32_t monitor_key_wait_with_timeout(uint32_t timeout)
{
    (void)timeout;
    return 0;
}

int monitor_sscanf(const char *input, const char *fmt, ...)
{
    (void)input; (void)fmt;
    return 0;
}

int monitor_snprintf(char *buf, unsigned int size, const char *fmt, ...)
{
    (void)buf; (void)size; (void)fmt;
    return 0;
}

/* ---- RTOS & system helpers ----------------------------------------- */

uint32_t rtos_mem_get_stats(void *stats_out)
{
    (void)stats_out;
    return 0;
}

int snv_compact(uint32_t arg)
{
    (void)arg;
    return 0;
}

int snv_free_space_query(void)
{
    return 0;
}

void system_power_down(void)
{
    for (;;) {}
}

void system_software_reset(void)
{
    for (;;) {}
}

void system_state_save(void)
{
    /* no-op in stub build */
}

/* ---- Audio helpers -------------------------------------------------- */

void audio_clip_dump_one(uint32_t index)
{
    (void)index;
}

void audio_player_play(uint32_t index)
{
    (void)index;
}

void audio_player_stop_or_pause(int action)
{
    (void)action;
}

/* ---- PACK filesystem helpers ---------------------------------------- */

void *packfs_open(void *params)
{
    (void)params;
    return NULL;
}

int packfs_next(void *handle, void *entry_out)
{
    (void)handle; (void)entry_out;
    return 0;
}

void packfs_close(void *handle)
{
    (void)handle;
}

int pack_ingest_start(void)
{
    return 0;
}

int pack_upload_finalize(void)
{
    return 0;
}

/* ---- Firmware update / info ----------------------------------------- */

int firmware_update_start(void *params)
{
    (void)params;
    return 0;
}

void print_firmware_info(void)
{
}

void format_size(uint32_t bytes, char *buf, unsigned int bufsz)
{
    (void)bytes; (void)buf; (void)bufsz;
}

/* ---- Log helpers ---------------------------------------------------- */

uint32_t log_block_count(void)
{
    return log_block_count_get();
}

void log_format_block(uint32_t index, void *out_16B)
{
    (void)index; (void)out_16B;
}

void log_submit(const void *block_16B)
{
    (void)block_16B;
}