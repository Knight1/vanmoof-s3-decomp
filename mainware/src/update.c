/*
 * update.c — OTA firmware-update orchestrator (subsystem_update_sm).
 *
 * One super-loop-ticked state machine (OEM 0x08031900) that pulls a multi-file
 * PACK package over BLE — the phone forwards it from the cloud — and flashes it
 * into every subsystem on the bike:
 *
 *   mainware.bin    this STM32F413 (self)  -> shadow flash 0x08060000, then a
 *                                             CPU reset (NVICReset) so the muco
 *                                             bootloader bank-swaps on next boot
 *   motorware.bin   motor controller       -> flash 0x080A0000 + bus push
 *   shifterware.bin e-Shifter (MM32F031)   -> flash 0x08010000, bus, Vbat-gated
 *   batteryware.bin battery BMS (STM32L0)  -> flash 0x080C0000, Modbus 0xAA
 *   bleware.bin     BLE co-proc (CC2642)   -> 0x11b/0x11c OAD push
 *
 * The package is a PACK container (pack_validate, docs/oad.md): a header with N
 * "bins", each {name, size, dest offset}. The full state writeup is docs/ota.md.
 *
 * Faithfulness: this is a behaviour-equivalent reconstruction of the live
 * disassembly — exact control flow per state, exact ctx field offsets + access
 * widths, exact rodata log-string literals (resolved from the literal pools),
 * and the SRAM globals pinned at their OEM addresses. The debug-log labels are
 * reproduced verbatim including OEM quirks (the state-1 "Ask Header" timer is
 * named "ble_reboot_tmr"; "Update Main by reboot, wait for BMS shutdown").
 *
 * Two distinct SRAM blocks back the SM:
 *   - g_update_sm   @ 0x20000760  the control struct: state byte at [0], file
 *                                 count [1], file index [2], the request-block
 *                                 scratch at +4, the PACK-header parse buffer at
 *                                 +8, the per-bin image table from +0x1C (0x40
 *                                 stride), flash dest/src/size at +0x1A0/+0x1A4/
 *                                 +0x1A8, erase-start ticks +0x1AC, scan index
 *                                 +0x19E, commit file-index +0x19C, retry +0x19D,
 *                                 the per-bin "claimed" flags from +0x14, BMS
 *                                 sub-state at +0x1B0..+0x1B3.
 *   - g_update_slots @ 0x20000079 a small block of scheduler-slot handles
 *                                 (indices 1..8; 0xFA = unallocated) plus a
 *                                 per-subsystem error/result code at [2].
 */

#include <stdint.h>
#include <string.h>

#include "log.h"        /* g_log_func, log_print_timestamp_prefix */
#include "scheduler.h"  /* scheduler_alloc/start/release/slot_is_idle/set_timer_name */
#include "flash.h"      /* flash_erase, flash_write, pack_validate */
#include "update.h"

/* ── SRAM-resident SM globals (absolute, matching the OEM) ──────────────────*/
extern uint8_t g_update_sm[];      /* control struct @ 0x20000760 */
extern uint8_t g_update_slots[];   /* scheduler-slot handles @ 0x20000079 */

/* The console/IPC fn-ptr table at SRAM 0x20009D98: [0] = g_log_func (the
 * printf-style logger, also used via DAT[2]); [1] = the BMS update-data feed
 * callback (void(void), driven while the bus is ready in state 0x10). */
extern void (*g_update_bms_feed)(void);   /* fn-ptr table entry [1] @ 0x20009D9C */

/* STM32F413 GPIO port bases (AHB1). */
#define GPIOA_BASE  ((void *)0x40020000u)   /* PA12 (0x1000) — BMS update strobe */
#define GPIOB_BASE  ((void *)0x40020400u)   /* PB14 (0x4000) — shifter power on   */

/* SCB Application Interrupt and Reset Control Register (NVICReset). */
#define SCB_AIRCR   (*(volatile uint32_t *)0xE000ED0Cu)
#define AIRCR_NVIC_RESET 0x05FA0004u        /* VECTKEY | SYSRESETREQ */

/* OTA flash destinations (per subsystem). */
#define FLASH_DEST_MAINWARE   0x08060000u   /* self -> shadow bank   */
#define FLASH_DEST_MOTORWARE  0x080A0000u
#define FLASH_DEST_SHIFTERWARE 0x08010000u
#define FLASH_DEST_BATTERYWARE 0x080C0000u

/* PACK source addresses for the post-flash pack_validate per subsystem. */
#define PACK_SRC_MAINWARE     0x08060000u
#define PACK_SRC_MOTORWARE    0x080A0000u
#define PACK_SRC_SHIFTERWARE  0x08010000u
#define PACK_SRC_BATTERYWARE  0x080C0000u

