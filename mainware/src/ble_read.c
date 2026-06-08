#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "app.h"
#include "app_state.h"
#include "crc.h"
#include "gpio.h"
#include "lighting.h"
#include "log.h"
#include "rtc.h"
#include "sensor.h"
#include "ssp.h"
#include "stm32f413_gpio.h"
#include "util.h"

/* ------------------------------------------------------------------ *
 * ble_read_request_dispatch (OEM 0x08034D20) — the BLE read/telemetry
 * surface, the read twin of ble_cmd_dispatch. A big switch on the 16-bit
 * GATT char id: each case reads ctx field(s) and/or queries a helper,
 * packs a response into a small stack byte buffer, and acks it to the
 * phone with ssp_ble_enqueue_tx_packet(char_id, len, &resp, 0). This is
 * what the app polls to render bike state. See docs/ble-commands.md
 * ("Read / telemetry surface") and docs/state-machine.md.
 *
 * The dispatcher ABI passes (char_id, p2, payload); the read path only
 * consumes char_id (p2/payload are part of the shared dispatcher
 * signature but unused here).
 * ------------------------------------------------------------------ */

/* Session/app context pointer holder (OEM RAM 0x20000944). All ctx field
 * reads dereference the live pointer stored here. Field offsets and
 * meanings are the "Read / telemetry surface" table in docs/ble-commands.md. */
#define CTX_PTR_HOLDER  (*(uint8_t **)0x20000944u)

/* App image header (OEM 0x08020000); +4 is the packed fw-version word. */
#define APP_IMAGE_HEADER  ((const uint8_t *)0x08020000u)

/* PD2 is the lock-pin sense read for 0x5568 (GPIOD_BASE from stm32f413_gpio.h). */

/* libc / CubeF4 HAL helpers, supplied by the vendored upstream at link time. */
extern int HAL_GPIO_ReadPin(void *GPIOx, uint16_t pin);

/* Telemetry / state getters are now sourced in their home modules (headers
 * included above):
 *   app.h      bike_status_coarse_get / bike_state_is_standby /
 *              ble_lock_state_get / ble_unlock_state_get / maybe_get_bike_state
 *   sensor.h   supply_voltage_read / charge_level_adc_get / hw_version_lookup
 *   gpio.h     gpio_pc0_is_low / gpio_pc1_is_low
 *   lighting.h ble_get_led_channel_state / light_sensor_read_step
 *   util.h     telemetry_map_clamp
 *   rtc.h      rtc_now_epoch_seconds
 * ble_get_charge_plug_state and ble_build_testmode_versions_blob are defined
 * below (this is their home).
 *
 * The motor/battery Modbus block poll (0x14/0x19) enqueues via the shared
 * outbound-message table (OEM maybe_enqueue_tx_message, 0x0803a1c4, in ssp.c)
 * and returns a status compared < 0x11. Kept as a local extern — no header,
 * since several callers pass the payload arg as a raw address. */
extern unsigned int maybe_enqueue_tx_message(uint16_t id, int kind, uint32_t src, int flags);

/* GPIOC charger-detect pin (PC10). */
#define GPIO_PIN_10   0x0400u

/* Bike-state context block (0x20000029): +0x13 = charge sub-state flag (the
 * state byte at +4 is read via maybe_get_bike_state). */
#define BLE_BIKE_CTX  ((volatile uint8_t *)0x20000029u)

/* Charger-plug / BMS charge state byte for the BLE 0x5542 read (OEM
 * ble_get_charge_plug_state, 0x0802a87c). Charger-detect line (PC10) low -> 4
 * (charger absent). Otherwise only state 0x15 (charging) consults the +0x13 flag
 * to pick sub-state 1 (set) vs 2 (clear); any other state reports 0. */
uint8_t ble_get_charge_plug_state(void)
{
    if (HAL_GPIO_ReadPin((void *)GPIOC_BASE, GPIO_PIN_10) == 0) {
        return 4;
    }
    if (maybe_get_bike_state() == 0x15u) {
        return BLE_BIKE_CTX[0x13] == 0 ? 2 : 1;
    }
    return 0;
}

