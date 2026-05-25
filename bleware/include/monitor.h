#ifndef BLEWARE_MONITOR_H
#define BLEWARE_MONITOR_H

#include <stdint.h>

typedef int (*monitor_cmd_handler_t)(int verb, void *p2, void *p3,
                                     uint32_t p4);

enum monitor_cmd_verb {
    MON_CMD_PRINT_HELP = 0,
    MON_CMD_FILL_NAME  = 1,
    MON_CMD_EXECUTE    = 2,
};

void monitor_log(const char *file, int line, const char *fn, int level,
                 const char *fmt, ...);
void monitor_print_help_line(const char *name, const char *description);
int  monitor_command_matches(const char *input, const char *name);

void *memcpy(void *dst, const void *src, unsigned int n);
int   monitor_sscanf(const char *input, const char *fmt, ...);
void  monitor_sleep(uint32_t ticks);
int   monitor_strlen(const char *s);
void     monitor_yield_ticks(uint32_t ticks);
uint32_t monitor_key_wait_with_timeout(void *timer_ctx, uint32_t period_us);
int   monitor_snprintf(char *buf, unsigned int size, const char *fmt, ...);

#endif /* BLEWARE_MONITOR_H */
