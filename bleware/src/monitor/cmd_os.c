/* monitor/cmd_os.c — TI-RTOS and power-control monitor commands.
 *
 * OEM entries translated here:
 *   0x0000F6A0  rtos_statistics
 *   0x00010078  rtos_nvm_compact
 *   0x0001DCD0  shutdown
 *   0x0001D8E4  reset
 *
 * Source path observed in the monitor_log strings of three of the
 * four handlers:  `source/monitor/cmd_os.c`. (`cmd_shutdown` doesn't
 * log so it has no embedded path; grouping by feature rather than by
 * string evidence.)
 */

#include "monitor.h"

#include <stdint.h>

#define LOG_LEVEL_INFO 8
#define LOG_LEVEL_WARN 9

static const char K_FILE[] = "source/monitor/cmd_os.c";

/* ===== TI-RTOS heap statistics — Memory_getStats() in TI parlance.
 * OEM struct laid out as 3 contiguous u32s: total / free / largest. */
struct rtos_mem_stats {
    uint32_t total_size;
    uint32_t total_free_size;
    uint32_t total_largest_free_size;
};
extern void rtos_mem_get_stats(struct rtos_mem_stats *out);   /* OEM FUN_00025208 */

/* ===== Cancellable 500 ms timer probe.
 * The OEM uses this to poll for a keypress on the debug UART while
 * the periodic-stats loop is running:
 *   - on construction, queues a 50 ms timer
 *   - returns 1 once any key arrives, ≤0 while still waiting
 * (Decoded shape; the underlying timer plumbing is in `task_sleep.c`.) */
/* monitor_key_wait_with_timeout — declared in header */
/* monitor_yield_ticks — declared in header */

/* ===== SNV (Simple Non-Volatile storage) compact.
 *  - `snv_compact(0)` performs the compaction; returns non-zero on failure.
 *  - `snv_free_space_query(&stats)` fills the same 3-word struct used
 *    above. The TI-RTOS NVS subsystem reuses the heap-stats shape. */
extern void snv_free_space_query(struct rtos_mem_stats *out); /* OEM FUN_0001B79C */
extern int  snv_compact(int reserved);                        /* OEM FUN_00025904 */

/* ===== Power-down / reset =====
 *  - `system_state_save()` flushes any in-progress writes / state to
 *    ext-flash before the chip stops.
 *  - `system_power_down(mode, flags)` is the actual sleep-deep path.
 *  - `system_software_reset()` is the NVIC SYSRESETREQ wrapper. */
/* system_state_save — declared in header */
/* system_power_down — declared in header */
/* system_software_reset — declared in header */

int cmd_rtos_statistics(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "rtos_statistics", 0x10);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("rtos_statistics",
                                "dump memory stats every 500ms");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "rtos_statistics") == 0) {
        return 2;
    }

    /* OEM stack layout: an 8-byte timer context immediately below the
     * stats struct, threaded between iterations so the keypress probe
     * carries state across the 4-tick yield. */
    uint8_t                timer_ctx[8];
    struct rtos_mem_stats  stats;

    int got_key = monitor_key_wait_with_timeout(timer_ctx, 50000 /* µs = 50 ms */);
    while (got_key < 1) {
        rtos_mem_get_stats(&stats);
        monitor_log(K_FILE, 0x35, "cmd_rtos_statistics", LOG_LEVEL_INFO,
                    "%-18s : %d", "total size", stats.total_size);
        monitor_log(K_FILE, 0x36, "cmd_rtos_statistics", LOG_LEVEL_INFO,
                    "%-18s : %d", "total free size", stats.total_free_size);
        monitor_log(K_FILE, 0x38, "cmd_rtos_statistics", LOG_LEVEL_INFO,
                    "%-18s : %d", "total largest free size",
                    stats.total_largest_free_size);
        monitor_yield_ticks(4);
        got_key = monitor_key_wait_with_timeout(timer_ctx, 50000);
    }
    return 0;
}

int cmd_rtos_nvm_compact(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "rtos_nvm_compact", 0x11);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("rtos_nvm_compact",
                                "Compact the non-volatile storage");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "rtos_nvm_compact") == 0) {
        return 2;
    }

    struct rtos_mem_stats stats;
    snv_free_space_query(&stats);
    monitor_log(K_FILE, 0x5e, "cmd_rtos_nvm_compact", LOG_LEVEL_WARN,
                "Free space before compaction: %d", stats.total_size);

    int rc = snv_compact(0);
    if (rc != 0) {
        monitor_log(K_FILE, 0x61, "cmd_rtos_nvm_compact", LOG_LEVEL_WARN,
                    "SNV Compact failed");
    } else {
        snv_free_space_query(&stats);
        monitor_log(K_FILE, 0x67, "cmd_rtos_nvm_compact", LOG_LEVEL_WARN,
                    "Free space after compaction: %d", stats.total_size);
    }
    return 0;
}

int cmd_shutdown(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "shutdown", 0x09);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("shutdown", "shutdown the system");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "shutdown") == 0) {
        return 2;
    }

    system_state_save();
    system_power_down(0, 0);
    return 0;
}

int cmd_reset(int verb, void *p2, void *p3, uint32_t p4)
{
    (void)p3;
    (void)p4;

    if (verb == MON_CMD_FILL_NAME) {
        memcpy(p2, "reset", 0x06);
        return 0;
    }

    if (verb == MON_CMD_PRINT_HELP) {
        monitor_print_help_line("reset",
                                "perform software reset of the MCU");
        return 0;
    }

    if (verb != MON_CMD_EXECUTE) {
        return 1;
    }

    if (monitor_command_matches((const char *)p2, "reset") == 0) {
        return 2;
    }

    system_software_reset();
    return 0;
}