/* Build the 0x60-byte testmode/versions ASCII blob for the BLE 0x5551 read (OEM
 * ble_build_testmode_versions_blob, 0x0802a9dc). Six 0x10-byte fields are carved
 * into the scratch at 0x200001D8 + 0x1C; per-module info comes through the
 * session-context pointer (CTX_PTR_HOLDER), this firmware's version from the
 * image header. The blob's hardware CRC-32 is returned through *out_crc — the
 * OEM stores it unconditionally and the 0x5551 reader passes NULL, relying on
 * the resulting write to flash-alias address 0 being benign; we guard it
 * instead. The other two callers (ble_telemetry_change_broadcast) pass a real
 * pointer. Returns the blob pointer. */
uint32_t *ble_build_testmode_versions_blob(uint32_t *out_crc)
{
    char    *buf = (char *)(0x200001d8u + 0x1c);
    uint8_t *dev = CTX_PTR_HOLDER;          /* session/module info struct */
    uint32_t v;

    memset(buf, 0, 0x60);

    /* field 0: this firmware's version word (image header + 4) */
    v = *(const uint32_t *)(APP_IMAGE_HEADER + 4);
    snprintf(buf, 0x10, "%d.%02d.%02d",
             (int)(v >> 0x18), (int)((v >> 0x10) & 0xff), (int)((v >> 8) & 0xff));

    /* field 1: shifter/sub fw (dev + 0x38c) */
    v = *(const uint32_t *)(dev + 0x38c);
    snprintf(buf + 0x10, 0x10, "%d.%d.%02d",
             (int)((v >> 0x10) & 0xff), (int)((v >> 8) & 0xff), (int)(v & 0xff));

    /* field 2: BMSWare fw (dev + 0x388), leading byte as %c */
    v = *(const uint32_t *)(dev + 0x388);
    snprintf(buf + 0x20, 0x10, "%c.%d.%02d.%02d\r\n",
             (int)(v >> 0x18), (int)((v >> 0x10) & 0xff),
             (int)((v >> 8) & 0xff), (int)(v & 0xff));

    /* field 3: name string at *(dev + 0x3e8) + 0x20 */
    snprintf(buf + 0x30, 0x10, "%s",
             (const char *)(*(const uint32_t *)(dev + 0x3e8) + 0x20));

    /* field 4: hw/board rev (u16 @ dev + 0x52a), hi.lo */
    {
        uint16_t hw = *(const uint16_t *)(dev + 0x52a);
        snprintf(buf + 0x40, 0x10, "%d.%d", (int)(hw >> 8), (int)(uint8_t)hw);
    }

    /* field 5: signed byte @ dev+0x409 (%X, sign-extended), low byte of u16 @ dev+0x408 */
    {
        int8_t  hi8 = *(const int8_t   *)(dev + 0x409);
        uint8_t lo8 = (uint8_t)*(const uint16_t *)(dev + 0x408);
        snprintf(buf + 0x50, 0x10, "%X.%02X", (unsigned)hi8, (unsigned)lo8);
    }

    /* CRC-32 over the whole 0x18-word (0x60-byte) blob via the HW CRC unit. */
    v = crc32_hw_feed((crc_dev_t *)0x20009d90u, (const uint32_t *)buf, 0x18);
    if (out_crc != 0) {
        *out_crc = v;
    }
    return (uint32_t *)buf;
}

/* The OEM log calls take two shapes: (*log[2])(fmt) for the plain label form
 * and (*log[0])(fmt, arg) for the formatted form. Both resolve to the same
 * logger object (OEM 0x20009D98) — i.e. g_log_func (log.h), a variadic
 * printf-style function pointer; model both shapes as g_log_func(fmt, ...). */