/* ── CubeF4 HAL + newlib (vendored at link time) ───────────────────────────*/
extern void HAL_GPIO_WritePin(void *GPIOx, uint16_t pin_mask, int state);
extern void systick_delay(uint32_t ms);

/* ── codebase helpers (decoded elsewhere) ──────────────────────────────────*/
extern uint8_t ssp_ble_enqueue_tx_packet(uint16_t cmd, uint8_t len, const void *data, char flag);
extern uint32_t maybe_enqueue_tx_message(int a, int b, int c, int d);
extern void    testmode_command_dispatch();        /* variadic-ish in OEM; some sites pass no arg */
extern int     save_state_record_to_eeprom();      /* 15-word record writer */
extern uint8_t update_mode_get(void);
extern void    update_mode_request(char mode);
extern void    watchdog_kick(void);
extern uint32_t systick_now(void);
extern void    state_flag_set(int on);
extern void    wwdg_apb_clk_disable(void);
extern void    wwdg_apb_clk_enable(void);
extern int     download_chunks_pending_count(void);
extern int     shifter_update_status_get(void);
extern void    shifter_update_request(int req);
extern int     batteryware_update_status_get(void);
extern void    batteryware_update_set_pending(void);
extern void    batteryware_update_arm(void);
extern int     modbus_bat_submit(const void *frame);
extern int     bus_rx_byte_locked(uint32_t handle);

/* ARM CMSIS data-synchronisation barrier (OEM helper at 0x08023304). */
extern void DataSynchronizationBarrier(int opt);

/* The handle/buffer bus_rx_byte_locked() reads from in state 0x10 (SRAM
 * 0x20000913 — passed by value as in the OEM). */
#define BUS_RX_HANDLE  0x20000913u

