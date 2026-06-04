/* boot.c — hand the CPU to the validated application.
 *
 * Reconstructed from goto_application (0x0800163C).
 *
 * The real application vector table sits at AP_BASE + 0x28 (just past the
 * 40-byte VanMoof header), so the initial SP is at +0x28 and the reset entry at
 * +0x2C. Before jumping, the loader tears down USART1, kicks the watchdog one
 * last time, and masks interrupts.
 */
#include "bmsboot.h"

/* Cortex-M0+ stack-pointer / privilege intrinsics (CMSIS-equivalent). */
static inline void     disable_irq(void)     { __asm volatile ("cpsid i" ::: "memory"); }
static inline void     set_msp(uint32_t sp)  { __asm volatile ("msr msp, %0" :: "r"(sp) : ); }
static inline uint32_t get_control(void)     { uint32_t c; __asm volatile ("mrs %0, control" : "=r"(c)); return c; }

void goto_application(void)
{
    comms_deinit();                       /* release USART1 / PA9-PA10          */
    REG32(IWDG_BASE) = IWDG_KR_RELOAD;    /* one last watchdog kick (IWDG_KR)   */
    disable_irq();

    uint32_t app_sp    = REG32(APP_BASE + IMG_HDR_SIZE);       /* +0x28 */
    uint32_t app_reset = REG32(APP_BASE + IMG_HDR_SIZE + 4);   /* +0x2C */

    if ((get_control() & 1u) == 0u)       /* privileged thread mode (always here) */
        set_msp(app_sp);
    ((void (*)(void))app_reset)();        /* never returns                      */
}
