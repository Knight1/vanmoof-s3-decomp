/* monitor/cmd_extflash.c — external SPI-flash console commands.
 *
 * Path string at flash 0x0000ACE4 confirms: `source/monitor/cmd_extflash.c`.
 *
 * This file implements the bleware monitor's `extflash-*` command(s).
 * Only ONE command — `extflash-verify` — actually survives in the OEM
 * binary (handler at 0x0000ABD8). See "Dead siblings" below for the
 * full story on the dump/erase/upload helpers that the OEM source file
 * presumably also defined.
 *
 * Monitor command handler signature
 * ---------------------------------
 * Every cmd_*.c handler in bleware exposes ONE entry point with the
 * convention:
 *
 *     int cmd_xxx(int verb, void *p2, void *p3, uint32_t p4);
 *
 * where `verb` selects mode:
 *
 *   verb == 0  PRINT_HELP: emit one help-table row. Handler calls
 *              `monitor_print_help_line(name, description)` — the
 *              printf-style helper at OEM 0x00021244 that formats
 *              `"    %-33s - %s\r\n"` and writes it to the monitor log.
 *              Walked by `cmd_help` over the static command table.
 *
 *   verb == 1  FILL_NAME: write the command name into the caller's
 *              16-byte buffer at `p2` (via `memcpy(p2, name, 0x10)`).
 *              Used by the monitor's tab-complete / introspection path.
 *
 *   verb == 2  EXECUTE: run the command with the user-input string
 *              in `p2`.
 *
 *   default    return 1 (unrecognised verb).
 *
 *   return 0 on success; non-zero error codes have command-specific
 *   semantics (2 = "could not open NVS", etc.).
 *
 * The registry IS the static table at OEM 0x0002A0BC — 27+ function
 * pointers, NULL-terminated, walked by:
 *   - `cmd_help` (OEM 0x00013C20)         with verb=0 (PRINT_HELP)
 *   - `monitor_dispatch_loop` (OEM 0x00024B38) with verb=2 (EXECUTE)
 * There is no startup registration walker — the table itself is
 * authoritative.
 */

#include "bleware.h"

#include <stdint.h>

/* ---- log emit + monitor registration ----
 *
 * `monitor_log(file, line, source_func_name, level, fmt, ...)` is the
 * universal structured-log entry (OEM @ 0x00006D90, named in this
 * decomp). Implemented somewhere in cmd_log.c (TBD). Callers use
 * level=2 for ERROR and level=9 for INFO. */
extern void monitor_log(const char *file, int line, const char *fn,
                        int level, const char *fmt, ...);

/* OEM 0x00021244 — emit one row of the help table via monitor_log
 * with format "    %-33s - %s\r\n". */
extern int  monitor_print_help_line(const char *name,
                                    const char *description);

/* C standard memcpy — OEM 0x00018654, the word-aligned-fast-path
 * implementation. Each cmd_* handler uses it on verb=1 to copy its
 * 16-byte name into the caller's buffer. */
extern void *memcpy(void *dst, const void *src, unsigned int n);

/* ---- ext-flash device wrappers ----
 *
 * Wrap the TI NVS (Non-Volatile Storage) driver + the SPI-flash chip
 * driver. The NVS driver is what cmd_extflash_verify opens before
 * touching the chip; the chip-info accessor returns a 12-byte struct
 * with `device_size_word` at offset 0 and a `name` pointer at +8. */
extern void *  nvs_open(void *params);
extern int     extflash_open(void *handle);
extern void    extflash_close(void);

extern int     extflash_retry_backoff(void);   /* returns 0 to keep retrying */
extern void *  extflash_get_chip_info(void);   /* returns pointer to info struct */
extern int     extflash_sw_wp_enabled(void);   /* SW status-register write-protect */
extern int     extflash_block_wp_enabled(void); /* per-block write-protect */

/* Log level constants — mirror what monitor_log uses in the OEM. */
#define LOG_ERR  2
#define LOG_INFO 9

/* Source-file path string the OEM logger uses to identify this TU.
 * The 'F' prefix is the OEM logger's "format string follows" marker;
 * the same string appears at flash 0x0000ACE4 in the OEM image. */
static const char K_FILE[] = "source/monitor/cmd_extflash.c";

