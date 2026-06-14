/* monitor/console.c — console-device driver glue for the debug monitor.
 *
 * The console input parser (monitor_readline / monitor_task_iteration in
 * monitor/monitor.c) reaches the UART through this thin driver layer.
 * These four functions were weak no-op stubs in src/hal_stubs.S until now;
 * each is decoded here to faithful C. The lowest-level byte transfer
 * primitives they call (FUN_00024c40 single-byte read, FUN_00025b84 /
 * FUN_00025b64 TX-FIFO drain, FUN_0002751a event-handle signal) are TI
 * UART/RTOS-driver leaves left as externs; they gc-section away with the
 * rest of the monitor in the stub build.
 *
 * OEM functions:
 *   monitor_console_read   @ 0x000238A8
 *   monitor_console_flush  @ 0x000255D4
 *   monitor_event_signal   @ 0x000232B8
 *   monitor_console_active @ 0x00027742
 */

#include <stdint.h>
#include <stddef.h>

/* "Console torn down" flag (RAM 0x2000AE04). Non-zero once the console
 * session is closed; both read and flush bail out early when it is set.
 * Owned by the (undecoded) console open/close path. */
extern volatile uint8_t g_console_disabled;

/* Bluetoothtask event-flag block (RAM 0x20005770). monitor_event_signal
 * ORs caller bits into `pending`, and once every bit of `required` is set
 * it pulses `signal` (waking the scheduler) and clears the required bits.
 * Accesses are serialised by `mutex`. */
struct console_event {
    uint32_t signal;          /* +0x00 event/semaphore handle to post   */
    uint32_t pending;         /* +0x04 accumulated flag bits             */
    uint32_t mutex;           /* +0x08 guard semaphore                   */
    uint32_t required;        /* +0x0c mask that triggers a wake         */
};
extern volatile struct console_event g_console_event;

/* Lowest-level console/RTOS leaves (other TUs / TI driver). */
extern int  FUN_00024c40(void *dst, uint32_t timeout_us);  /* read one byte; 1 = got a byte */
extern int  FUN_00025b84(void);                            /* TX bytes still pending */
extern void FUN_00025b64(void *scratch);                   /* push one TX byte out */
extern void FUN_0002751a(uint32_t handle);                 /* post the wake handle */

/* TI-RTOS ROM semaphore primitives (src/hal_stubs.S ROM aliases). */
extern int  ti_semaphore_pend(uint32_t sem, uint32_t timeout);
extern void ti_semaphore_post(uint32_t sem);

/* Read up to `count` bytes into `dst`, one at a time, each poll bounded by
 * `timeout_us`. Returns the number of bytes read, or -1 if the console has
 * been torn down. Stops early the first time a poll returns no byte.
 *
 * OEM @ 0x000238A8. */
int monitor_console_read(void *dst, int count, uint32_t timeout_us)
{
    if (g_console_disabled != 0) {
        return -1;
    }

    uint8_t *p   = (uint8_t *)dst;
    int      got = 0;
    do {
        if (FUN_00024c40(p, timeout_us) != 1) {
            return got;             /* nothing more available */
        }
        got++;
        if (got == 0) {             /* OEM's int-wrap guard (unreachable) */
            return 0;
        }
        count--;
        p++;
    } while (count != 0);

    return got;
}

/* Drain the console TX FIFO. Returns 0 once empty, or -1 if the console is
 * torn down. OEM @ 0x000255D4. */
int monitor_console_flush(void)
{
    if (g_console_disabled != 0) {
        return -1;
    }

    uint32_t scratch = 0;           /* FUN_00025b64 fills it; init only to satisfy -W */
    while (FUN_00025b84() > 0) {
        FUN_00025b64(&scratch);
    }
    return 0;
}

/* Post `flag_bits` to the bluetoothtask event block and, once the required
 * mask is fully satisfied, pulse the scheduler and clear those bits. The
 * whole update is taken under the block's guard semaphore.
 *
 * OEM @ 0x000232B8. */
void monitor_event_signal(uint32_t flag_bits)
{
    if (ti_semaphore_pend(g_console_event.mutex, 0xffffffffu) == 0) {
        return;
    }

    uint32_t combined = g_console_event.pending | flag_bits;
    g_console_event.pending = combined;
    if ((g_console_event.required & combined) == g_console_event.required) {
        FUN_0002751a(g_console_event.signal);
        g_console_event.pending &= ~g_console_event.required;
    }

    ti_semaphore_post(g_console_event.mutex);
}

/* Whether the console session is live. In this image it is a constant 1.
 * OEM @ 0x00027742. */
int monitor_console_active(void)
{
    return 1;
}
