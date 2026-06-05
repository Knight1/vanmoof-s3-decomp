/*
 * ble.c — BLE app-command write surface (ble_cmd_dispatch).
 *
 * The phone app controls the bike over BLE; the bleware MCU (CC2642) bridges
 * the GATT writes onto the inter-module bus, and mainware lands them here. This
 * is a big switch on the 16-bit command id (OEM 0x08033970): 0x55xx ids are the
 * GATT characteristic writes (id == service + char-index + 1 — see
 * docs/ble-uuids.md), the low ids (0x0A..0x123) are internal/provisioning.
 *
 * The read twin is ble_read_request_dispatch in ble_read.c. The full
 * command->action map is docs/ble-commands.md; the lock/alarm states these
 * commands drive are docs/state-machine.md.
 *
 * Faithfulness: control-flow shape, ctx field offsets + access widths, helper
 * calls, state transitions, the config-persist / EEPROM-record idioms and the
 * enqueue thresholds were all verified against the live disassembly. The
 * debug-log string *text* is a best-effort resolution of the OEM rodata
 * pointers — it is cosmetic (g_log_func is a printf-style fn ptr with no
 * behavioural effect); where a literal reads oddly it is the OEM string or a
 * close resolution, not a behavioural change.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "log.h"       /* g_log_func — printf-style fn ptr @ SRAM 0x20009D98 */
#include "scheduler.h" /* scheduler_alloc / scheduler_start / sched_cb_t */

/* ── globals (absolute SRAM, matching the OEM) ─────────────────────────────
 * The session-context pointer lives in a holder cell at 0x20000944; deref once
 * per call to reach the struct base. The byte at holder+4 (0x20000948) is a
 * SEPARATE global — the eShifter-busy latch — NOT a ctx field (the OEM indexes
 * it off the holder, a classic decompiler trap). */
#define APP_CTX_HOLDER   (*(uint8_t **)0x20000944u)
#define g_eshifter_busy  (*(volatile uint8_t *)0x20000948u)

/* case 0x5564 wheel-size 1 s task scheduler-slot handle (0xFA = unallocated). */
#define g_wheelsize_timer (*(volatile uint8_t *)0x20000082u)

/* STM32F413 GPIO port bases (AHB1). */
#define GPIOA_BASE  ((void *)0x40020000u)
#define GPIOB_BASE  ((void *)0x40020400u)   /* PB9 (0x200) — wheel-size strobe   */
#define GPIOC_BASE  ((void *)0x40020800u)   /* PC8 (0x100) — lock-pin sense      */
#define GPIOE_BASE  ((void *)0x40020C00u)   /* PE5 (0x20)  — BLE-chip reset line  */
#define GPIOG_BASE  ((void *)0x40021000u)   /* PG3 (0x08)  — wheel-size select    */

/* ── CubeF4 HAL + newlib (vendored at link time) ───────────────────────────*/
extern void HAL_GPIO_WritePin(void *GPIOx, uint16_t pin_mask, int state);
extern int  HAL_GPIO_ReadPin(void *GPIOx, uint16_t pin_mask);
extern void systick_delay(uint32_t ms);