/* Verb selector — universal across all cmd_*.c handlers. */
enum cmd_verb {
    CMD_VERB_PRINT_HELP = 0,
    CMD_VERB_FILL_NAME  = 1,
    CMD_VERB_EXECUTE    = 2,
};

/* OEM @ 0x0000ABD8 (~220 B). The `extflash-verify` monitor command:
 * open the external SPI flash via the NVS driver, read its chip-info
 * struct, and emit four formatted log lines reporting flash type,
 * device size in megabytes, SW-status-register write-protect state,
 * and block-write-protection state.
 *
 * Returns:
 *   0  — verify completed (any of the cmd-verb branches);
 *   1  — unrecognised verb;
 *   2  — could not open NVS handle. */
int cmd_extflash_verify(int verb, void *p2, void *p3, uint32_t p4)
{
    if (verb == CMD_VERB_FILL_NAME) {
        memcpy(p2, "extflash-verify", 0x10);
        return 0;
    }

    if (verb == CMD_VERB_PRINT_HELP) {
        monitor_print_help_line("extflash-verify",
                                "verify the current flashchip");
        return 0;
    }

    if (verb != CMD_VERB_EXECUTE) {
        return 1;
    }

    /* ---- execute ---- */

    void *nvs = nvs_open(p2);
    if (nvs == 0) {
        return 2;
    }

    /* Retry-on-open loop: keep trying to open the flash via the NVS
     * handle. On failure, log an error and back off; loop until the
     * backoff returns non-zero (giving up) or until open succeeds. */
    int open_status;
    for (;;) {
        open_status = extflash_open((int *)nvs);
        if (open_status == 1) {
            break;  /* success */
        }
        extflash_close();
        monitor_log(K_FILE, 0x45, "cmd_extflash_verify", LOG_ERR,
                    "Failed while opening Non Volatile memory\n");
        int give_up = extflash_retry_backoff();
        nvs = 0;
        if (give_up != 0) {
            break;  /* gave up */
        }
    }

    if (open_status == 0) {
        /* opened-and-gave-up branch — chip not accessible. */
        return 0;
    }

    /* Open succeeded — query chip info and report. */
    struct chip_info {
        uint32_t       size_word;  /* device size in some unit; >>20 gives MB */
        uint32_t       _pad;
        const char    *name;       /* flash chip family name */
    } *info = (struct chip_info *)extflash_get_chip_info();

    monitor_log(K_FILE, 0x4C, "cmd_extflash_verify", LOG_INFO,
                "Flash type         : %s", info->name);

    info = (struct chip_info *)extflash_get_chip_info();
    monitor_log(K_FILE, 0x4E, "cmd_extflash_verify", LOG_INFO,
                "Device size (Mbyte): %d", info->size_word >> 20);

    char sw_wp = (extflash_sw_wp_enabled() == 1) ? 'y' : 'n';
    monitor_log(K_FILE, 0x50, "cmd_extflash_verify", LOG_INFO,
                "SW status register write protection: %c", sw_wp);

    char blk_wp = (extflash_block_wp_enabled() == 1) ? 'y' : 'n';
    monitor_log(K_FILE, 0x51, "cmd_extflash_verify", LOG_INFO,
                "Block write-protection enabled     : %c", blk_wp);

    extflash_close();
    return 0;
}

/* Dead siblings — cmd_extflash_dump / _erase / _upload
 * ----------------------------------------------------
 * The OEM `.rodata` retains three function-name strings the C
 * preprocessor (`__func__`) would have emitted alongside their
 * respective bodies:
 *
 *   0x0002AB94  "cmd_extflash_dump"
 *   0x0002ABA6  "cmd_extflash_erase"
 *   0x0002ABCD  "cmd_extflash_upload"
 *
 * Both 32-bit literal-pool scans and MOVW immediate scans confirm
 * NONE of these strings is referenced anywhere in the binary. There
 * is no surviving function body, no monitor-registration call, no
 * command-name string ("extflash-dump", etc.), and no help text. The
 * OEM build's link-time gc dropped the bodies but the per-TU
 * `.rodata.str` section was kept whole, leaving the names as
 * artefacts.
 *
 * Conclusion: only `extflash-verify` is reachable in the production
 * bleware image. We deliberately do NOT declare extern stubs for the
 * three dead siblings — the build would have to invent bodies for
 * them, and the result would not match OEM behaviour. If a future
 * bleware drop reintroduces them (e.g. a debug build that keeps the
 * full extflash console), revisit this file. */