void subsystem_update_sm(int param_1)
{
    uint8_t *ctx = (uint8_t *)param_1;
    uint8_t *sm  = g_update_sm;
    uint8_t *slot = g_update_slots;

    /* Modbus-to-BMS frame scratch (state 0x10). */
    uint8_t  mb_frame[0x88];
    /* request-block / single-byte SSP scratch + pack_validate out header. */
    uint8_t  ssp_one;
    uint32_t pack_hdr[4];

    switch (sm[0]) {
    case 1:   /* ASK HEADER */
        if (slot[0] == SCHED_SLOT_NONE) {
            slot[0] = scheduler_alloc();
            scheduler_set_timer_name(slot[0], 10000, "ble_reboot_tmr");
            scheduler_start(slot[0], 10000, (sched_cb_t)0);
        }
        if (scheduler_slot_is_idle(slot[0]) != 0) {
            scheduler_release(&slot[0]);
            sm[0] = 2;
            log_print_timestamp_prefix();
            g_log_func("End BLE reboot wait\r\n");
        }
        break;

    case 2:   /* HEADER WAIT done */
        log_print_timestamp_prefix();
        g_log_func("Ask Header\r\n");
        *(uint32_t *)(ctx + 0x32c) = 0;
        *(uint16_t *)(ctx + 0x330) = 0;
        if (slot[1] == SCHED_SLOT_NONE) {
            slot[1] = scheduler_alloc();
            scheduler_set_timer_name(slot[1], 5000, "fw_timeout_tmr");
            scheduler_start(slot[1], 5000, (sched_cb_t)0);
        }
        ctx[0x2dc] = 0;
        sm[1] = 0;
        sm[2] = 0;
        *(uint32_t *)(sm + 4) = 0;
        if (ssp_ble_enqueue_tx_packet(0x104, 4, sm + 4, '\0') > 0x80) {
            g_log_func("  ERROR SSPB place\r\n");
        }
        sm[0] = 3;
        break;

    case 3:   /* GET HEADER */
        if (scheduler_slot_is_idle(slot[1]) != 0) {
            log_print_timestamp_prefix();
            g_log_func("Get header timeout\r\n");
            testmode_command_dispatch(2);
            sm[0] = 0x15;
        }
        if (ctx[0x2dc] != 0) {
            uint32_t *hdr = (uint32_t *)(sm + 8);
            hdr[0] = *(uint32_t *)(ctx + 0x1dc);
            hdr[1] = *(uint32_t *)(ctx + 0x1e0);
            hdr[2] = *(uint32_t *)(ctx + 0x1e4);
            if (memcmp(hdr, "PACK", 4) == 0) {
                hdr[3] = 0;
                *(uint32_t *)((uint8_t *)hdr + 0xf) = 0;
                g_log_func("Offset: 0x%08X\r\n", hdr[1]);
                g_log_func("Size  : 0x%08X\r\n", hdr[2]);
                *((uint8_t *)hdr - 7) = (uint8_t)(hdr[2] >> 6);  /* sm[1] = bin count */
                g_log_func("  Number of bins %d\r\n");
                *((uint8_t *)(hdr - 2)) = 4;                     /* sm[0] = 4 */
                testmode_command_dispatch(0);
            } else {
                g_log_func("Invalid packet header\r\n");
                testmode_command_dispatch(3);
                sm[0] = 0x15;
            }
        }
        break;

    case 4:   /* ASK BLOCK */
        ctx[0x2dc] = 0;
        slot[2] = 1;
        *(uint32_t *)(sm + 4) = *(uint32_t *)(sm + 0xc) + (uint32_t)sm[2] * 0x40;
        if (ssp_ble_enqueue_tx_packet(0x104, 4, sm + 4, '\0') > 0x80) {
            g_log_func("  ERROR SSPB place\r\n");
        }
        scheduler_start(slot[1], 5000, (sched_cb_t)0);
        sm[0] = 6;
        break;

    case 5:   /* SCAN FILES */
        sm[0] = 0x16;
        sm[0x19c] = 0;
        sm[0x19d] = 0;
        sm[0x19e] = 0;
        for (;;) {
            uint32_t i = sm[0x19e];
            uint8_t *bin;
            if (i >= sm[1]) {
                break;
            }
            bin = sm + 0x1c + i * 0x40;
            if (memcmp(bin, "mainware.bin", 2) == 0 && sm[i + 0x14] == 0) {
                ctx[0x32c + i] = 2;
                sm[i + 0x14] = 1;
                *(uint32_t *)(sm + 4) = *(uint32_t *)(sm + i * 0x40 + 0x54);
                *(uint32_t *)(sm + 0x1a0) =
                    *(uint32_t *)(sm + i * 0x40 + 0x54) + *(uint32_t *)(sm + i * 0x40 + 0x58);
                *(uint32_t *)(sm + 0x1a4) = FLASH_DEST_MAINWARE;
                *(uint32_t *)(sm + 0x1a8) = 0x40000;
                g_log_func("Process: %s %d bytes on 0x%08X\r\n", bin);
                sm[0] = 7;
                return;
            }
            if (memcmp(bin, "motorware.bin", 2) == 0 && sm[i + 0x14] == 0) {
                ctx[0x32c + i] = 3;
                sm[i + 0x14] = 1;
                *(uint32_t *)(sm + 4) = *(uint32_t *)(sm + i * 0x40 + 0x54);
                *(uint32_t *)(sm + 0x1a0) =
                    *(uint32_t *)(sm + i * 0x40 + 0x54) + *(uint32_t *)(sm + i * 0x40 + 0x58);
                *(uint32_t *)(sm + 0x1a4) = FLASH_DEST_MOTORWARE;
                *(uint32_t *)(sm + 0x1a8) = 0x20000;
                g_log_func("Process: %s %d bytes on 0x%08X\r\n", bin);
                sm[0] = 7;
                return;
            }
            if (memcmp(bin, "shifterware.bin", 2) == 0 && sm[i + 0x14] == 0) {
                log_print_timestamp_prefix();
                g_log_func("Shifter power on\r\n");
                HAL_GPIO_WritePin(GPIOB_BASE, 0x4000, 1);
                ctx[0x334] = 0;
                i = sm[0x19e];
                ctx[0x32c + i] = 4;
                sm[i + 0x14] = 1;
                *(uint32_t *)(sm + 4) = *(uint32_t *)(sm + i * 0x40 + 0x54);
                *(uint32_t *)(sm + 0x1a0) =
                    *(uint32_t *)(sm + i * 0x40 + 0x54) + *(uint32_t *)(sm + i * 0x40 + 0x58);
                *(uint32_t *)(sm + 0x1a4) = FLASH_DEST_SHIFTERWARE;
                *(uint32_t *)(sm + 0x1a8) = 0x10000;
                g_log_func("Process: %s %d bytes on 0x%08X\r\n", sm + i * 0x40 + 0x1c);
                sm[0] = 7;
                return;
            }
            if (memcmp(bin, "batteryware.bin", 2) == 0 && sm[i + 0x14] == 0) {
                ctx[0x32c + i] = 5;
                sm[i + 0x14] = 1;
                *(uint32_t *)(sm + 4) = *(uint32_t *)(sm + i * 0x40 + 0x54);
                *(uint32_t *)(sm + 0x1a0) =
                    *(uint32_t *)(sm + i * 0x40 + 0x54) + *(uint32_t *)(sm + i * 0x40 + 0x58);
                *(uint32_t *)(sm + 0x1a4) = FLASH_DEST_BATTERYWARE;
                *(uint32_t *)(sm + 0x1a8) = 0x20000;
                g_log_func("Process: %s %d bytes on 0x%08X\r\n", bin);
                sm[0] = 7;
                return;
            }
            if (memcmp(bin, "bleware.bin", 2) == 0 && sm[i + 0x14] == 0) {
                ctx[0x32c + i] = 1;
                sm[i + 0x14] = 1;
                *(uint32_t *)(sm + 4) = *(uint32_t *)(sm + i * 0x40 + 0x54);
                *(uint32_t *)(sm + 0x1a0) =
                    *(uint32_t *)(sm + i * 0x40 + 0x54) + *(uint32_t *)(sm + i * 0x40 + 0x58);
                g_log_func("Info: %s %d bytes on 0x%08X\r\n", bin);
            }
            sm[0x19e] = sm[0x19e] + 1;
        }
        break;

    case 6:   /* RECV BLOCK */
        if (scheduler_slot_is_idle(slot[1]) != 0) {
            log_print_timestamp_prefix();
            g_log_func("Get info timeout\r\n");
            testmode_command_dispatch(2);
            sm[0] = 0x15;
        }
        if (ctx[0x2dc] != 0) {
            uint32_t idx = sm[2];
            const uint32_t *src = (const uint32_t *)(ctx + 0x1dc);
            uint32_t *dst = (uint32_t *)(sm + idx * 0x40 + 0x1c);
            do {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
                src += 4;
                dst += 4;
            } while (src != (const uint32_t *)(ctx + 0x21c));
            g_log_func("%s %d bytes on 0x%08X\r\n",
                       sm + idx * 0x40 + 0x1c,
                       *(uint32_t *)(sm + idx * 0x40 + 0x58),
                       *(uint32_t *)(sm + idx * 0x40 + 0x54));
            {
                uint8_t next = (uint8_t)(sm[2] + 1);
                sm[2] = next;
                if (next < sm[1]) {
                    sm[0] = 4;
                } else {
                    sm[0] = 5;
                }
            }
        }
        break;

    case 7: {  /* ERASE */
        int err;
        g_log_func("Erasing shadow flash %d Kb\r\n", *(uint32_t *)(sm + 0x1a8) >> 10);
        wwdg_apb_clk_disable();
        err = flash_erase(*(int *)(sm + 0x1a4), *(int *)(sm + 0x1a8));
        if (err == 0) {
            wwdg_apb_clk_enable();
            watchdog_kick();
            g_log_func("OK\r\n");
            sm[0] = 8;
            *(uint32_t *)(sm + 0x1ac) = systick_now();
        } else {
            wwdg_apb_clk_enable();
            watchdog_kick();
            g_log_func("ERROR %d\r\n", err);
            testmode_command_dispatch(5);
            sm[0] = 0x15;
        }
        break;
    }

    case 8:   /* DL WAIT */
        if (download_chunks_pending_count() == 0x80) {
            sm[0] = 9;
        }
        break;

    case 9:   /* ASK DATA */
        if (ssp_ble_enqueue_tx_packet(0x104, 4, sm + 4, '\0') > 0x80) {
            g_log_func("  ERROR SSPB place\r\n");
        }
        ctx[0x2dc] = 0;
        scheduler_start(slot[1], 5000, (sched_cb_t)0);
        sm[0] = 10;
        break;

    case 10:  /* WRITE */
        if (scheduler_slot_is_idle(slot[1]) != 0) {
            log_print_timestamp_prefix();
            g_log_func("Get block timeout\r\n");
            testmode_command_dispatch(2);
            sm[0] = 0x15;
        }
        if (ctx[0x2dc] != 0) {
            int err = flash_write(*(uint32_t *)(sm + 0x1a4), (const uint32_t *)(ctx + 0x1dc), 0xff);
            if (err == 0) {
                if (*(uint32_t *)(sm + 4) < *(uint32_t *)(sm + 0x1a0)) {
                    sm[0] = 8;
                } else {
                    uint32_t size = *(uint32_t *)(sm + sm[0x19e] * 0x40 + 0x58);
                    uint32_t now = systick_now();
                    uint32_t elapsed = now - *(uint32_t *)(sm + 0x1ac);
                    /* bytes/sec via the /1000 magic-multiply (0x10624DD3 >> 38). */
                    g_log_func("\r\nWritten %d bytes/sec\r\n",
                               size / (uint32_t)(((uint64_t)0x10624DD3u * elapsed) >> 0x26));
                    sm[0] = 5;
                }
                state_flag_set(0);
                g_log_func("Write 0x%08X\r", *(uint32_t *)(sm + 0x1a4));
                state_flag_set(1);
                *(uint32_t *)(sm + 4) += 0x100;
                *(uint32_t *)(sm + 0x1a4) += 0x100;
            } else {
                log_print_timestamp_prefix();
                g_log_func("Flash write error %d\r\n", err);
                testmode_command_dispatch(6);
                sm[0] = 0x15;
            }
        }
        break;

    case 0xb:   /* SHIFTER push: start OAD */
        if ((int8_t)slot[6] == (int8_t)0xfa) {
            slot[6] = scheduler_alloc();
            scheduler_start(slot[6], 1000, (sched_cb_t)0);
            g_log_func("Disable Advertise\r\n");
            ssp_one = 0;
            if (ssp_ble_enqueue_tx_packet(0x110, 1, &ssp_one, '\0') > 0x80) {
                g_log_func("  ERROR SSP place\r\n");
            }
        }
        if (scheduler_slot_is_idle(slot[6]) != 0 &&
            (*(int16_t *)(ctx + 0x402) == 1 || *(int16_t *)(ctx + 0x400) == 1)) {
            scheduler_release(&slot[6]);
            sm[0] = 0xc;
            update_mode_request('\x04');
            g_log_func("Start motor update..");
        }
        break;

    case 0xc:   /* SHIFTER push: confirm mode */
        if (update_mode_get() == 0xc || update_mode_get() == 0xd) {
            ssp_one = 1;
            if (ssp_ble_enqueue_tx_packet(0x110, 1, &ssp_one, '\0') > 0x80) {
                g_log_func("  ERROR SSP place\r\n");
            }
            if (update_mode_get() == 0xc) {
                g_log_func("Update Ok\r\n");
                sm[0] = 0xd;
                if ((int8_t)slot[4] == (int8_t)0xfa) {
                    slot[4] = scheduler_alloc();
                }
                scheduler_set_timer_name(slot[4], 3000, "ask_motor_ver_tmr");
                scheduler_start(slot[4], 3000, (sched_cb_t)0);
            } else {
                g_log_func("Motorware Update ERR\r\n");
                slot[2] = 8;
                sm[0] = 0x16;
            }
        }
        break;

    case 0xd:   /* SHIFTER push: enqueue bus message */
        if (scheduler_slot_is_idle(slot[4]) != 0) {
            scheduler_release(&slot[4]);
            g_log_func("Ask motor version\r\n");
            if (maybe_enqueue_tx_message(10, 0, 0, 1) > 0x10) {
                g_log_func("  ERROR SSP place\r\n");
            }
            sm[0] = 0x16;
        }
        break;

    case 0xe:   /* SHIFTER wait */
        if (shifter_update_status_get() == 1 || shifter_update_status_get() == 3) {
            sm[0] = 0x16;
        }
        if (shifter_update_status_get() == 2) {
            g_log_func("Shifter Update ERR\r\n");
            sm[0] = 0x16;
            slot[2] = 10;
        }
        break;

    case 0xf:   /* BMS arm */
        if (batteryware_update_status_get() == 1) {
            if ((int8_t)slot[7] == (int8_t)0xfa) {
                slot[7] = scheduler_alloc();
                scheduler_start(slot[7], 100000, (sched_cb_t)0);
            }
            *(uint16_t *)(ctx + 0x3f2) = 0;
            sm[0x1b1] = 1;
            sm[0x1b2] = 0xc;
            ctx[0x32b + sm[0x19c]] = 6;
            if (save_state_record_to_eeprom(
                    *(uint32_t *)(ctx + 0x310), *(uint32_t *)(ctx + 0x314),
                    *(uint32_t *)(ctx + 0x318), *(uint32_t *)(ctx + 0x31c),
                    *(uint32_t *)(ctx + 0x320), *(uint32_t *)(ctx + 0x324),
                    *(uint32_t *)(ctx + 0x328), *(uint32_t *)(ctx + 0x32c),
                    *(uint32_t *)(ctx + 0x330), *(uint32_t *)(ctx + 0x334),
                    *(uint32_t *)(ctx + 0x338), *(uint32_t *)(ctx + 0x33c),
                    *(uint32_t *)(ctx + 0x340), *(uint32_t *)(ctx + 0x344),
                    *(uint32_t *)(ctx + 0x348)) != 0) {
                g_log_func(" ERROR Save values\r\n");
            }
            sm[0] = 0x10;
        }
        if (batteryware_update_status_get() == 2) {
            ssp_one = 1;
            if (ssp_ble_enqueue_tx_packet(0x110, 1, &ssp_one, '\0') > 0x80) {
                g_log_func("  ERROR SSP place\r\n");
            }
            g_log_func("Battery Update ERR\r\n");
            slot[2] = 9;
            sm[0] = 0x16;
        }
        break;

    case 0x10:  /* BMS update over Modbus slave 0xAA */
        if (sm[0x1b1] != 0 && bus_rx_byte_locked(BUS_RX_HANDLE) != 0) {
            scheduler_start(slot[7], 1000, (sched_cb_t)0);
            if ((uint8_t)(sm[0x1b3] - 6) < 0x78) {
                g_update_bms_feed();
            }
        }
        if (scheduler_slot_is_idle(slot[7]) != 0) {
            sm[0x1b1] = 0;
            sm[0x1b2] = (uint8_t)(sm[0x1b2] - 1);
            if (sm[0x1b2] == 0) {
                g_log_func("BMS fail to read ID\r\n");
                slot[2] = 2;
                sm[0] = 0x16;
            } else {
                log_print_timestamp_prefix();
                g_log_func("BMS ask ID\r\n");
                mb_frame[0] = 0xaa;
                mb_frame[1] = 3;
                *(uint16_t *)(mb_frame + 2) = 0;
                mb_frame[0x84] = 1;
                if (modbus_bat_submit(mb_frame) != 0) {
                    g_log_func("  MB Error\r\n");
                }
                scheduler_start(slot[7], 10000, (sched_cb_t)0);
            }
        }
        if (*(int16_t *)(ctx + 0x3f2) == 0x100) {
            scheduler_release(&slot[7]);
            g_log_func("BMS ID ok\r\n");
            mb_frame[0] = 0xaa;
            mb_frame[1] = 6;
            *(uint16_t *)(mb_frame + 2) = 8;
            mb_frame[0x84] = 1;
            mb_frame[4] = 1;
            mb_frame[5] = 0;
            if (modbus_bat_submit(mb_frame) != 0) {
                g_log_func("  MB Error\r\n");
            }
            ssp_one = 1;
            if (ssp_ble_enqueue_tx_packet(0x110, 1, &ssp_one, '\0') > 0x80) {
                g_log_func("  ERROR SSP place\r\n");
            }
            ctx[0x32b + sm[0x19c]] = 6;
            if (save_state_record_to_eeprom(
                    *(uint32_t *)(ctx + 0x310), *(uint32_t *)(ctx + 0x314),
                    *(uint32_t *)(ctx + 0x318), *(uint32_t *)(ctx + 0x31c),
                    *(uint32_t *)(ctx + 0x320), *(uint32_t *)(ctx + 0x324),
                    *(uint32_t *)(ctx + 0x328), *(uint32_t *)(ctx + 0x32c),
                    *(uint32_t *)(ctx + 0x330), *(uint32_t *)(ctx + 0x334),
                    *(uint32_t *)(ctx + 0x338), *(uint32_t *)(ctx + 0x33c),
                    *(uint32_t *)(ctx + 0x340), *(uint32_t *)(ctx + 0x344),
                    *(uint32_t *)(ctx + 0x348)) != 0) {
                g_log_func(" ERROR Save values\r\n");
            }
            HAL_GPIO_WritePin(GPIOA_BASE, 0x1000, 1);
            sm[0] = 0x16;
        }
        break;

    case 0x11:  /* POWERBANK / BLE push: image transfer start */
        g_log_func("Start BLE update\r\n");
        if (ssp_ble_enqueue_tx_packet(0x11b, 0, (void *)0, '\x01') > 0x80) {
            g_log_func("  ERROR SSPB place\r\n");
        }
        ctx[0x37c] = 0;
        if ((int8_t)slot[8] == (int8_t)0xfa) {
            slot[8] = scheduler_alloc();
            scheduler_start(slot[8], 15000, (sched_cb_t)0);
        }
        sm[0] = 0x12;
        break;

    case 0x12:  /* BLE push: send image */
        if (scheduler_slot_is_idle(slot[8]) != 0) {
            scheduler_release(&slot[8]);
            g_log_func("ERR ble ask timeout\r\n");
            testmode_command_dispatch(0xb);
            sm[0] = 0x15;
        }
        if (ctx[0x37c] != 0) {
            g_log_func("flash version: 0x%08X\r\n", *(uint32_t *)(ctx + 0x380));
            ssp_one = 1;
            g_log_func("Trigger update\r\n");
            *(uint32_t *)(ctx + 0x38c) = 0;
            if (ssp_ble_enqueue_tx_packet(0x11c, 1, &ssp_one, '\0') > 0x80) {
                g_log_func("  ERROR SSPB place\r\n");
            }
            scheduler_start(slot[8], 15000, (sched_cb_t)0);
            sm[0] = 0x13;
        }
        break;

    case 0x13:  /* BLE push: confirm */
        if (*(int *)(ctx + 0x38c) == *(int *)(ctx + 0x380) && *(int *)(ctx + 0x38c) != 0) {
            g_log_func("BLE update ok\r\n");
            sm[0] = 0x16;
        }
        if (scheduler_slot_is_idle(slot[8]) != 0) {
            scheduler_release(&slot[8]);
            g_log_func("Timeout 0x%08X 0x%08X\r\n",
                       *(uint32_t *)(ctx + 0x380), *(uint32_t *)(ctx + 0x38c));
            testmode_command_dispatch(0xb);
            sm[0] = 0x15;
        }
        break;

    case 0x14:  /* ERROR */
        g_log_func("\r\n ERR CRC\r\n");
        testmode_command_dispatch(7);
        sm[0] = 0x15;
        break;

    case 0x15:  /* CLEANUP */
        scheduler_release(&slot[1]);
        g_log_func("\r\nStopped with ERROR\r\n");
        sm[0] = 0;
        break;

    case 0x16:  /* FINALIZE / per-target commit */
        scheduler_release(&slot[1]);
        if (sm[0x1b0] == 0) {
            sm[0x1b0] = 1;
            g_log_func("Seq %d Order ", sm[0x19c]);
            for (uint32_t i = 0; i < 5; i = (uint8_t)(i + 1)) {
                g_log_func("%d ", ctx[0x32c + i]);
            }
            g_log_func("\r\n");
        }
        {
            uint8_t fi = sm[0x19c];
            switch (ctx[0x32c + fi]) {
            case 0:
                if (fi == 0) {
                    g_log_func("No packet found\r\n");
                    testmode_command_dispatch(4);
                    sm[0] = 0x15;
                } else {
                    if (slot[2] == 1) {
                        testmode_command_dispatch();
                    } else {
                        testmode_command_dispatch();
                    }
                    sm[0] = 0;
                }
                break;

            case 1:   /* bleware -> push over BLE OAD */
                sm[0x19c] = (uint8_t)(fi + 1);
                sm[0] = 0x11;
                break;

            case 2:   /* mainware self -> validate shadow + reboot */
                if (pack_validate(pack_hdr, (uint32_t *)PACK_SRC_MAINWARE) == 0) {
                    if ((int8_t)slot[3] == (int8_t)0xfa) {
                        g_log_func("Update Main by reboot, wait for BMS shutdown\r\n");
                        if (save_state_record_to_eeprom(
                                *(uint32_t *)(ctx + 0x310), *(uint32_t *)(ctx + 0x314),
                                *(uint32_t *)(ctx + 0x318), *(uint32_t *)(ctx + 0x31c),
                                *(uint32_t *)(ctx + 0x320), *(uint32_t *)(ctx + 0x324),
                                *(uint32_t *)(ctx + 0x328), *(uint32_t *)(ctx + 0x32c),
                                *(uint32_t *)(ctx + 0x330), *(uint32_t *)(ctx + 0x334),
                                *(uint32_t *)(ctx + 0x338), *(uint32_t *)(ctx + 0x33c),
                                *(uint32_t *)(ctx + 0x340), *(uint32_t *)(ctx + 0x344),
                                *(uint32_t *)(ctx + 0x348)) != 0) {
                            g_log_func(" ERROR Save values\r\n");
                        }
                        slot[3] = scheduler_alloc();
                        scheduler_start(slot[3], 800, (sched_cb_t)0);
                        batteryware_update_set_pending();
                    }
                    if (scheduler_slot_is_idle(slot[3]) != 0) {
                        g_log_func("NVICReset\r\n");
                        systick_delay(10);
                        DataSynchronizationBarrier(0xf);
                        SCB_AIRCR = AIRCR_NVIC_RESET | (SCB_AIRCR & 0x700);
                        DataSynchronizationBarrier(0xf);
                        do {
                            /* spin until the reset takes effect */
                        } while (1);
                    }
                } else {
                    sm[0] = 0x14;
                }
                break;

            case 3:   /* motorware -> validate + version compare */
                if ((int8_t)slot[4] == (int8_t)0xfa) {
                    slot[4] = scheduler_alloc();
                    scheduler_set_timer_name(slot[4], 5000, "ask_motor_ver_tmr");
                    scheduler_start(slot[4], 5000, (sched_cb_t)0);
                }
                if ((*(uint32_t *)(ctx + 0x388) & 0xffffff) != 0 ||
                    scheduler_slot_is_idle(slot[4]) != 0) {
                    scheduler_release(&slot[4]);
                    if (pack_validate(pack_hdr, (uint32_t *)PACK_SRC_MOTORWARE) == 0) {
                        sm[0x19c] = (uint8_t)(sm[0x19c] + 1);
                        if ((*(uint32_t *)(ctx + 0x388) & 0xffffff) == (pack_hdr[1] >> 8)) {
                            g_log_func("No need for motorware update\r\n");
                            sm[0] = 0x16;
                        } else {
                            sm[0] = 0xb;
                        }
                    } else {
                        slot[2] = 0x14;
                        sm[0] = 0x16;
                    }
                }
                break;

            case 4:   /* shifterware -> Vbat gate + validate + version compare */
                if ((int8_t)slot[5] == (int8_t)0xfa) {
                    slot[5] = scheduler_alloc();
                    scheduler_set_timer_name(slot[5], 8000, "wait_for_bms_tmr");
                    scheduler_start(slot[5], 8000, (sched_cb_t)0);
                }
                if (*(uint16_t *)(ctx + 0x3b0) > 25000 &&
                    scheduler_slot_is_idle(slot[5]) != 0) {
                    scheduler_release(&slot[5]);
                    if (pack_validate(pack_hdr, (uint32_t *)PACK_SRC_SHIFTERWARE) == 0) {
                        sm[0x19c] = (uint8_t)(sm[0x19c] + 1);
                        if ((uint16_t)(pack_hdr[1] >> 16) == *(uint16_t *)(ctx + 0x336)) {
                            g_log_func("No need for Shifter update\r\n");
                            sm[0] = 0x16;
                        } else {
                            shifter_update_request(2);
                            sm[0] = 0xe;
                        }
                    } else {
                        slot[2] = 0x14;
                        sm[0] = 0x16;
                    }
                }
                if (scheduler_slot_is_idle(slot[5]) != 0) {
                    scheduler_release(&slot[5]);
                    slot[2] = 10;
                    sm[0] = 0x16;
                    log_print_timestamp_prefix();
                    g_log_func("Shifter update no vbat (%d)\r", *(uint16_t *)(ctx + 0x3b0));
                    sm[0x19d] = (uint8_t)(sm[0x19d] + 1);
                    if (sm[0x19d] > 3) {
                        sm[0x19d] = 0;
                        testmode_command_dispatch(10);
                        sm[0] = 0x15;
                    }
                }
                break;

            case 5:   /* batteryware -> Vbat gate + validate + version compare */
                if ((int8_t)slot[5] == (int8_t)0xfa) {
                    slot[5] = scheduler_alloc();
                    scheduler_set_timer_name(slot[5], 5000, "wait_for_bms_tmr");
                    scheduler_start(slot[5], 5000, (sched_cb_t)0);
                }
                if (*(uint16_t *)(ctx + 0x3b0) < 0x61a9 || *(int16_t *)(ctx + 0x408) == 0) {
                    if (scheduler_slot_is_idle(slot[5]) != 0) {
                        scheduler_release(&slot[5]);
                        slot[2] = 9;
                        sm[0] = 0x16;
                        log_print_timestamp_prefix();
                        g_log_func("BMS update no vbat\r\n");
                        sm[0x19d] = (uint8_t)(sm[0x19d] + 1);
                        if (sm[0x19d] > 3) {
                            sm[0x19d] = 0;
                            testmode_command_dispatch(9);
                            sm[0] = 0x15;
                        }
                    }
                } else {
                    scheduler_release(&slot[5]);
                    if (pack_validate(pack_hdr, (uint32_t *)PACK_SRC_BATTERYWARE) == 0) {
                        sm[0x19c] = (uint8_t)(sm[0x19c] + 1);
                        if ((uint32_t)(uint16_t)(pack_hdr[1] >> 16) ==
                            (uint32_t)*(int16_t *)(ctx + 0x408)) {
                            g_log_func("No need for BMS update\r\n");
                            sm[0] = 0x16;
                        } else {
                            batteryware_update_arm();
                            ssp_one = 0;
                            if (ssp_ble_enqueue_tx_packet(0x110, 1, &ssp_one, '\0') > 0x80) {
                                g_log_func("  ERROR SSP place\r\n");
                            }
                            sm[0] = 0xf;
                        }
                    } else {
                        slot[2] = 0x14;
                        sm[0] = 0x16;
                    }
                }
                break;
            }
        }
        break;
    }
}