void ble_read_request_dispatch(unsigned int char_id, unsigned int p2,
                               unsigned char *payload)
{
    uint8_t  *ctx = CTX_PTR_HOLDER;
    uint8_t   resp[28];   /* overlays OEM local_18/14/10/c (7 stack words) */
    char      strbuf[96]; /* OEM acStack_78 */
    uint8_t   hwver;      /* OEM local_79 */
    unsigned int len;
    uint8_t   r8;
    uint16_t  v16;
    int       i;

    (void)p2;
    (void)payload;

    if (char_id >= 0x55c2u) {
        goto unhandled;
    }

    if (char_id < 0x5503u) {
        if (char_id == 0x14u) {
            /* motor data block: Modbus poll, source ctx+0x378 */
            log_print_timestamp_prefix();
            g_log_func("REQ motor options\r\n");
            len = maybe_enqueue_tx_message(0x14, 2, (uint32_t)(ctx + 0x378), 0);
            if (len >= 0x11u) {
                g_log_func("  3ERROR SSP place\r\n");
            }
            return;
        }
        if (char_id == 0x19u) {
            /* battery data block: Modbus poll, source ctx+0x3B2 */
            log_print_timestamp_prefix();
            g_log_func("REQ ibat\r\n");
            len = maybe_enqueue_tx_message(0x19, 2, (uint32_t)(ctx + 0x3b2), 0);
            if (len >= 0x11u) {
                g_log_func("  2ERROR SSP place\r\n");
            }
            return;
        }
        goto unhandled;
    }

    switch (char_id) {
    case 0x5503: /* backup-code-set flag: 1 if u16 at ctx+0x100 == 0x00FF, else 0 (len 3) */
        g_log_func("REQ 0x5503\r\n");
        resp[0] = (*(int16_t *)(ctx + 0x100) == 0xff) ? 1 : 0;
        if (ssp_ble_enqueue_tx_packet(0x5503, 3, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5521: /* lock state (0 unlocked / 1 locked / 2 in PIN-lock) */
        g_log_func("REQ 0x5521\r\n");
        resp[0] = ble_lock_state_get();
        if (ssp_ble_enqueue_tx_packet(0x5521, 1, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5522:
        g_log_func("REQ 0x5522\r\n");
        resp[0] = 0;
        if (ssp_ble_enqueue_tx_packet(0x5522, 1, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5523: /* unlock/alarm state */
        g_log_func("REQ 0x5523\r\n");
        resp[0] = ble_unlock_state_get();
        if (ssp_ble_enqueue_tx_packet(0x5523, 1, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5524: /* alarm on/off (ctx+0x317) */
        g_log_func("REQ 0x5524\r\n");
        resp[0] = ctx[0x317];
        if (ssp_ble_enqueue_tx_packet(0x5524, 1, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5531: /* odometer / distance (ctx+0x31C, u32) */
        g_log_func("REQ 0x5531\r\n");
        memcpy(resp, ctx + 0x31c, 4);
        if (ssp_ble_enqueue_tx_packet(0x5531, 4, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5532: /* speed (ctx+0x3C2, scaled /10) */
        g_log_func("REQ 0x5532\r\n");
        {
            uint32_t v = (uint32_t)((0xCCCCCCCDull * *(uint16_t *)(ctx + 0x3c2)) >> 0x23);
            memcpy(resp, &v, 4);
        }
        if (ssp_ble_enqueue_tx_packet(0x5532, 4, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5533: /* units (ctx+0x10A) */
        g_log_func("REQ 0x5533\r\n");
        resp[0] = ctx[0x10a];
        if (ssp_ble_enqueue_tx_packet(0x5533, 1, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5534: /* motor power level (ctx+0x3C9) + ride-change (ctx+0x3CB) */
        g_log_func("REQ 0x5534\r\n");
        resp[0] = ctx[0x3c9];
        resp[1] = ctx[0x3cb];
        if (ssp_ble_enqueue_tx_packet(0x5534, 2, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5535: /* region (ctx+0x109, 3 -> -1/0xFF) + region lock (ctx+0x144) */
    case 0x553c:
        g_log_func("REQ 0x%04x\r\n", char_id);
        r8 = ctx[0x109];
        if ((int8_t)r8 == 3) {
            r8 = 0xff;
        }
        resp[0] = r8;
        resp[1] = ctx[0x144];
        if (ssp_ble_enqueue_tx_packet((uint16_t)char_id, 2, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5536: /* shift gear (ctx+0x3CC) */
        g_log_func("REQ 0x5536\r\n");
        resp[0] = ctx[0x3cc];
        if (ssp_ble_enqueue_tx_packet(0x5536, 1, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5537: /* speed "moments" — per-region 6 up/down points, scaled /10 */
        g_log_func("REQ 0x5537\r\n");
        {
            uint8_t *blk = ctx + (unsigned)ctx[0x109] * 6;
            resp[0] = (uint8_t)((0xCCCCCCCDull * *(uint16_t *)(blk + 0x10e)) >> 0x23);
            resp[1] = (uint8_t)((0xCCCCCCCDull * *(uint16_t *)(blk + 0x110)) >> 0x23);
            resp[2] = (uint8_t)((0xCCCCCCCDull * *(uint16_t *)(blk + 0x112)) >> 0x23);
            resp[3] = (uint8_t)((0xCCCCCCCDull * *(uint16_t *)(blk + 0x126)) >> 0x23);
            resp[4] = (uint8_t)((0xCCCCCCCDull * *(uint16_t *)(blk + 0x128)) >> 0x23);
            resp[5] = (uint8_t)((0xCCCCCCCDull * *(uint16_t *)(blk + 0x12a)) >> 0x23);
        }
        if (ssp_ble_enqueue_tx_packet(0x5537, 6, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5538: /* transmission auto/manual (ctx+0x108) */
        g_log_func("REQ 0x5538\r\n");
        resp[0] = ctx[0x108];
        if (ssp_ble_enqueue_tx_packet(0x5538, 1, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5539: /* horn file (ctx+0x374, u16 -> u32) */
        g_log_func("REQ 0x5539\r\n");
        {
            uint32_t v = *(uint16_t *)(ctx + 0x374);
            memcpy(resp, &v, 4);
        }
        if (ssp_ble_enqueue_tx_packet(0x5539, 4, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x553a: /* pedal speed (ctx+0x372, u16 -> u32) */
        g_log_func("REQ 0x553A\r\n");
        {
            uint32_t v = *(uint16_t *)(ctx + 0x372);
            memcpy(resp, &v, 4);
        }
        if (ssp_ble_enqueue_tx_packet(0x553a, 4, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x553b: /* fixed 6-byte zero block */
        g_log_func("REQ 0x553B\r\n");
        memset(resp, 0, 16);
        if (ssp_ble_enqueue_tx_packet(0x553b, 6, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5541: /* battery summary: SoC, temps, ext-bat block */
        /* raw SoC: ctx+0x3FC if set (!= -1) else ctx+0x315 */
        if (*(int16_t *)(ctx + 0x3fc) == -1) {
            r8 = ctx[0x315];
        } else {
            r8 = (uint8_t)*(int16_t *)(ctx + 0x3fc);
        }
        resp[1] = (uint8_t)*(uint16_t *)(ctx + 0x422);
        resp[2] = (uint8_t)*(uint16_t *)(ctx + 0x424);
        resp[3] = (uint8_t)(*(uint16_t *)(ctx + 0x424) >> 8);
        resp[4] = ctx[0x3e1];
        resp[5] = ctx[0x3d4];
        resp[6] = ctx[0x3dd];
        resp[7] = (uint8_t)*(uint16_t *)(ctx + 0x3de);
        resp[8] = (uint8_t)(*(uint16_t *)(ctx + 0x3de) >> 8);
        resp[9] = ctx[0x3e0];
        resp[0] = telemetry_map_clamp(r8, 0, 0x61, 0, 100);
        g_log_func("REQ 0x5541\r\n");
        if (ssp_ble_enqueue_tx_packet(0x5541, 10, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5542: /* BMS serial/version (ctx+0x3D5..0x3D7) */
        g_log_func("REQ 0x5542\r\n");
        r8 = ble_get_charge_plug_state();
        resp[0] = r8;
        resp[1] = 0;
        resp[2] = ctx[0x3d5];
        resp[3] = ctx[0x3d6];
        resp[4] = ctx[0x3d7];
        resp[5] = ctx[0x3d8];
        resp[6] = ctx[0x3d9];
        if (ssp_ble_enqueue_tx_packet(0x5542, 7, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5543: /* charger state */
        g_log_func("REQ 0x5543\r\n");
        resp[0] = charge_level_adc_get();
        if (ssp_ble_enqueue_tx_packet(0x5543, 1, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5544: /* LiPo status index (ctx+0x3D0, 1 byte zero-extended to 4) */
        g_log_func("REQ 0x5544\r\n");
        memset(resp, 0, 16);
        resp[0] = ctx[0x3d0];
        if (ssp_ble_enqueue_tx_packet(0x5544, 4, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5545: /* no-op */
        return;

    case 0x5546: /* motor telemetry (ctx+0x36C, low byte) */
        g_log_func("REQ 0x5546\r\n");
        resp[0] = (uint8_t)*(uint16_t *)(ctx + 0x36c);
        if (ssp_ble_enqueue_tx_packet(0x5546, 1, resp, 0) >= 0x81) {
            g_log_func("  5ERROR SSP place\r\n");
        }
        return;

    case 0x5547: /* motor telemetry (ctx+0x36A; sentinel -0x110 -> 0) */
        g_log_func("REQ 0x5547\r\n");
        if (*(int16_t *)(ctx + 0x36a) == -0x110) {
            resp[0] = 0;
        } else {
            resp[0] = (uint8_t)*(int16_t *)(ctx + 0x36a);
        }
        if (ssp_ble_enqueue_tx_packet(0x5547, 1, resp, 0) >= 0x81) {
            g_log_func("  6ERROR SSP place\r\n");
        }
        return;

    case 0x5548: /* motor error flags (ctx+0x3A4, 8 B) — only if any set */
        if (*(int16_t *)(ctx + 0x3a4) == 0 && *(int16_t *)(ctx + 0x3aa) == 0 &&
            *(int16_t *)(ctx + 0x3a6) == 0 && *(int16_t *)(ctx + 0x3a8) == 0) {
            g_log_func("Ignore 0x5548\r\n");
            return;
        }
        g_log_func("REQ 0x5548\r\n");
        if (ssp_ble_enqueue_tx_packet(0x5548, 8, ctx + 0x3a4, 0) >= 0x81) {
            g_log_func("  7ERROR SSP place\r\n");
        }
        return;

    case 0x5549: /* model / variant string (ctx+0x64A) */
        g_log_func("REQ 0x5549\r\n");
        len = strlen((char *)(ctx + 0x64a));
        if (ssp_ble_enqueue_tx_packet(0x5549, (uint16_t)len, ctx + 0x64a, 0) >= 0x81) {
            g_log_func("  8ERROR SSP place\r\n");
        }
        return;

    case 0x554a: /* app fw version (image header @ 0x08020004) — gated on state != 3 */
        if (bike_status_coarse_get() == 3) {
            g_log_func("Ignore REQ 0x554A\r\n");
            return;
        }
        g_log_func("REQ 0x554A\r\n");
        {
            uint32_t v = *(uint32_t *)(APP_IMAGE_HEADER + 4);
            snprintf(strbuf, 0x60, "%d.%02d.%02d",
                     (int)(v >> 0x18), (int)((v & 0xffffff) >> 0x10),
                     (int)((v & 0xffff) >> 8));
        }
        len = strlen(strbuf);
        if (ssp_ble_enqueue_tx_packet(0x554a, (uint16_t)len, strbuf, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x554c: /* version string (ctx+0x388) — gated on state != 3 */
        if (bike_status_coarse_get() == 3) {
            g_log_func("Ignore REQ 0x554C\r\n");
            return;
        }
        g_log_func("REQ 0x554C\r\n");
        {
            uint32_t v = *(uint32_t *)(ctx + 0x388);
            snprintf(strbuf, 0x60, "%c.%d.%02d.%02d",
                     (int)(v >> 0x18), (int)((v & 0xffffff) >> 0x10),
                     (int)((v & 0xffff) >> 8), (int)(v & 0xff));
        }
        len = strlen(strbuf);
        if (ssp_ble_enqueue_tx_packet(0x554c, (uint16_t)len, strbuf, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x554d: /* HW version (hw_version_lookup) — gated on state != 3 */
        if (bike_status_coarse_get() == 3) {
            g_log_func("Ignore REQ 0x554D\r\n");
            return;
        }
        g_log_func("REQ 0x554D\r\n");
        if (hw_version_lookup(&hwver) == 0) {
            g_log_func("  ERR HWversion\r\n");
        }
        snprintf(strbuf, 0x60, "HW:%d", hwver);
        len = strlen(strbuf);
        if (ssp_ble_enqueue_tx_packet(0x554d, (uint16_t)len, strbuf, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x554e: /* modem version (string at *(ctx+1000) + 0x20) — gated on state != 3 */
        if (bike_status_coarse_get() == 3) {
            g_log_func("Ignore REQ 0x554E\r\n");
            return;
        }
        g_log_func("REQ 0x554E\r\n");
        snprintf(strbuf, 0x60, "%s", (char *)(*(int *)(ctx + 1000) + 0x20));
        len = strlen(strbuf);
        if (ssp_ble_enqueue_tx_packet(0x554e, (uint16_t)len, strbuf, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x554f: /* shifter version (ctx+0x336, hi.lo) — gated on state != 3 */
        if (bike_status_coarse_get() == 3) {
            g_log_func("Ignore REQ 0x554F\r\n");
            return;
        }
        g_log_func("REQ 0x554F\r\n");
        snprintf(strbuf, 0x60, "%d.%d",
                 (int)(*(uint16_t *)(ctx + 0x336) >> 8),
                 (int)(uint8_t)*(uint16_t *)(ctx + 0x336));
        len = strlen(strbuf);
        if (ssp_ble_enqueue_tx_packet(0x554f, (uint16_t)len, strbuf, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5550: /* powerbank version (ctx+0x408) + optional S/N (ctx+0x3DA..0x3DC) */
        if (bike_status_coarse_get() == 3) {
            g_log_func("Ignore REQ 0x5550\r\n");
            return;
        }
        if (*(int16_t *)(ctx + 0x408) == 0) {
            g_log_func("Ignore REQ 0x5550\r\n");
            return;
        }
        g_log_func("REQ 0x5550\r\n");
        if (ctx[0x3e1] == 0) {
            snprintf(strbuf, 0x60, "%X.%02X",
                     (unsigned)(*(uint16_t *)(ctx + 0x408) >> 8),
                     (unsigned)(uint8_t)*(uint16_t *)(ctx + 0x408));
        } else {
            snprintf(strbuf, 0x60, "%X.%02X,%02X.%02X.%02X",
                     (unsigned)(*(uint16_t *)(ctx + 0x408) >> 8),
                     (unsigned)(uint8_t)*(uint16_t *)(ctx + 0x408),
                     (unsigned)ctx[0x3da], (unsigned)ctx[0x3db],
                     (unsigned)ctx[0x3dc]);
        }
        len = strlen(strbuf);
        if (ssp_ble_enqueue_tx_packet(0x5550, (uint16_t)len, strbuf, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5551: /* testmode blob (ble_build_testmode_versions_blob, 0x60 B) */
        g_log_func("REQ 0x5551\r\n");
        if (ssp_ble_enqueue_tx_packet(0x5551, 0x60, ble_build_testmode_versions_blob(0), 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5561: /* coarse bike status */
        g_log_func("REQ 0x5561\r\n");
        resp[0] = (uint8_t)bike_status_coarse_get();
        if (ssp_ble_enqueue_tx_packet(0x5561, 1, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5562: /* alarm armed? (bike_state_is_standby, state 0x0E) */
        g_log_func("REQ 0x5562\r\n");
        resp[0] = bike_state_is_standby() ? 1 : 0;
        if (ssp_ble_enqueue_tx_packet(0x5562, 1, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5563: /* error/status flags (ctx+0x3B8, 8 B) */
        g_log_func("REQ 0x5563\r\n");
        if (ssp_ble_enqueue_tx_packet(0x5563, 8, ctx + 0x3b8, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5564: /* wheel size (ctx+0x10B) */
        g_log_func("REQ 0x5564\r\n");
        resp[0] = ctx[0x10b];
        if (ssp_ble_enqueue_tx_packet(0x5564, 1, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5567: /* tracking/modem field (rtc_now_epoch_seconds, u32) */
        g_log_func("REQ 0x5567\r\n");
        {
            uint32_t v = rtc_now_epoch_seconds();
            memcpy(resp, &v, 4);
        }
        if (ssp_ble_enqueue_tx_packet(0x5567, 4, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5568: /* button states + lock pin (GPIOD PD2) */
        g_log_func("REQ 0x5568\r\n");
        resp[0] = gpio_pc0_is_low();
        resp[1] = gpio_pc1_is_low();
        resp[2] = (HAL_GPIO_ReadPin((void *)GPIOD_BASE, 4) == 0) ? 1 : 0;
        resp[3] = 0;
        if (ssp_ble_enqueue_tx_packet(0x5568, 4, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5569: /* supply OK? (supply_voltage_read >= 20000) */
        g_log_func("REQ 0x5569\r\n");
        v16 = supply_voltage_read();
        resp[0] = (v16 < 0x4e21) ? 0 : 1;   /* 0x4e21 == 20001 -> "< 20001" i.e. >= 20000 OK */
        if (ssp_ble_enqueue_tx_packet(0x5569, 1, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5572: /* backup-code group words (ctx+0xF4/0xF8/0xFC) — byte-swapped, 12 B */
        g_log_func("REQ 0x5572\r\n");
        {
            uint32_t w0 = *(uint32_t *)(ctx + 0xf4);
            uint32_t w1 = *(uint32_t *)(ctx + 0xf8);
            uint32_t w2 = *(uint32_t *)(ctx + 0xfc);
            resp[0]  = (uint8_t)(w0 >> 0x18); resp[1]  = (uint8_t)(w0 >> 0x10);
            resp[2]  = (uint8_t)(w0 >> 8);    resp[3]  = (uint8_t)w0;
            resp[4]  = (uint8_t)(w1 >> 0x18); resp[5]  = (uint8_t)(w1 >> 0x10);
            resp[6]  = (uint8_t)(w1 >> 8);    resp[7]  = (uint8_t)w1;
            resp[8]  = (uint8_t)(w2 >> 0x18); resp[9]  = (uint8_t)(w2 >> 0x10);
            resp[10] = (uint8_t)(w2 >> 8);    resp[11] = (uint8_t)w2;
        }
        if (ssp_ble_enqueue_tx_packet(0x5572, 0xc, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5574: /* horn file select (ctx+0x318) */
        g_log_func("REQ 0x5574\r\n");
        resp[0] = ctx[0x318];
        if (ssp_ble_enqueue_tx_packet(0x5574, 1, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5581: /* light mode (ctx+0x10C) */
        g_log_func("REQ 0x5581\r\n");
        resp[0] = ctx[0x10c];
        if (ssp_ble_enqueue_tx_packet(0x5581, 1, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5582: /* LED control — three channel on/off bits */
        g_log_func("REQ 0x5582\r\n");
        resp[0] = (uint8_t)ble_get_led_channel_state() & 1;
        resp[1] = (uint8_t)((uint32_t)(ble_get_led_channel_state() << 0x1e) >> 0x1f); /* bit 1 */
        resp[2] = (uint8_t)((uint32_t)(ble_get_led_channel_state() << 0x1d) >> 0x1f); /* bit 2 */
        if (ssp_ble_enqueue_tx_packet(0x5582, 3, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x5584: /* dark/light threshold (light sensor + ctx+0x102) */
        g_log_func("REQ 0x5584\r\n");
        i = light_sensor_read_step();
        if (i == 0xfffe) {
            i = 0;
        }
        resp[0] = (uint8_t)((uint32_t)i >> 8);
        resp[1] = (uint8_t)i;
        resp[2] = (uint8_t)(*(uint16_t *)(ctx + 0x102) >> 8);
        resp[3] = (uint8_t)*(uint16_t *)(ctx + 0x102);
        if (ssp_ble_enqueue_tx_packet(0x5584, 4, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    case 0x55c1: /* log-to-app (ctx+0x313), or 3 when state == 0x17 (DIAGNOSE) */
        g_log_func("REQ 0x55C1\r\n");
        resp[0] = (maybe_get_bike_state() == 0x17) ? 3 : ctx[0x313];
        if (ssp_ble_enqueue_tx_packet(0x55c1, 1, resp, 0) >= 0x81) {
            g_log_func("  ERROR SSP place\r\n");
        }
        return;

    default:
        break;
    }

unhandled:
    log_print_timestamp_prefix();
    g_log_func("Unhandeled SSP request %04X\r\n", char_id);
}
