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
int  monitor_print_help_line(const char *name, const char *description);
int  monitor_command_matches(const char *input, const char *name);

void *memcpy(void *dst, const void *src, unsigned int n);
int   monitor_sscanf(const char *input, const char *fmt, ...);
void  monitor_sleep(uint32_t ticks);

#endif /* BLEWARE_MONITOR_H */
