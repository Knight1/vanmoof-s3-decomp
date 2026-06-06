#ifndef MAINWARE_MAIN_H
#define MAINWARE_MAIN_H

#include <stdint.h>

/*
 * main.h — the mainware application entry + super-loop (OEM `main`, 0x0803DEA8)
 * and the boot/clock bring-up chain (boot_init_cold 0x0803DDE0, boot_init_warm
 * 0x0803DADC, mainware_boot_init_sequence 0x0803FC94).
 *
 * `main` is the bike's top-level loop: after the cold/warm clock + peripheral
 * bring-up it ticks every subsystem once per iteration — lights, modem SIM SM,
 * the shifter/BMS Modbus services, the BLE<->SSP bridges, motor recovery, the
 * LED matrix, status_process (the behaviour engine), the OTA updater, and the
 * telemetry broadcaster — forever. See docs/boot.md for the full map.
 */

/* The application/session context: one flat struct at SRAM 0x200083A8. The same
 * block is reached elsewhere via the holder at 0x20000944 (g_app_ctx_ptr, see
 * app.c) and via g_app_state.ctx_sub (app_state.h, struct session_ctx). main()
 * and the boot init use the absolute base directly, matching the OEM literal
 * pool (DAT_0803E11C). Fields are accessed by byte offset. */
#define G_APP_CTX_ADDR   0x200083A8u

/* Warm-boot marker at the head of SRAM: when *0x20000000 holds this value the
 * RAM image survived (warm/soft reset) and the LSE/RTC is already running, so
 * the clock tree is brought up with LSE instead of LSI. */
#define BOOT_MAGIC_ADDR  0x20000000u
#define BOOT_MAGIC_WARM  0x55AA55CFu

/* Vector table relocation: main() points SCB->VTOR at the app vector table
 * (the muco bootloader's table lives at 0x08000000; ours at the app base). */
#define APP_VECTOR_TABLE 0x08020200u

/* Application entry, called from Reset_Handler after SystemInit + the CRT. */
int main(void);

#endif
