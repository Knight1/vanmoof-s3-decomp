/* boot.c — A/B (dual-bank) boot orchestrator and jump-to-application.
 *
 * Reconstructed from boot_main (0x080014A0) and goto_application (0x080019C0).
 *
 * boot_main() decides what to run on every reset:
 *   1. a finished OTA left a fresh image in Shadow  -> install Shadow->AP, boot
 *   2. AP is valid                                  -> back up AP->Shadow, boot AP
 *   3. AP corrupt but Shadow is valid               -> recover Shadow->AP, boot
 *   4. neither bank valid                           -> stay resident as the
 *                                                      serial-download server
 *
 * The image banks are AP_BASE (booted) and SHADOW_BASE (backup / OTA staging);
 * image_verify() returns IMG_OK (0) only for a magic + size + CRC-32 match.
 */
#include "powerbankboot.h"

/* OEM SRAM globals (addresses recorded in docs/hardware.md). */
uint8_t          g_upgrade_finished;   /* 0x20000BB4 */
volatile uint8_t g_boot_events;        /* 0x20000BBC — posted by SysTick      */
uint8_t          g_loop_mode;          /* 0x20000B50 — bit0 busy, bit1 done   */

static uint8_t   s_keepalive_div;      /* 0x20000B90 — ~100-tick banner re-arm */

/* Cortex-M0 stack-pointer / privilege intrinsics (CMSIS-equivalent). */
static inline void set_msp(uint32_t sp)      { __asm volatile ("msr msp, %0" :: "r"(sp) : ); }
static inline uint32_t get_control(void)     { uint32_t c; __asm volatile ("mrs %0, control" : "=r"(c)); return c; }

/* goto_application() — hand the CPU to the validated image at AP_BASE.
 * The real vector table sits at AP_BASE + 0x28 (just past the VanMoof header),
 * so the initial SP is at +0x28 and the reset entry at +0x2C. */
void goto_application(void)
{
    dbg_printf(STR_GOTO_APP);
    store_boot_flag(0);                 /* clear the persisted upgrade flag    */
    iwdg_refresh();                     /* one last kick before handing over   */

    uint32_t app_sp    = REG32(AP_BASE + IMG_HDR_SIZE);       /* +0x28 */
    uint32_t app_reset = REG32(AP_BASE + IMG_HDR_SIZE + 4);   /* +0x2C */

    if ((get_control() & 1u) == 0u)     /* privileged thread mode (always, here)*/
        set_msp(app_sp);
    ((void (*)(void))app_reset)();      /* never returns                       */
}

void boot_main(void)
{
    boot_hw_init();
    boot_read_persistent_flags();

    /* 1. Finished OTA: a verified image is parked in Shadow. Install + boot. */
    if (g_upgrade_finished == 1 && image_verify((const uint32_t *)SHADOW_BASE) == IMG_OK) {
        flash_copy_image(AP_BASE, SHADOW_BASE);     /* "Copy Shadow to AP" */
        goto_application();
    }

    /* 2. AP is good. Make sure Shadow holds a matching backup, then boot AP. */
    if (image_verify((const uint32_t *)AP_BASE) == IMG_OK) {
        if (image_verify((const uint32_t *)SHADOW_BASE) != IMG_OK)
            flash_copy_image(SHADOW_BASE, AP_BASE); /* "Copy AP to Shadow" */
        goto_application();
    }

    /* 3. AP is corrupt but Shadow survived. Recover and boot. */
    if (image_verify((const uint32_t *)SHADOW_BASE) == IMG_OK) {
        flash_copy_image(AP_BASE, SHADOW_BASE);     /* "Copy Shadow to AP" */
        goto_application();
    }

    /* 4. Both banks invalid — run the resident serial-download server. */
    store_boot_flag(0);
    g_loop_mode = 0;
    ota_reset();
    s_keepalive_div = 0;
    uart_tx_string(STR_BANNER);         /* "I am VM-BATT BL"                   */
    dbg_printf(STR_IN_BL);
    uart_tx_flush();

    for (;;) {
        /* bit 0: a download just finalised (mode bit1) -> mirror AP into Shadow */
        if (g_boot_events & 0x01u) {
            g_boot_events &= (uint8_t)~0x01u;
            if (g_loop_mode & 0x02u) {
                dbg_printf(STR_UPGRADE_FIN);
                if (image_verify((const uint32_t *)AP_BASE) == IMG_OK)
                    flash_copy_image(SHADOW_BASE, AP_BASE);  /* "Copy AP to Shadow" */
                g_loop_mode = 0;
                ota_reset();
                s_keepalive_div = 0;
                uart_tx_string(STR_BANNER);
                dbg_printf(STR_IN_BL);
            }
        }

        /* bit 1: ~100-tick keepalive — re-announce the loader to the host */
        if (g_boot_events & 0x02u) {
            g_boot_events &= (uint8_t)~0x02u;
            if (++s_keepalive_div > 99u) {
                s_keepalive_div = 0;
                uart_tx_string(STR_BANNER);
                dbg_printf(STR_IN_BL);
            }
        }

        /* bit 2: pet the watchdog unless a transfer holds the busy lock */
        if (g_boot_events & 0x04u) {
            g_boot_events &= (uint8_t)~0x04u;
            if ((g_loop_mode & 0x01u) == 0u)
                iwdg_refresh();
        }

        /* bit 3: reserved */
        if (g_boot_events & 0x08u)
            g_boot_events &= (uint8_t)~0x08u;

        uart_rx_drain();    /* feed received bytes into ota_process_byte()     */
        uart_tx_pump();     /* push queued reply bytes out USART2              */
    }
}
