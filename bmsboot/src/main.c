/* main.c — bootloader entry, boot decision and resident super-loop.
 *
 * Reconstructed from main (0x08000980). Unlike the STM32F0 sibling
 * powerbankboot (which splits a thin STL main from a boot_main orchestrator),
 * the BMS loader does everything here:
 *
 *   1. boot_hw_init() + led_init(), announce the V007 banner.
 *   2. Act on the persisted EEPROM boot flag:
 *        0xCC -> if Shadow is valid, install Shadow->AP and boot
 *        0x33 -> rewrite the flag to 0x55 (acknowledge) and run the loop
 *        0x5A -> rewrite to 0x55, erase AP + Shadow, run the loop (force DL)
 *        else -> run the loop
 *   3. Resident super-loop driven by SysTick event bits (g_boot_events):
 *        bit2 paces the boot decision (validate AP -> boot, else recover from
 *             Shadow, else wait, counting down g_boot_countdown);
 *        bit6 kicks the watchdog while no transfer is in progress;
 *        every pass drains the RX ring (-> ota_process_byte) and pumps TX.
 *
 * Boot-decision and download work happens only when the comms port was brought
 * up (download_pin_check); a normal unit validates AP on the first bit2 event
 * and jumps straight to the application.
 */
#include "bmsboot.h"

/* Shared bootloader state (OEM SRAM globals; see docs/hardware.md). */
volatile uint8_t g_boot_events;        /* 0x200008BC — posted by SysTick       */
volatile uint8_t g_boot_countdown;     /* 0x2000088C — boot-delay countdown    */
volatile uint8_t g_loop_flags;         /* 0x20000850 — bit0 busy, bit1 done,…  */

static void boot_flag_write(uint8_t v)
{
    flash_program((uint8_t *)BOOT_FLAG_ADDR, 1, &v);
}

int main(void)
{
    boot_hw_init();
    led_init();
    uart_tx_string(STR_BANNER_V007);
    uart_tx_flush();

    g_boot_countdown = 0;
    ota_reset();

    uint8_t flag = REG8(BOOT_FLAG_ADDR);
    if (flag == BOOT_FLAG_STAY) {                       /* 0xCC */
        while (image_verify((const uint32_t *)SHADOW_BASE) == IMG_OK)
            flash_copy_image();                          /* installs + boots */
    } else if (flag == BOOT_FLAG_ACK) {                 /* 0x33 */
        boot_flag_write(BOOT_FLAG_NORMAL);
    } else if (flag == BOOT_FLAG_WIPE) {                /* 0x5A */
        boot_flag_write(BOOT_FLAG_NORMAL);
        flash_erase_page(APP_BASE);
        flash_erase_page(SHADOW_BASE);
    }

    for (;;) {
        if (g_boot_events & 0x01u) g_boot_events &= (uint8_t)~0x01u;
        if (g_boot_events & 0x02u) g_boot_events &= (uint8_t)~0x02u;

        if (g_boot_events & 0x04u) {                    /* bit2: boot decision  */
            g_boot_events &= (uint8_t)~0x04u;
            if (g_boot_countdown == 0) {
                if ((g_loop_flags & 0x02u) == 0) {
                    if ((g_loop_flags & 0x04u) == 0 && (g_loop_flags & 0x01u) == 0) {
                        if (image_verify((const uint32_t *)APP_BASE) == IMG_OK) {
                            boot_flag_write(0);
                            goto_application();           /* boot (no return)    */
                        }
                        while (image_verify((const uint32_t *)SHADOW_BASE) == IMG_OK)
                            flash_copy_image();           /* recover + boot      */
                        flash_erase_page(APP_BASE);
                        g_loop_flags |= 0x04u;
                        boot_flag_write(BOOT_FLAG_NORMAL);
                    }
                    g_boot_countdown = 0xC8u;
                } else {                                  /* upgrade just done   */
                    g_loop_flags &= (uint8_t)~0x02u;
                    if (image_verify((const uint32_t *)APP_BASE) == IMG_OK) {
                        boot_flag_write(0);
                        goto_application();
                    } else {
                        flash_erase_page(APP_BASE);
                        g_loop_flags &= (uint8_t)~0x04u;
                        g_loop_flags &= (uint8_t)~0x01u;
                        uart_tx_string(STR_BANNER_V006);
                        g_boot_countdown = 0xC8u;
                    }
                }
            } else if (--g_boot_countdown == 0) {
                uart_tx_string(STR_BANNER_V006);
            }
        }

        if (g_boot_events & 0x08u) g_boot_events &= (uint8_t)~0x08u;
        if (g_boot_events & 0x10u) g_boot_events &= (uint8_t)~0x10u;
        if (g_boot_events & 0x20u) g_boot_events &= (uint8_t)~0x20u;

        if (g_boot_events & 0x40u) {                    /* bit6: watchdog kick  */
            g_boot_events &= (uint8_t)~0x40u;
            if ((g_loop_flags & 0x01u) == 0 && ota_addr() == 0)
                REG32(IWDG_BASE) = IWDG_KR_RELOAD;
        }

        uart_rx_drain();                                /* -> ota_process_byte  */
        uart_tx_pump();                                 /* push queued replies  */
    }
}
