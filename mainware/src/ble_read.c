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
#include "scheduler.h"
#include "sensor.h"
#include "ssp.h"
#include "stm32f413_gpio.h"
#include "systick.h"
#include "util.h"

/* Cross-module leaves used by the telemetry-change broadcaster (hdc1080_*,
 * clock_pulse_gpioa8_until_pc9 come from sensor.h / app.h). */
extern int bounded_strncmp(const char *a, const char *b, unsigned int n);   /* 0x0802181C */
extern int ble_interval_debounce(void *state, unsigned a, unsigned lo,
                                  unsigned hi, unsigned win, unsigned z);     /* 0x0803A538 */

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

/* ble_telemetry_change_broadcast (OEM 0x0803A5B0) — the master telemetry
 * change-detector, ticked once per super-loop. It caches ~30 telemetry values in
 * the block at SRAM 0x200081C8 and, whenever one changes, pushes the new value to
 * the app as the matching BLE characteristic (0x5521..0x5584), logging
 * "Notify 0x<char>". The button-state notify (0x5568) runs unconditionally; the
 * rest are gated on PC2 present + a 1.5 s throttle (SLOT 0x200000D8) + not being in
 * a transfer state (0x19/0x1A). The two loggers are the g_log_func table slots
 * [0] (real prints) and [2] (the "Notify …" trace). Payloads are packed byte-wise,
 * little-endian, exactly as the OEM does — including its quirks (a couple of
 * mis-prefixed error strings, and the 0x5582 payload leaving byte 2 unset). */
