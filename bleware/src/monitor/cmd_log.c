/* monitor/cmd_log.c — log monitor commands decoded from the table.
 *
 * OEM entries translated here:
 *   0x0000C2D4  log_dump
 *   0x0000E190  log_inject
 *   0x0001699C  log_count
 *   0x00016DCC  log_flush
 */

#include "monitor.h"

#include <stdint.h>

#define LOG_LEVEL_HELP 8
#define LOG_LEVEL_INFO 9

static const char K_FILE[] = "source/monitor/cmd_log.c";

extern int   log_block_count(void);
extern void  log_format_block(char *dst, int index);
extern void *monitor_alloc(uint32_t size);
extern int   monitor_strlen(const char *s);
extern void  log_submit(uint32_t channel, void *block, uint32_t len);
extern void  monitor_free(void *ptr);
extern int   monitor_snprintf(char *dst, const char *fmt, ...);
extern void  log_region_erase(void);    /* OEM FUN_000230D8 — erases the 128 KB log region on ext-flash */
extern void  log_writer_restart(void);  /* OEM FUN_00017B24 — rewinds the log writer cursor */

int cmd_log_dump(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    int count = log_block_count();

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "log_dump", 9);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("log_dump <start_index> <n>",
                                "print <n> blocks starting at address <start_index>");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    const char *input = (const char *)p2;
    if (monitor_command_matches(input, "log_dump") == 0) {
        return 2;
    }

    int start = 0;
    int n = 0;
    if (monitor_sscanf(input, "log_dump %d %d", &start, &n) != 2) {
        monitor_log(K_FILE, 0x78, "cmd_log_dump", LOG_LEVEL_HELP,
                    "Invalid log dump arguments");
        return 2;
    }

    if (count <= start) {
        monitor_log(K_FILE, 0x7d, "cmd_log_dump", LOG_LEVEL_HELP,
                    "Start index should not exceed <%d", count);
        return 0;
    }

    int end = start + n;
    if (count <= end) {
        end = count;
    }

    for (int i = start; i < end; i++) {
        char line[16];
        log_format_block(line, i);
        monitor_log(K_FILE, 0x87, "cmd_log_dump", LOG_LEVEL_HELP,
                    "  %-16s", line);
        monitor_sleep(4);
    }

    return 0;
}

int cmd_log_inject(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    int count = log_block_count();

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "log_inject", 0x0b);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("log_inject <n>", "Create <n> fake logs");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    const char *input = (const char *)p2;
    if (monitor_command_matches(input, "log_inject") == 0) {
        return 2;
    }

    int n = 0;
    if (monitor_sscanf(input, "log_inject %d", &n) != 1) {
        monitor_log(K_FILE, 0xaf, "cmd_log_inject", LOG_LEVEL_HELP,
                    "Invalid log inject arguments");
        return 2;
    }

    monitor_log(K_FILE, 0xb3, "cmd_log_inject", LOG_LEVEL_INFO,
                "Currently we have %d log blocks", count);

    for (int i = 0; i < n; i++) {
        uint16_t *block = (uint16_t *)monitor_alloc(0x18);
        if (block != 0) {
            monitor_snprintf((char *)(block + 3), "logblock <%d>", i);
            block[0] = 0x0106;
            int len = monitor_strlen((const char *)(block + 3));
            block[2] = (uint16_t)len;
            log_submit(0x1d, block, (uint32_t)len + 6u);
            monitor_free(block);
        }
    }

    return 0;
}

int cmd_log_count(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "log_count", 0x0a);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("log_count", "get log count statistic");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "log_count") == 0) {
        return 2;
    }

    int count = log_block_count();
    monitor_log(K_FILE, 0x54, "cmd_log_count", LOG_LEVEL_HELP,
                "Amount of log blocks <%d>", count);
    return 0;
}

int cmd_log_flush(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "log_flush", 0x0a);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("log_flush", "flush all log entries");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "log_flush") == 0) {
        return 2;
    }

    log_region_erase();
    monitor_log(K_FILE, 0x33, "cmd_log_flush", LOG_LEVEL_HELP,
                "Done erasing logs");
    log_writer_restart();
    return 0;
}
