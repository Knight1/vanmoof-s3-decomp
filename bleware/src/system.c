/* system.c — power-down, software-reset, and state-save helpers.
 *
 * Called by the monitor commands cmd_shutdown and cmd_reset.
 *
 *   system_state_save()        @ 0x00026FF4 — LED off (DIO 0xD low)
 *   system_power_down()        @ 0x0001D404 — AON sleep-deep sequence
 *   system_software_reset()    @ 0x0001F7F8 — same as firmware_abort
 */

#include <stdint.h>

#include "bleware.h"

void system_state_save(void)
{
    extern void *g_bleware_gpio_ctx;
    extern int   gpio_write(const void *ctx, uint32_t pin, int value);
    gpio_write(g_bleware_gpio_ctx, 0xD, 0);
}

int system_power_down(int unused1, int unused2)
{
    (void)unused1; (void)unused2;

    /* The OEM body (~100 B) invokes TI-RTOS Task_self, checks a
     * power-state flag at DAT_0001D460+0x134, calls AON shutdown
     * helpers, and enters a deep-sleep state. Until we decode the
     * full AON driver, this stub simply halts. */
    for (;;) {}
}

void system_software_reset(void)
{
    firmware_abort();
}