/* update_sm_is_idle (OEM 0x08032980) — the OTA control state byte (g_update_sm[0])
 * is 0 when no transfer is in progress. */
int update_sm_is_idle(void)
{
    return g_update_sm[0] == 0;
}

/* update_sm_request_start (OEM 0x08032990) — kick the OTA machine into the "start"
 * state (2) if idle; otherwise complain (the app tried to start twice). */
void update_sm_request_start(void)
{
    if (g_update_sm[0] == 0) {
        g_update_sm[0] = 2;
    } else {
        g_log_func("Update was already started\r\n");
    }
}

/* update_sm_kickoff (OEM 0x080329B8) — force the OTA machine to state 1. */
void update_sm_kickoff(void)
{
    g_update_sm[0] = 1;
}

/* testmode_enter_state_16 (OEM 0x080329C4) — enter OTA/test state 0x16, persist the
 * state record (testmode_command_dispatch(0)), then stash the caller's byte at
 * g_update_sm[0x19C]. */
void testmode_enter_state_16(uint8_t param)
{
    g_update_sm[0] = 0x16;
    testmode_command_dispatch(0);
    g_update_sm[0x19c] = param;
}

/* testmode_continue_state_10 (OEM 0x080329E0) — log "Continue %d", enter OTA/test
 * state 0x10 (with a state-record save) and stash the byte at g_update_sm[0x19C]. */
void testmode_continue_state_10(int param_1)
{
    g_log_func("Continue %d\r\n", param_1);
    g_update_sm[0] = 0x10;
    testmode_command_dispatch(0);
    g_update_sm[0x19c] = (uint8_t)param_1;
}