void ble_telemetry_change_broadcast(void *ctx_)
{
    uint8_t   *ctx    = (uint8_t *)ctx_;
    uint8_t   *cache  = (uint8_t *)0x200081c8u;   /* telemetry value cache */
    uint8_t   *slot   = (uint8_t *)0x200000d8u;   /* [0] 1.5s timer, [1] lock-state cache, [4/5] debounce, [6] HDC timer */
    log_func_t notify = *(log_func_t *)0x20009da0u;   /* g_log_func table[2] — change trace */
    uint8_t    buf[24];

    /* First entry: arm the 1.5 s broadcast timer + snapshot the initial values. */
    if (slot[0] == SCHED_SLOT_NONE) {
        slot[0] = scheduler_alloc();
        scheduler_set_timer_name(slot[0], 0x5dc, "ble_interval_tmr");
        scheduler_start(slot[0], 0x5dc, 0);
        cache[0] = ctx[0x3c9];
        cache[1] = ctx[0x3cc];
        *(uint16_t *)(cache + 2) = *(uint16_t *)(ctx + 0x3fc);
        *(uint16_t *)(cache + 4) = (uint16_t)charge_level_adc_get();
        *(uint16_t *)(cache + 6) = *(uint16_t *)(ctx + 0x36c);
        *(uint32_t *)(cache + 8) = *(uint32_t *)(ctx + 0x31c);
        *(uint16_t *)(cache + 0xc) = *(uint16_t *)(ctx + 0x3c2);
        *(uint16_t *)(cache + 0xe) = 0;
        cache[0x10] = (uint8_t)ble_unlock_state_get();
    }

    /* 0x5568 — button state (PC0 horn / PC1 boost / PD2), always checked. */
    if (maybe_get_bike_state() != 0x19 && maybe_get_bike_state() != 0x1a &&
        ((unsigned)gpio_pc0_is_low() != cache[0x11] ||
         (unsigned)gpio_pc1_is_low() != cache[0x12] ||
         (_Bool)cache[0x13] != (HAL_GPIO_ReadPin((void *)GPIOD_BASE, 4) == 0))) {
        cache[0x11] = (uint8_t)gpio_pc0_is_low();  buf[0] = cache[0x11];
        cache[0x12] = (uint8_t)gpio_pc1_is_low();  buf[1] = cache[0x12];
        cache[0x13] = (HAL_GPIO_ReadPin((void *)GPIOD_BASE, 4) == 0);  buf[2] = cache[0x13];
        buf[3] = 0;
        notify("Notify 0x5568\r\n");
        if (ssp_ble_enqueue_tx_packet(0x5568, 4, buf, 0) > 0x80) {
            notify("  ERROR SSP place\r\n");
        }
    }

    if (!(HAL_GPIO_ReadPin((void *)GPIOC_BASE, 4) != 0 &&
          systick_now() != *(uint32_t *)(cache + 0x14) &&
          maybe_get_bike_state() != 0x19 && maybe_get_bike_state() != 0x1a)) {
        return;
    }
    *(uint32_t *)(cache + 0x14) = systick_now();

    /* 0x554E — modem info string. */
    {
        char *s = (char *)(*(int *)(ctx + 0x3e8) + 0x20);
        unsigned len = strlen(s);
        if (bounded_strncmp((char *)(cache + 0x18), s, len) != 0 && len < 0x10) {
            memcpy(cache + 0x18, s, len);
            snprintf((char *)buf, 0x18, "%s", s);
            notify("Notify 0x554E\r\n");
            if (ssp_ble_enqueue_tx_packet(0x554e, (uint16_t)strlen((char *)buf), buf, 0) > 0x80) {
                g_log_func("  ERROR SSP place\r\n");
            }
        }
    }

    /* 0x5582 — LED channel bits (OEM leaves payload byte 2 unset). */
    if ((unsigned)ble_get_led_channel_state() != cache[0x28]) {
        cache[0x28] = (uint8_t)ble_get_led_channel_state();
        notify("Notify 0x5582\r\n");
        buf[0] = (uint8_t)(ble_get_led_channel_state() & 1);
        buf[1] = (uint8_t)((ble_get_led_channel_state() << 0x1e) >> 0x1f);
        buf[3] = (uint8_t)((ble_get_led_channel_state() << 0x1d) >> 0x1f);
        if (ssp_ble_enqueue_tx_packet(0x5582, 3, buf, 0) > 0x80) {
            notify("  ERROR SSP place\r\n");
        }
    }

    /* 0x5538 — transmission mode (ctx+0x108). */
    if ((char)ctx[0x108] != (char)cache[0x29]) {
        cache[0x29] = ctx[0x108];
        buf[0] = ctx[0x108];
        notify("Notify 0x5538\r\n");
        if (ssp_ble_enqueue_tx_packet(0x5538, 1, buf, 0) > 0x80) {
            notify("  ERROR SSP place\r\n");
        }
    }

    /* 0x5521 — lock state (cached in slot[1]). */
    if ((unsigned)ble_lock_state_get() != slot[1]) {
        slot[1] = (uint8_t)ble_lock_state_get();
        buf[0] = slot[1];
        notify("Notify 0x5521\r\n");
        if (ssp_ble_enqueue_tx_packet(0x5521, 1, buf, 0) > 0x80) {
            g_log_func("  ERROR SSPB place1\r\n");
        }
    }

    /* 0x5523 — unlock state. */
    if ((unsigned)ble_unlock_state_get() != cache[0x10]) {
        cache[0x10] = (uint8_t)ble_unlock_state_get();
        buf[0] = cache[0x10];
        notify("Notify 0x5523\r\n");
        if (ssp_ble_enqueue_tx_packet(0x5523, 1, buf, 0) > 0x80) {
            g_log_func("  ERROR SSPB place1a\r\n");
        }
    }

    /* 0x5561 — coarse bike status. */
    if ((unsigned)bike_status_coarse_get() != cache[0x2a]) {
        cache[0x2a] = (uint8_t)bike_status_coarse_get();
        buf[0] = cache[0x2a];
        notify("Notify 0x5561\r\n");
        if (ssp_ble_enqueue_tx_packet(0x5561, 1, buf, 0) > 0x80) {
            g_log_func("  ERROR SSPB place2a\r\n");
        }
    }

    /* 0x5562 — bike-state mode (drive 0x0C -> 0, ready 0x0E -> 1, boost 0x07 -> 2). */
    if (maybe_get_bike_state() != cache[0x2b]) {
        cache[0x2b] = maybe_get_bike_state();
        notify("Notify 0x5562\r\n");
        buf[0] = 0;
        if (maybe_get_bike_state() == 0x0c)      buf[0] = 0;
        else if (maybe_get_bike_state() == 0x0e) buf[0] = 1;
        else if (maybe_get_bike_state() == 0x07) buf[0] = 2;
        if (ssp_ble_enqueue_tx_packet(0x5562, 1, buf, 0) > 0x80) {
            g_log_func("  ERROR SSPB place\r\n");
        }
    }

    /* 0x5534 — power level (ctx+0x3C9) + owner-code state (ctx+0x3CB). */
    if ((char)ctx[0x3c9] != (char)cache[0]) {
        cache[0] = ctx[0x3c9];
        buf[0] = ctx[0x3c9];
        buf[1] = ctx[0x3cb];
        notify("Notify 0x5534\r\n");
        if (ssp_ble_enqueue_tx_packet(0x5534, 2, buf, 0) > 0x80) {
            g_log_func("  ERROR SSPB place2\r\n");
        }
    }

    /* 0x5536 — light mode (ctx+0x3CC). */
    if ((char)ctx[0x3cc] != (char)cache[1]) {
        notify("Notify 0x5536\r\n");
        cache[1] = ctx[0x3cc];
        buf[0] = ctx[0x3cc];
        if (ssp_ble_enqueue_tx_packet(0x5536, 1, buf, 0) > 0x80) {
            g_log_func("  ERROR SSPB place3\r\n");
        }
    }

    /* 0x5541 — BMS pack telemetry (10 bytes: clamped SOC + cell/voltage words). */
    {
        int16_t soc = *(int16_t *)(ctx + 0x3fc);
        if (soc != *(int16_t *)(cache + 2) || memcmp(cache + 0x2c, ctx + 0x3d4, 0xe) != 0) {
            *(uint32_t *)(cache + 0x2c) = *(uint32_t *)(ctx + 0x3d4);
            *(uint32_t *)(cache + 0x30) = *(uint32_t *)(ctx + 0x3d8);
            *(uint32_t *)(cache + 0x34) = *(uint32_t *)(ctx + 0x3dc);
            *(int16_t  *)(cache + 0x38) = (int16_t)*(uint32_t *)(ctx + 0x3e0);
            *(int16_t  *)(cache + 2) = soc;
            notify("Notify 0x5541\r\n");
            buf[0] = (uint8_t)telemetry_map_clamp((uint8_t)soc, 0, 0x61, 0, 100);
            buf[1] = ctx[0x422];
            buf[2] = ctx[0x424];
            buf[3] = ctx[0x425];
            buf[4] = ctx[0x3e1];
            buf[5] = ctx[0x3d4];
            buf[6] = ctx[0x3dd];
            buf[7] = ctx[0x3de];
            buf[8] = ctx[0x3df];
            buf[9] = ctx[0x3e0];
            if (ssp_ble_enqueue_tx_packet(0x5541, 10, buf, 0) > 0x80) {
                notify("  ERROR SSP place\r\n");
            }
        }
    }

    /* 0x5543 — charge level. */
    if ((int16_t)charge_level_adc_get() != *(int16_t *)(cache + 4)) {
        notify("Notify 0x5543\r\n");
        *(uint16_t *)(cache + 4) = (uint16_t)charge_level_adc_get();
        buf[0] = (uint8_t)charge_level_adc_get();
        if (ssp_ble_enqueue_tx_packet(0x5543, 1, buf, 0) > 0x80) {
            g_log_func("  ERROR SSPB place5\r\n");
        }
    }

    /* 0x5546 — driver temperature (ctx+0x36C), ±2 hysteresis. */
    if (*(int16_t *)(ctx + 0x36c) + 2 < (int)*(int16_t *)(cache + 6) ||
        (int)*(int16_t *)(cache + 6) < *(int16_t *)(ctx + 0x36c) - 2) {
        notify("Notify 0x5546\r\n");
        *(uint16_t *)(cache + 6) = *(uint16_t *)(ctx + 0x36c);
        buf[0] = ctx[0x36c];
        if (ssp_ble_enqueue_tx_packet(0x5546, 1, buf, 0) > 0x80) {
            g_log_func("  5ERROR SSPB place6\r\n");
        }
    }

    /* 0x5547 — motor temperature (ctx+0x36A), ±2 hysteresis (-0x110 -> 0). */
    if (*(int16_t *)(ctx + 0x36a) + 2 < (int)*(int16_t *)(cache + 0x3a) ||
        (int)*(int16_t *)(cache + 0x3a) < *(int16_t *)(ctx + 0x36a) - 2) {
        notify("Notify 0x5547\r\n");
        int16_t mt = *(int16_t *)(ctx + 0x36a);
        *(int16_t *)(cache + 0x3a) = mt;
        buf[0] = (mt == -0x110) ? 0 : (uint8_t)mt;
        if (ssp_ble_enqueue_tx_packet(0x5547, 1, buf, 0) > 0x80) {
            g_log_func("  6ERROR SSPB place7\r\n");
        }
    }

    /* 0x5531 — odometer (ctx+0x31C). */
    if (*(int *)(ctx + 0x31c) != *(int *)(cache + 8)) {
        notify("Notify 0x5531\r\n");
        *(uint32_t *)buf = *(uint32_t *)(ctx + 0x31c);
        *(uint32_t *)(cache + 8) = *(uint32_t *)buf;
        if (ssp_ble_enqueue_tx_packet(0x5531, 4, buf, 0) > 0x80) {
            g_log_func("  ERROR SSPB place9\r\n");
        }
    }

    /* 0x554F — shifter firmware version (ctx+0x336). */
    if ((int)*(int16_t *)(cache + 0x3c) != (int)*(uint16_t *)(ctx + 0x336)) {
        notify("Notify 0x554F\r\n");
        uint16_t v = *(uint16_t *)(ctx + 0x336);
        *(uint16_t *)(cache + 0x3c) = v;
        snprintf((char *)buf, 0x18, "%d.%d", (int)(v >> 8), (int)(uint8_t)v);
        if (ssp_ble_enqueue_tx_packet(0x554f, (uint16_t)strlen((char *)buf), buf, 0) > 0x80) {
            g_log_func("  ERROR SSP place\r\n");
        }
    }

    /* 0x5550 — BMS firmware version + build (ctx+0x408 / ctx+0x3DA..0x3DC). */
    if (*(int16_t *)(ctx + 0x408) != *(int16_t *)(cache + 0x3e) ||
        *(uint16_t *)(slot + 2) != (uint16_t)ctx[0x3da]) {
        notify("Notify 0x5550\r\n");
        uint16_t v = *(uint16_t *)(ctx + 0x408);
        *(uint16_t *)(cache + 0x3e) = v;
        uint8_t b = ctx[0x3da];
        *(uint16_t *)(slot + 2) = b;
        snprintf((char *)buf, 0x18, "%X.%02X %X.%X.%X",
                 (unsigned)(v >> 8), (unsigned)(uint8_t)v, (unsigned)b,
                 (unsigned)ctx[0x3db], (unsigned)ctx[0x3dc]);
        if (ssp_ble_enqueue_tx_packet(0x5550, (uint16_t)strlen((char *)buf), buf, 0) > 0x80) {
            g_log_func("  ERROR SSP place\r\n");
        }
    }

    /* 0x554C — motorware version (ctx+0x388). */
    if (*(int *)(ctx + 0x388) != *(int *)(cache + 0x40)) {
        notify("Notify 0x554C\r\n");
        uint32_t v = *(uint32_t *)(ctx + 0x388);
        *(uint32_t *)(cache + 0x40) = v;
        snprintf((char *)buf, 0x18, "%c.%d.%02d.%02d",
                 (int)(v >> 0x18), (int)((v & 0xffffff) >> 0x10),
                 (int)((v & 0xffff) >> 8), (int)(v & 0xff));
        if (ssp_ble_enqueue_tx_packet(0x554c, (uint16_t)strlen((char *)buf), buf, 0) > 0x80) {
            g_log_func("  ERROR SSP place\r\n");
        }
    }

    /* 0x5542 — charge plug state (7 bytes). */
    if ((unsigned)ble_get_charge_plug_state() != cache[0x44] ||
        (char)ctx[0x3d5] != (char)cache[0x45]) {
        notify("Notify 0x5542\r\n");
        cache[0x45] = ctx[0x3d5];
        cache[0x44] = (uint8_t)ble_get_charge_plug_state();
        buf[0] = cache[0x44];
        buf[1] = 0;
        buf[2] = ctx[0x3d5];
        buf[3] = ctx[0x3d6];
        buf[4] = ctx[0x3d7];
        buf[5] = ctx[0x3d8];
        buf[6] = ctx[0x3d9];
        if (ssp_ble_enqueue_tx_packet(0x5542, 7, buf, 0) > 0x80) {
            notify("  ERROR SSP place\r\n");
        }
    }

    /* 0x5563 — the 64-bit fault-flag pair (ctx+0x3B8 low / +0x3BC high), debounced. */
    if ((*(uint32_t *)(ctx + 0x3bc) != *(uint32_t *)(cache + 0x4c) ||
         *(uint32_t *)(ctx + 0x3b8) != *(uint32_t *)(cache + 0x48)) &&
        ble_interval_debounce(slot + 4, 0, *(uint32_t *)(ctx + 0x3b8),
                              *(uint32_t *)(ctx + 0x3bc), 0x40000, 0) != 0 &&
        ble_interval_debounce(slot + 5, 0, *(uint32_t *)(ctx + 0x3b8),
                              *(uint32_t *)(ctx + 0x3bc), 0x100000, 0) != 0) {
        notify("Notify 0x5563\r\n");
        g_log_func("Error Flags: 0x%08X %08X\r\n",
                   *(uint32_t *)(ctx + 0x3bc), *(uint32_t *)(ctx + 0x3b8));
        *(uint32_t *)(cache + 0x48) = *(uint32_t *)(ctx + 0x3b8);
        *(uint32_t *)(cache + 0x4c) = *(uint32_t *)(ctx + 0x3bc);
        *(uint32_t *)buf       = *(uint32_t *)(ctx + 0x3b8);
        *(uint32_t *)(buf + 4) = *(uint32_t *)(ctx + 0x3bc);
        if (ssp_ble_enqueue_tx_packet(0x5563, 8, buf, 0) > 0x80) {
            g_log_func("  ERROR SSPB place8\r\n");
        }
    }

    /* The remaining broadcasts are gated on the same 1.5 s slot going idle. */
    if (scheduler_slot_is_idle(slot[0]) != 0) {
        scheduler_start(slot[0], 0x5dc, 0);

        if (*(uint16_t *)(ctx + 0x3b0) > 25000 &&
            maybe_enqueue_tx_message(0xc, 0, 0, 1) > 0x10) {
            g_log_func("  ERROR SSP place\r\n");
        }

        /* 0x5539 — front assist speed limit (ctx+0x374). */
        if (*(int16_t *)(ctx + 0x374) != *(int16_t *)(cache + 0x50)) {
            notify("Notify 0x5539\r\n");
            *(uint16_t *)(cache + 0x50) = *(uint16_t *)(ctx + 0x374);
            *(uint32_t *)buf = *(uint16_t *)(ctx + 0x374);
            if (ssp_ble_enqueue_tx_packet(0x5539, 4, buf, 0) > 0x80) {
                g_log_func("  ERROR SSPB place9\r\n");
            }
        }

        /* 0x5548 — the 4 assist speed presets (ctx+0x3A4, 8 bytes, sent in place). */
        if (*(int16_t *)(ctx + 0x3a4) != *(int16_t *)(cache + 0x52) ||
            *(int16_t *)(ctx + 0x3aa) != *(int16_t *)(cache + 0x54) ||
            *(int16_t *)(ctx + 0x3a6) != *(int16_t *)(cache + 0x56) ||
            *(int16_t *)(ctx + 0x3a8) != *(int16_t *)(cache + 0x58)) {
            notify("Notify 0x5548\r\n");
            *(uint16_t *)(cache + 0x52) = *(uint16_t *)(ctx + 0x3a4);
            *(uint16_t *)(cache + 0x54) = *(uint16_t *)(ctx + 0x3aa);
            *(uint16_t *)(cache + 0x56) = *(uint16_t *)(ctx + 0x3a6);
            *(uint16_t *)(cache + 0x58) = *(uint16_t *)(ctx + 0x3a8);
            if (ssp_ble_enqueue_tx_packet(0x5548, 8, ctx + 0x3a4, 0) > 0x80) {
                notify(" SSP place\r\n");
            }
        }

        /* 0x5551 — the test-mode versions blob (0x60 bytes). */
        {
            int blob[2];
            ble_build_testmode_versions_blob((uint32_t *)blob);
            if (*(int *)(cache + 0x5c) != blob[0]) {
                *(int *)(cache + 0x5c) = blob[0];
                notify("Notify 0x5551\r\n");
                void *payload = (void *)ble_build_testmode_versions_blob(0);
                if (ssp_ble_enqueue_tx_packet(0x5551, 0x60, payload, 0) > 0x80) {
                    notify("  ERROR SSP place\r\n");
                }
            }
        }

        /* 0x553A — rear assist word (ctx+0x372). */
        if (*(int16_t *)(ctx + 0x372) != *(int16_t *)(cache + 0x60)) {
            notify("Notify 0x553A\r\n");
            *(uint16_t *)(cache + 0x60) = *(uint16_t *)(ctx + 0x372);
            *(uint32_t *)buf = *(uint16_t *)(ctx + 0x372);
            if (ssp_ble_enqueue_tx_packet(0x553a, 4, buf, 0) > 0x80) {
                g_log_func("  ERROR SSPB place9\r\n");
            }
        }

        /* 0x5584 — ambient light + dark threshold (ctx+0x102), ±0x19 hysteresis. */
        if (light_sensor_read_step() != 0xfffe) {
            int lx = light_sensor_read_step();
            if (lx + 0x19 < (int)(unsigned)*(uint16_t *)(cache + 0x62) ||
                (int)(unsigned)*(uint16_t *)(cache + 0x62) < lx - 0x19) {
                notify("Notify 0x5584\r\n");
                *(int16_t *)(cache + 0x62) = (int16_t)lx;
                buf[0] = (uint8_t)((unsigned)lx >> 8);
                buf[1] = (uint8_t)lx;
                buf[2] = (uint8_t)((unsigned)*(uint16_t *)(ctx + 0x102) >> 8);
                buf[3] = ctx[0x102];
                if (ssp_ble_enqueue_tx_packet(0x5584, 4, buf, 0) > 0x80) {
                    g_log_func("  ERROR SSP place\r\n");
                }
            }
        }

        /* Every 10 passes, re-trigger an HDC1080 temperature/humidity read. */
        cache[0x64] = (uint8_t)(cache[0x64] + 1);
        if (cache[0x64] == 10) {
            cache[0x64] = 0;
            if (hdc1080_set_pointer((void *)0x20009b04u) == 0) {
                if (slot[6] == SCHED_SLOT_NONE) {
                    slot[6] = scheduler_alloc();
                    scheduler_set_timer_name(slot[6], 100, "hdc_read_tmr");
                    scheduler_start(slot[6], 100, 0);
                }
            } else {
                cache[0x65] = (uint8_t)(cache[0x65] + 1);
                g_log_func(" ERR HDC start\r\n");
            }
            if (cache[0x65] == 3) {
                cache[0x65] = 0;
                g_log_func("NAK\r\n", clock_pulse_gpioa8_until_pc9());
            }
        }

        /* 0x5532 — speed (ctx+0x3C2), broadcast when the /10 km/h bucket changes. */
        if ((uint32_t)(((uint64_t)0xCCCCCCCDULL * *(uint16_t *)(ctx + 0x3c2)) >> 0x22) !=
            (uint32_t)(((uint64_t)0xCCCCCCCDULL * *(uint16_t *)(cache + 0xc)) >> 0x22)) {
            notify("Notify 0x5532\r\n");
            uint16_t sp = *(uint16_t *)(ctx + 0x3c2);
            *(uint16_t *)(cache + 0xc) = sp;
            int32_t out = (int32_t)(((uint64_t)0x66666667ULL * (uint32_t)(sp + 5)) >> 0x22);
            if (ssp_ble_enqueue_tx_packet(0x5532, 4, &out, 0) > 0x80) {
                g_log_func("  ERROR SSPB placeA\r\n");
            }
        }
    }

    /* The HDC1080 result timer fired -> read the temperature and broadcast 0x5545. */
    if (scheduler_slot_is_idle(slot[6]) != 0) {
        short  temp;
        unsigned short rh;
        scheduler_release(slot + 6);
        if (hdc1080_read((void *)0x20009b04u, &temp, &rh) != 0) {
            g_log_func(" ERR HDC read\r\n");
        }
        int16_t tC = (int16_t)(temp / 10);
        if (tC != *(int16_t *)(cache + 0xe)) {
            notify("Notify 0x5545\r\n");
            *(int16_t *)(cache + 0xe) = tC;
            buf[0] = (uint8_t)tC;
            if (ssp_ble_enqueue_tx_packet(0x5545, 1, buf, 0) > 0x80) {
                g_log_func("  ERROR SSPB placeB\r\n");
            }
        }
    }
}