/* ── codebase helpers (decoded elsewhere) ──────────────────────────────────*/
extern void    log_print_timestamp_prefix(void);
extern void    channel_notify_with_status(int status);
extern void    enter_mode3_arm_show_timer(void);
extern void    config_persist_dual_bank(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
extern int     save_state_record_to_eeprom(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
extern uint8_t ssp_ble_enqueue_tx_packet(uint16_t cmd, uint8_t len, const void *data, char flag);
extern void    maybe_set_state_if_unlocked(char state);
extern uint8_t maybe_get_bike_state(void);
extern void    set_mode_state_byte(int mode);
extern void    maybe_set_pending_request(const void *req);
extern void    reset_dual_buffers_and_flags(void);

/* Display/sound request descriptors in flash (passed by pointer to
 * maybe_set_pending_request); kept as opaque externs at their OEM addresses. */
extern const uint8_t REQ_ALARM_OFF[];      /* 0x08046708 */
extern const uint8_t REQ_ALARM_ON[];       /* 0x08046744 */
extern const uint8_t REQ_UNITS_METRIC[];   /* 0x08046634 */
extern const uint8_t REQ_UNITS_IMPERIAL[]; /* 0x080468E4 */
extern const uint8_t REQ_REGION_EU[];      /* 0x08046760 */
extern const uint8_t REQ_REGION_US[];      /* 0x080467D8 */
extern const uint8_t REQ_REGION_JP[];      /* 0x080467B4 */
extern const uint8_t REQ_REGION_OFFROAD[]; /* 0x08046780 */
extern const uint8_t REQ_SOUND_SELECT[];   /* 0x080464EC */
extern const uint8_t REQ_CODE_UP[];        /* 0x08046820 */
extern const uint8_t REQ_CODE_DOWN[];      /* 0x0804684C */
extern const uint8_t REQ_HORN_0A[];        /* 0x08046720 */
extern const uint8_t REQ_HORN_16[];        /* 0x080467A0 */
extern const uint8_t REQ_HORN_17[];        /* 0x080467CC */
extern const uint8_t REQ_HORN_18[];        /* 0x08046794 */
extern const uint8_t REQ_LIGHT_ON[];       /* 0x08046810 */
extern const uint8_t REQ_LIGHT_OFF[];      /* 0x0804687C */
extern const uint8_t REQ_LIGHT_AUTO[];     /* 0x080467E4 */

/* case 0x5564 1 s wheel-size poll task body (OEM 0x08033935, Thumb). */
extern void FUN_08033934(void);   /* matches sched_cb_t (void(void)) */

/* ── still-auto-named callees (kept as FUN_ externs; meaning undecided) ─────*/
extern void FUN_0804031c(int);
extern void FUN_08029c0c(uint8_t);
extern void FUN_0802a03c(void);
extern void FUN_0802e3d0(uint16_t);
extern void FUN_0803b76c(void);
extern void FUN_0803dbb8(void);
extern void FUN_0802f16c(void);
extern void FUN_0803bec8(uint8_t a, int b);
extern void FUN_0803fc24(uint32_t base, uint8_t v);
extern void FUN_0802954c(void);
extern void FUN_08028458(uint8_t);
extern void FUN_0803ce08(void);
extern void FUN_08038ec4(uint8_t);
extern void FUN_080381d0(uint32_t);
extern char FUN_08033914(uint32_t);    /* digit/popcount of a 32-bit word */
extern void FUN_0803c5fc(int);
extern void FUN_0803c5f0(int);
extern void FUN_0803c608(int);
extern void FUN_080298dc(void);

void ble_cmd_dispatch(unsigned int cmd, unsigned int p2, unsigned char *payload)
{
    uint8_t *ctx = APP_CTX_HOLDER;
    uint8_t  cfg_scratch[0xC0];   /* receives ctx+0x104 (0xC0 B) before config-persist */
    uint32_t rec_scratch[11];     /* receives ctx+0x320.. before each EEPROM record    */

    if (cmd < 0x55c2) {
        if (cmd < 0x5503) {
            if (cmd < 0xe) {
                switch (cmd) {
                case 0x0a:
                    g_log_func("got motor version\r\n");
                    *(uint32_t *)(ctx + 0x388) = *(uint32_t *)payload;
                    return;
                case 0x0b:
                    g_log_func("got motor io\r\n");
                    *(uint16_t *)(ctx + 0x376) = *(uint16_t *)payload;
                    return;
                case 0x0c:
                    *(uint32_t *)(ctx + 0x364) = *(uint32_t *)payload;
                    *(uint32_t *)(ctx + 0x368) = *(uint32_t *)(payload + 4);
                    *(uint32_t *)(ctx + 0x36c) = *(uint32_t *)(payload + 8);
                    *(uint16_t *)(ctx + 0x370) = *(uint16_t *)(payload + 0xc);
                    return;
                case 0x0d:
                    *(uint32_t *)(ctx + 0x372) = *(uint32_t *)payload;
                    return;
                }
                return;
            }
            else if (cmd < 0x124 && cmd > 0xf9) {
                switch (cmd) {
                case 0xfa:
                    g_log_func("CMD_BLE_MAINWARE_DELAY_STANDBY\r\n", *payload);
                    FUN_0804031c(7);
                    if (*payload == 1 && (uint8_t)(ctx[0x310] - 3u) < 2) {
                        log_print_timestamp_prefix();
                        g_log_func("set delay\r\n");
                        ctx[0x310] = 0x0b;
                        ctx[0x312] = 0;
                        memcpy(rec_scratch, ctx + 0x320, sizeof(rec_scratch));
                        if (save_state_record_to_eeprom(*(uint32_t *)(ctx + 0x310),
                                                        *(uint32_t *)(ctx + 0x314),
                                                        *(uint32_t *)(ctx + 0x318),
                                                        *(uint32_t *)(ctx + 0x31c)) != 0) {
                            g_log_func(" ERROR Save values\r\n");
                        }
                        maybe_set_state_if_unlocked('0');
                    }
                    if (*payload != 1) {
                        return;
                    }
                    if (maybe_get_bike_state() != 5) {
                        return;
                    }
                    log_print_timestamp_prefix();
                    g_log_func("Start PIN\r\n");
                    maybe_set_state_if_unlocked('\x0e');
                    set_mode_state_byte(0xf);
                    return;

                case 0x105:
                    memcpy(ctx + 0x1dc, payload, 0x100);
                    ctx[0x2dc] = 1;
                    return;

                case 0x10e:
                    g_log_func("CMD_OAD_STATUS\r\n");
                    FUN_08029c0c(*payload);
                    return;

                case 0x113:
                    g_log_func("CMD_AUDIO_STOPPED\r\n");
                    *(uint16_t *)(ctx + 0x3e3) = *(uint16_t *)payload;
                    if (ctx[0x3e4] != 0 && ctx[0x3e4] != 2) {
                        return;
                    }
                    FUN_0802a03c();
                    return;

                case 0x118:
                    g_log_func("CMD_BLE_VERSION_INFO\r\n");
                    *(uint32_t *)(ctx + 0x38c) = *(uint32_t *)payload;
                    return;

                case 0x119:
                    g_log_func("CMD_BLE_RSSI\r\n");
                    ctx[0x3e2] = *payload;
                    return;

                case 0x11a:
                    g_log_func("CMD_BLE_MAC\r\n");
                    *(uint32_t *)(ctx + 0x390) = *(uint32_t *)payload;
                    *(uint16_t *)(ctx + 0x394) = *(uint16_t *)(payload + 4);
                    /* OEM format @0x08051D40 = "%02X%02X%02XDeBug" (3 args, no bug) */
                    snprintf((char *)(ctx + 0x398), 0xc, "%02X%02X%02XDeBug",
                             (unsigned)ctx[0x393], (unsigned)ctx[0x394], (unsigned)ctx[0x395]);
                    return;

                case 0x11b:
                    g_log_func("CMD_BLE_OAD_IMAGE_INFO\r\n");
                    *(uint32_t *)(ctx + 0x37c) = *(uint32_t *)payload;
                    *(uint32_t *)(ctx + 0x380) = *(uint32_t *)(payload + 4);
                    *(uint32_t *)(ctx + 0x384) = *(uint32_t *)(payload + 8);
                    g_log_func("Ver says: %02X\r\n", *(uint32_t *)(ctx + 0x380));
                    g_log_func("Size says: %d\r\n", *(uint32_t *)(ctx + 0x384));
                    return;

                case 0x122:
                    g_log_func("CMD_BLE_DISPLAY_CUSTOM_DISPLAY_ENABLE\r\n");
                    g_log_func("Size %d\r\n", *(uint16_t *)payload);
                    FUN_0802e3d0(*(uint16_t *)payload);
                    return;

                case 0x123:
                    g_log_func("CMD_BLE_TOP_HALF_OF_DISPLAY\r\n");
                    if (*payload != 0) {
                        g_log_func("BLE reset high\r\n");
                        HAL_GPIO_WritePin(GPIOE_BASE, 0x20, 1);
                        return;
                    }
                    g_log_func("BLE reset low\r\n");
                    HAL_GPIO_WritePin(GPIOE_BASE, 0x20, 0);
                    return;
                }
                return;
            }
            return;
        }
        else {
            switch (cmd) {
            case 0x5503:
                g_log_func("CMD_BLE_SECURITY_BACKUP_CODE\r\n");
                channel_notify_with_status(1);
                if (payload[0] == 0xff && payload[1] == 0xff && payload[2] == 0xff) {
                    *(uint16_t *)(ctx + 0x100) = 0xff;
                    ctx[0x317] = 0;
                    payload[0] = 1;
                } else {
                    *(uint16_t *)(ctx + 0x100) =
                        (uint16_t)(payload[1] * 10 + payload[2] * 100 + payload[0]);
                    ctx[0x317] = 1;
                    payload[0] = 0;
                }
                g_log_func("SOC saved %d\r\n",
                           ((unsigned)payload[0] + payload[1] + payload[2]) * 2 + 0x7b0);
                memcpy(cfg_scratch, ctx + 0x104, 0xC0);
                config_persist_dual_bank(*(uint32_t *)(ctx + 0xf4), *(uint32_t *)(ctx + 0xf8),
                                         *(uint32_t *)(ctx + 0xfc), *(uint32_t *)(ctx + 0x100));
                if (ssp_ble_enqueue_tx_packet(0x5503, 1, payload, '\0') > 0x80) {
                    g_log_func("  ERROR SSP place\r\n");
                }
                return;

            case 0x5521:
                g_log_func("CMD_BLE_DEFENCE_ALARM_STATE\r\n");
                if (*payload == 1) {
                    ctx[0x312] = 1;
                    maybe_set_state_if_unlocked(' ');
                    return;
                }
                if (*payload == 2) {
                    ctx[0x312] = 0;
                    reset_dual_buffers_and_flags();
                    maybe_set_state_if_unlocked('0');
                    return;
                }
                if (*payload != 0) {
                    return;
                }
                if (ctx[0x312] != 0) {
                    if (HAL_GPIO_ReadPin(GPIOC_BASE, 0x100) == 0) {
                        g_log_func("App connect Leave, BMS alarm\r\n");
                        g_log_func("BT set Shipping mode\r\n");
                        ctx[0x351] = 3;
                        ctx[0x350] = 3;
                        ctx[0x352] = 3;
                        maybe_set_state_if_unlocked('$');
                    }
                }
                return;

            case 0x5523:
                g_log_func("CMD_BLE_DEFENCE_LOCK_STATE\r\n");
                channel_notify_with_status(1);
                switch (*payload) {
                case 0:
                    ctx[0x310] = 0x0b;
                    FUN_0803b76c();
                    reset_dual_buffers_and_flags();
                    ctx[0x312] = 0;
                    maybe_set_state_if_unlocked('\x0e');
                    break;
                case 1:
                    ctx[0x312] = 1;
                    g_log_func("BT virtual unlock\r\n");
                    maybe_set_state_if_unlocked('\x0e');
                    break;
                case 2:
                    ctx[0x310] = 0x0b;
                    channel_notify_with_status(0xe);
                    maybe_set_state_if_unlocked('\x01');
                    break;
                case 3:
                    ctx[0x310] = 2;
                    maybe_set_state_if_unlocked('\x02');
                    break;
                case 4:
                    ctx[0x310] = 4;
                    ctx[0x312] = 1;
                    maybe_set_state_if_unlocked('\x04');
                    FUN_0803dbb8();
                    break;
                default:
                    g_log_func("Invalid lock state\r\n");
                    break;
                }
                memcpy(rec_scratch, ctx + 0x320, sizeof(rec_scratch));
                if (save_state_record_to_eeprom(*(uint32_t *)(ctx + 0x310),
                                                *(uint32_t *)(ctx + 0x314),
                                                *(uint32_t *)(ctx + 0x318),
                                                *(uint32_t *)(ctx + 0x31c)) != 0) {
                    g_log_func(" ERROR Save values\r\n");
                }
                return;

            case 0x5524:
                g_log_func("CMD_BLE_DEFENCE_ALARM_SETTING\r\n");
                channel_notify_with_status(1);
                enter_mode3_arm_show_timer();
                if (*payload == 0) {
                    maybe_set_pending_request(REQ_ALARM_OFF);
                } else {
                    if (*payload != 1) {
                        g_log_func("Invalid rx value\r\n");
                        return;
                    }
                    maybe_set_pending_request(REQ_ALARM_ON);
                }
                ctx[0x317] = *payload;
                memcpy(rec_scratch, ctx + 0x320, sizeof(rec_scratch));
                if (save_state_record_to_eeprom(*(uint32_t *)(ctx + 0x310),
                                                *(uint32_t *)(ctx + 0x314),
                                                *(uint32_t *)(ctx + 0x318),
                                                *(uint32_t *)(ctx + 0x31c)) != 0) {
                    g_log_func(" ERROR Save values\r\n");
                }
                if (ssp_ble_enqueue_tx_packet(0x5524, 1, payload, '\0') > 0x80) {
                    g_log_func("  ERROR SSP place\r\n");
                }
                return;

            case 0x5531:
                g_log_func("CMD_BLE_MOVEMENT_TOTAL_DISTANCE\r\n");
                channel_notify_with_status(1);
                *(uint32_t *)(ctx + 0x31c) = *(uint32_t *)payload;
                if (ssp_ble_enqueue_tx_packet(0x5531, 4, payload, '\0') > 0x80) {
                    g_log_func("  ERROR SSP place\r\n");
                }
                return;

            case 0x5533:
                g_log_func("CMD_BLE_MOVEMENT_UNIT_SETTING\r\n");
                if (*payload > 1) {
                    g_log_func("Invalid unit setting\r\n");
                    return;
                }
                channel_notify_with_status(1);
                ctx[0x10a] = *payload;
                enter_mode3_arm_show_timer();
                if (*payload == 0) {
                    maybe_set_pending_request(REQ_UNITS_METRIC);
                } else if (*payload == 1) {
                    maybe_set_pending_request(REQ_UNITS_IMPERIAL);
                }
                g_log_func("Write 0x5533 %d\r\n", *payload);
                memcpy(cfg_scratch, ctx + 0x104, 0xC0);
                config_persist_dual_bank(*(uint32_t *)(ctx + 0xf4), *(uint32_t *)(ctx + 0xf8),
                                         *(uint32_t *)(ctx + 0xfc), *(uint32_t *)(ctx + 0x100));
                if (ssp_ble_enqueue_tx_packet(0x5533, 1, payload, '\0') > 0x80) {
                    g_log_func("  ERROR SSP place\r\n");
                }
                return;

            case 0x5534:
                g_log_func("CMD_BLE_MOVEMENT_POWER_ASSISTANCE_MODE\r\n");
                if (*payload > (ctx[0x144] == 0 ? 5u : 4u)) {
                    g_log_func("Invalid power assistant mode\r\n");
                    return;
                }
                channel_notify_with_status(1);
                ctx[0x3cb] = (payload[1] == 1);
                if (ctx[0x3c9] != *payload) {
                    ctx[0x3c9] = *payload;
                    ctx[0x3ca] = *payload;
                    FUN_0802f16c();
                    switch (ctx[0x3c9]) {
                    case 1: *(uint16_t *)(ctx + 0x354) = 0xb4; break;
                    case 2: *(uint16_t *)(ctx + 0x354) = 0x78; break;
                    case 3: *(uint16_t *)(ctx + 0x354) = 0x3c; break;
                    case 4: *(uint16_t *)(ctx + 0x354) = 0x1e; break;
                    default: *(uint16_t *)(ctx + 0x354) = 0;   break;
                    }
                    FUN_0803bec8(ctx[0x3c9], 0xb);
                }
                if (ssp_ble_enqueue_tx_packet(0x5534, 2, payload, '\0') > 0x80) {
                    g_log_func("  ERROR SSP place\r\n");
                }
                return;

            case 0x5535:
            case 0x553c:
                g_log_func("CMD_BLE_MOVEMENT_REGION_SPEED_LIMIT\r\n");
                if ((uint8_t)(*payload - 4) < 0xfb) {
                    g_log_func("Invalid region\r\n");
                    return;
                }
                if (cmd == 0x553c) {
                    if (payload[1] > 2) {
                        g_log_func("Invalid region lock value\r\n");
                        return;
                    }
                    ctx[0x144] = payload[1];
                } else {
                    if (ctx[0x144] == 1 && *payload == 0xff) {
                        g_log_func("Region 'off road' is not allowed\r\n");
                        return;
                    }
                    if (ctx[0x144] == 2) {
                        g_log_func("Region is locked\r\n");
                        return;
                    }
                }
                channel_notify_with_status(1);
                if (*payload == 0xff) {
                    ctx[0x109] = 3;
                } else {
                    ctx[0x109] = *payload;
                }
                g_log_func("Write 0x5535 %d\r\n", *payload);
                memcpy(cfg_scratch, ctx + 0x104, 0xC0);
                config_persist_dual_bank(*(uint32_t *)(ctx + 0xf4), *(uint32_t *)(ctx + 0xf8),
                                         *(uint32_t *)(ctx + 0xfc), *(uint32_t *)(ctx + 0x100));
                enter_mode3_arm_show_timer();
                switch (ctx[0x109]) {
                case 0: maybe_set_pending_request(REQ_REGION_EU); break;
                case 1: maybe_set_pending_request(REQ_REGION_US); break;
                case 2: maybe_set_pending_request(REQ_REGION_JP); break;
                case 3: maybe_set_pending_request(REQ_REGION_OFFROAD); break;
                }
                FUN_0803fc24((uint32_t)(uintptr_t)(ctx + 0x1c4), ctx[0x109]);
                if (cmd == 0x553c) {
                    if (ssp_ble_enqueue_tx_packet(0x553c, 2, payload, '\0') > 0x80) {
                        g_log_func("Invalid parameter use (1..4)\r\n");
                    }
                    return;
                }
                if (ssp_ble_enqueue_tx_packet(0x5535, 1, payload, '\0') > 0x80) {
                    g_log_func("Invalid parameter use (1..4)\r\n");
                }
                return;

            case 0x5536:
                g_log_func("CMD_BLE_MOVEMENT_ESHIFTER\r\n");
                /* The busy latch is the holder-relative global at 0x20000948,
                 * NOT a ctx field. */
                if (*(int16_t *)(ctx + 0x3fc) != -1 && (char)g_eshifter_busy == 0) {
                    g_eshifter_busy = 1;
                    log_print_timestamp_prefix();
                    g_log_func("Shifter calibrate\r\n");
                    FUN_0802954c();
                    return;
                }
                if ((uint8_t)(*payload - 1) < 4) {
                    channel_notify_with_status(1);
                    ctx[0x3cc] = *payload;
                    *(int32_t *)(ctx + 0x338) += 1;
                    FUN_08028458(*payload);
                    return;
                }
                g_log_func("Invalid parameter use (1..4)\r\n");
                return;

            case 0x5537:
                g_log_func("CMD_BLE_MOVEMENT_ESHIFTER_RATIO\r\n");
                if (ssp_ble_enqueue_tx_packet(0x5537, 6, payload, '\0') > 0x80) {
                    g_log_func("Invalid parameter use (1..4)\r\n");
                }
                {
                    uint8_t region = ctx[0x109];
                    uint8_t *slot = ctx + region * 6;
                    *(uint16_t *)(slot + 0x10e) = (uint16_t)(payload[0] * 10);
                    *(uint16_t *)(slot + 0x110) = (uint16_t)(payload[1] * 10);
                    *(uint16_t *)(slot + 0x112) = (uint16_t)(payload[2] * 10);
                    *(uint16_t *)(slot + 0x126) = (uint16_t)(payload[3] * 10);
                    *(uint16_t *)(slot + 0x128) = (uint16_t)(payload[4] * 10);
                    *(uint16_t *)(slot + 0x12a) = (uint16_t)(payload[5] * 10);
                }
                g_log_func("Write 0x5537 %d %d %d %d %d %d\r\n",
                           payload[0], payload[1], payload[2],
                           payload[3], payload[4], payload[5]);
                memcpy(cfg_scratch, ctx + 0x104, 0xC0);
                config_persist_dual_bank(*(uint32_t *)(ctx + 0xf4), *(uint32_t *)(ctx + 0xf8),
                                         *(uint32_t *)(ctx + 0xfc), *(uint32_t *)(ctx + 0x100));
                channel_notify_with_status(1);
                return;

            case 0x5538:
                g_log_func("CMD_BLE_ESHIFTER_MODE\r\n");
                if (*payload < 2) {
                    if (ssp_ble_enqueue_tx_packet(0x5538, 1, payload, '\0') > 0x80) {
                        g_log_func("  ERROR SSP place\r\n");
                    }
                    ctx[0x108] = *payload;
                    g_log_func("Shifter mode set\r\n");
                    memcpy(cfg_scratch, ctx + 0x104, 0xC0);
                    config_persist_dual_bank(*(uint32_t *)(ctx + 0xf4), *(uint32_t *)(ctx + 0xf8),
                                             *(uint32_t *)(ctx + 0xfc), *(uint32_t *)(ctx + 0x100));
                    channel_notify_with_status(1);
                    return;
                }
                g_log_func("Invalid shifter mode\r\n");
                return;

            case 0x5562:
                if (ctx[0x310] != 0x0b ||
                    (maybe_get_bike_state() == 0x15 &&
                     (maybe_get_bike_state() != 0x15 || ctx[0x3e1] == 0)) ||
                    (maybe_get_bike_state() > 0x18 && maybe_get_bike_state() < 0x1e)) {
                    g_log_func("Ignore alarm-arm\r\n");
                    return;
                }
                g_log_func("CMD_BLE_DEFENCE_ALARM_ARM\r\n");
                if (*payload == 1) {
                    channel_notify_with_status(1);
                    g_log_func("BT set autowakeup\r\n");
                    g_log_func("BT set sleep mode\r\n");
                    ctx[0x350] = 4;
                    ctx[0x351] = 4;
                    ctx[0x352] = 4;
                    maybe_set_state_if_unlocked('\x0e');
                    return;
                }
                if (*payload == 2) {
                    g_log_func("BT set riding mode\r\n");
                    g_log_func("BT set sleep mode\r\n");
                    ctx[0x350] = 4;
                    ctx[0x351] = 4;
                    ctx[0x352] = 4;
                    maybe_set_state_if_unlocked('\x07');
                    return;
                }
                if (*payload != 0) {
                    channel_notify_with_status(2);
                    g_log_func("Invalid alarm-arm value\r\n");
                    return;
                }
                channel_notify_with_status(1);
                if (HAL_GPIO_ReadPin(GPIOC_BASE, 0x100) != 0) {
                    if (maybe_get_bike_state() != '2' &&
                        maybe_get_bike_state() != 0 &&
                        maybe_get_bike_state() != 1) {
                        g_log_func("Ignore autowakeup\r\n");
                        maybe_set_state_if_unlocked('\x12');
                        return;
                    }
                    g_log_func("Already awake\r\n");
                    return;
                }
                if (maybe_get_bike_state() == '\f') {
                    return;
                }
                if (maybe_get_bike_state() == 0x15) {
                    if (maybe_get_bike_state() != 0x15) {
                        return;
                    }
                    if (ctx[0x3e1] == 0) {
                        return;
                    }
                }
                g_log_func("BT set Shipping mode\r\n");
                ctx[0x351] = 3;
                ctx[0x350] = 3;
                ctx[0x352] = 3;
                maybe_set_state_if_unlocked('$');
                return;

            case 0x5564:
                g_log_func("CMD_BLE_STATE_WHEEL_SIZE\r\n");
                if (*payload > 1) {
                    g_log_func("Error wheel size: 0..1\r\n");
                    return;
                }
                ctx[0x10b] = *payload;
                g_log_func("Write 0x5564 %d\r\n", *payload);
                memcpy(cfg_scratch, ctx + 0x104, 0xC0);
                config_persist_dual_bank(*(uint32_t *)(ctx + 0xf4), *(uint32_t *)(ctx + 0xf8),
                                         *(uint32_t *)(ctx + 0xfc), *(uint32_t *)(ctx + 0x100));
                channel_notify_with_status(1);
                if (ctx[0x10b] == 1) {
                    HAL_GPIO_WritePin(GPIOG_BASE, 8, 1);
                } else {
                    HAL_GPIO_WritePin(GPIOG_BASE, 8, 0);
                }
                systick_delay(10);
                HAL_GPIO_WritePin(GPIOB_BASE, 0x200, 1);
                systick_delay(10);
                HAL_GPIO_WritePin(GPIOB_BASE, 0x200, 0);
                if (g_wheelsize_timer == 0xfa) {
                    g_wheelsize_timer = scheduler_alloc();
                    scheduler_start(g_wheelsize_timer, 1000, FUN_08033934);
                }
                if (ssp_ble_enqueue_tx_packet(0x5564, 1, payload, '\0') > 0x80) {
                    g_log_func("  ERROR SSP place\r\n");
                }
                return;

            case 0x5565:
                g_log_func("CMD_BLE_STATE_FORCE_SMS_CHECK\r\n");
                if (*payload == 1) {
                    FUN_0803ce08();
                }
                return;

            case 0x5566:
                g_log_func("CMD_BLE_HORN_SELECTOR\r\n");
                if (*payload < 7) {
                    channel_notify_with_status(7);
                    enter_mode3_arm_show_timer();
                    maybe_set_pending_request(REQ_SOUND_SELECT);
                    FUN_08038ec4(*payload);
                    return;
                }
                g_log_func("Invalid horn index\r\n");
                return;

            case 0x5567:
                g_log_func("CMD_BLE_TRACKING_FIELD\r\n");
                FUN_080381d0(*(uint32_t *)payload);
                return;

            case 0x5571:
                g_log_func("CMD_BLE_SOUND_PLAY\r\n");
                channel_notify_with_status(*payload);
                return;

            case 0x5572:
                g_log_func("CMD_BLE_SOUND_VOLUME\r\n");
                channel_notify_with_status(1);
                {
                    char a = FUN_08033914(*(uint32_t *)(ctx + 0xf4));
                    char b = FUN_08033914(*(uint32_t *)(ctx + 0xf8));
                    char c = FUN_08033914(*(uint32_t *)(ctx + 0xfc));
                    uint32_t w0 = (uint32_t)((payload[0] << 24) | (payload[1] << 16) |
                                             (payload[2] << 8) | payload[3]);
                    uint32_t w1 = (uint32_t)((payload[4] << 24) | (payload[5] << 16) |
                                             (payload[6] << 8) | payload[7]);
                    uint32_t w2 = (uint32_t)((payload[8] << 24) | (payload[9] << 16) |
                                             (payload[10] << 8) | payload[0xb]);
                    char d, e, f;
                    *(uint32_t *)(ctx + 0xf4) = w0;
                    *(uint32_t *)(ctx + 0xf8) = w1;
                    *(uint32_t *)(ctx + 0xfc) = w2;
                    d = FUN_08033914(w0);
                    e = FUN_08033914(w1);
                    f = FUN_08033914(w2);
                    enter_mode3_arm_show_timer();
                    if ((uint8_t)(a + b + c) < (uint8_t)(d + e + f)) {
                        maybe_set_pending_request(REQ_CODE_UP);
                    } else {
                        maybe_set_pending_request(REQ_CODE_DOWN);
                    }
                }
                g_log_func("Write 0x5572 0x%08X 0x%08X 0x%08X\r\n",
                           *(uint32_t *)(ctx + 0xf4), *(uint32_t *)(ctx + 0xf8),
                           *(uint32_t *)(ctx + 0xfc));
                memcpy(cfg_scratch, ctx + 0x104, 0xC0);
                config_persist_dual_bank(*(uint32_t *)(ctx + 0xf4), *(uint32_t *)(ctx + 0xf8),
                                         *(uint32_t *)(ctx + 0xfc), *(uint32_t *)(ctx + 0x100));
                if (ssp_ble_enqueue_tx_packet(0x5572, 0xc, payload, '\0') > 0x80) {
                    g_log_func("  ERROR SSP place\r\n");
                }
                return;

            case 0x5574:
                g_log_func("CMD_BLE_HORN_FILE\r\n");
                enter_mode3_arm_show_timer();
                switch (*payload) {
                case 0x0a:
                    maybe_set_pending_request(REQ_HORN_0A);
                    channel_notify_with_status(10);
                    break;
                case 0x16:
                    maybe_set_pending_request(REQ_HORN_16);
                    channel_notify_with_status(0x16);
                    break;
                case 0x17:
                    maybe_set_pending_request(REQ_HORN_17);
                    channel_notify_with_status(0x17);
                    break;
                case 0x18:
                    maybe_set_pending_request(REQ_HORN_18);
                    channel_notify_with_status(0x18);
                    break;
                default:
                    g_log_func("Invalid horn file\r\n");
                    return;
                }
                ctx[0x318] = *payload;
                memcpy(rec_scratch, ctx + 0x320, sizeof(rec_scratch));
                if (save_state_record_to_eeprom(*(uint32_t *)(ctx + 0x310),
                                                *(uint32_t *)(ctx + 0x314),
                                                *(uint32_t *)(ctx + 0x318),
                                                *(uint32_t *)(ctx + 0x31c)) != 0) {
                    g_log_func(" ERROR Save values\r\n");
                }
                if (ssp_ble_enqueue_tx_packet(0x5574, 1, payload, '\0') > 0x80) {
                    g_log_func("  ERROR SSP place\r\n");
                }
                return;

            case 0x5581:
                g_log_func("CMD_BLE_LIGHT_LIGHT_SETTING\r\n");
                if (*payload > 2) {
                    g_log_func("Invalid light setting\r\n");
                    return;
                }
                ctx[0x10c] = *payload;
                g_log_func("Write 0x5581 %d\r\n", *payload);
                memcpy(cfg_scratch, ctx + 0x104, 0xC0);
                config_persist_dual_bank(*(uint32_t *)(ctx + 0xf4), *(uint32_t *)(ctx + 0xf8),
                                         *(uint32_t *)(ctx + 0xfc), *(uint32_t *)(ctx + 0x100));
                channel_notify_with_status(1);
                enter_mode3_arm_show_timer();
                if (*payload == 1) {
                    maybe_set_pending_request(REQ_LIGHT_ON);
                } else if (*payload == 2) {
                    maybe_set_pending_request(REQ_LIGHT_OFF);
                } else if (*payload == 0) {
                    maybe_set_pending_request(REQ_LIGHT_AUTO);
                }
                if (ssp_ble_enqueue_tx_packet(0x5581, 1, payload, '\0') > 0x80) {
                    g_log_func("  ERROR SSP place\r\n");
                }
                return;

            case 0x5582:
                g_log_func("CMD_BLE_LIGHT_LIGHT_STATE\r\n");
                g_log_func("Size %d\r\n", p2);
                FUN_0803c5fc(payload[0] == 0 ? 0 : 100);
                FUN_0803c5f0(payload[1] == 0 ? 0 : 100);
                FUN_0803c608(payload[2] == 0 ? 0 : 100);
                return;

            case 0x5584:
                g_log_func("CMD_BLE_LIGHT_LIGHT_SENSOR\r\n");
                if (*(int16_t *)payload != -1) {
                    channel_notify_with_status(1);
                    *(int16_t *)(ctx + 0x102) = *(int16_t *)payload;
                    g_log_func("Write 0x5584 %d\r\n", *(int16_t *)payload);
                    memcpy(cfg_scratch, ctx + 0x104, 0xC0);
                    config_persist_dual_bank(*(uint32_t *)(ctx + 0xf4), *(uint32_t *)(ctx + 0xf8),
                                             *(uint32_t *)(ctx + 0xfc), *(uint32_t *)(ctx + 0x100));
                    return;
                }
                g_log_func(" ERROR invalid value 0..65535\r\n");
                return;

            case 0x55a1:
                g_log_func("CMD_BLE_TEST_FLAGS\r\n");
                ctx[0xf0] = *payload & 1;
                ctx[0xf1] = (uint8_t)(((uint32_t)*payload << 0x1e) >> 0x1f);
                return;

            case 0x55a2:
                g_log_func("CMD_BLE_TESTMODE_A\r\n");
                *(uint32_t *)(ctx + 0)   = 1;
                *(uint32_t *)(ctx + 4)   = 10;
                *(uint32_t *)(ctx + 8)   = 1;
                *(uint32_t *)(ctx + 0xc) = 0x100;
                memcpy(ctx + 0x10, payload, 0x28);   /* 0x20-byte block + 8-byte tail */
                return;

            case 0x55a3:
                g_log_func("CMD_BLE_TESTMODE_B\r\n");
                *(uint32_t *)(ctx + 0)   = 0x0b;
                *(uint32_t *)(ctx + 4)   = 9;
                *(uint32_t *)(ctx + 8)   = 1;
                *(uint32_t *)(ctx + 0xc) = 0x100;
                memcpy(ctx + 0x4c, payload, 0x24);   /* 0x20-byte block + 4-byte tail */
                return;

            case 0x55c1:
                g_log_func("CMD_BLE_LOG_MODE\r\n");
                if (*payload < 2) {
                    channel_notify_with_status(1);
                    ctx[0x313] = (*payload != 0);
                    if (*payload != 0) {
                        FUN_080298dc();
                    }
                    return;
                }
                if (*payload == 3) {
                    maybe_set_state_if_unlocked('\x17');
                }
                return;
            }
        }
    }

    g_log_func("Unhandeled SSP message %04X\r\n", cmd, p2);
}
