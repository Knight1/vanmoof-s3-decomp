#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "app_state.h"
#include "audio.h"
#include "console.h"
#include "flash.h"
#include "log.h"
#include "rtc.h"
#include "scheduler.h"
#include "sensor.h"
#include "ssp.h"
#include "stm32f413_gpio.h"
#include "systick.h"
#include "util.h"
#include "watchdog.h"

#define LOGIN_FAIL_LIMIT      5u     /* lockout after N consecutive bad lines */
#define LOGIN_LOCKOUT_TICKS   5000u  /* 0x1388 — 5 s at 1 ms/tick */

#define VOLUME_MAX            0x40u  /* "Volume 0..64" range check */
#define AUDIO_AMP_ENABLE_PIN  (1u << 2)  /* PE2 — drive high to enable amp */
#define AUDIO_AMP_MUTE_PIN    (1u << 5)  /* PD5 — drive high to mute */

/* Helpers we recognise (vendor-stock CubeF4 HAL + newlib). They're
 * supplied by the vendored upstream sources at link-time. */
extern void HAL_GPIO_WritePin(void *GPIOx, uint16_t pin_mask, int state);

/* Helpers we have not decoded yet — kept as opaque extern declarations
 * so we can call them at the right addresses for behavioural
 * equivalence without committing to a name we'd have to revise. */
/* config_persist_dual_bank + struct boot_cfg_block come from flash.h. */

/* Hard-coded fallback password. Reading the OEM rodata at 0x080547EC.
 * Accepted in addition to whatever the user has stored in
 * g_app_state.ctx_sub->user_password — works even when that slot is
 * empty, since strcmp against an empty user_password always returns
 * non-zero for non-empty input and the second comparison runs
 * unconditionally. */
static const char k_login_fallback_password[] =
    "vEVjGF!paYsM2EBV8SoDT8*T0eB&#T6xevaoxCaO";

/* Console line tokenizer (OEM 0x08040A5C). Delimiters are space / `.` / `:`;
 * the line terminator is NUL. Leaves `*pp` at the first character of the next
 * token (or on the terminating NUL when the line runs out). */
int console_next_token(char **pp)
{
    char c;

    if (**pp == '\0') {
        return 0;
    }

    /* Skip the current token. */
    for (;;) {
        c = **pp;
        if (c == '\0' || c == ' ' || c == '.' || c == ':') {
            break;
        }
        (*pp)++;
    }

    /* Skip the run of delimiters to the next token. */
    for (;;) {
        c = **pp;
        if ((c != ' ' && c != '.' && c != ':') || c == '\0') {
            break;
        }
        (*pp)++;
    }

    return c != '\0';
}

void login_handler(char *input)
{
    /* If a previous lockout has armed g_console_state.login_state with
     * a scheduler slot, see whether that slot has expired. If so,
     * release it (which also resets login_state back to
     * SCHED_SLOT_NONE) and clear the fail counter before processing
     * this line. */
    if (scheduler_slot_is_idle(g_console_state.login_state)) {
        scheduler_release(&g_console_state.login_state);
        g_app_state.ctx_sub->fail_count = 0;
    }

    if (input[0] == '\0') {
        return;
    }

    if (g_console_state.login_state != SCHED_SLOT_NONE) {
        /* Lockout slot still has time on it. Re-arm it to a fresh
         * window — any input typed during cooldown extends the wait —
         * and stall the user. */
        scheduler_start(g_console_state.login_state, LOGIN_LOCKOUT_TICKS, 0);
        g_log_func("Please wait..");
        return;
    }

    /* Try the user-configurable service password first. The
     * `user_password[0] != '\0'` guard is defensive: strcmp wouldn't
     * report a match between non-empty input and an empty stored
     * password anyway, but the OEM code re-checks. */
    if (strcmp(input, g_app_state.ctx_sub->user_password) == 0
            && g_app_state.ctx_sub->user_password[0] != '\0') {
        goto login_ok;
    }
    /* Fall back to the hard-coded password. */
    if (strcmp(input, k_login_fallback_password) == 0) {
        goto login_ok;
    }

    /* Mismatch. Log + count + arm lockout on the 5th consecutive
     * failure. The compare is on the pre-increment value, so the
     * branch fires when this call is the LIMIT-th failure. */
    g_log_func("Error login\r\n");
    {
        uint8_t fc = g_app_state.ctx_sub->fail_count;
        g_app_state.ctx_sub->fail_count = (uint8_t)(fc + 1u);
        if (fc == LOGIN_FAIL_LIMIT - 1u) {
            uint8_t slot = scheduler_alloc();
            g_console_state.login_state = slot;
            scheduler_start(slot, LOGIN_LOCKOUT_TICKS, 0);
        }
    }
    return;

login_ok:
    g_app_state.ctx_sub->fail_count = 0;
    g_app_state.ctx_sub->logged_in = 1;
    g_log_func("\r\nWelcome to ES3\r\n");
}

/* Internal: shared body of `volume_low_set` / `volume_medium_set` /
 * `volume_high_set` — they differ only in which byte in `ctx_sub` receives the
 * parsed value (+0x105/+0x106/+0x107). The function deliberately mirrors the OEM
 * control flow,
 * including the snapshot memcpy of `ctx_sub[0x104..0x1C4]` onto the
 * stack just before the audio-engine apply call. The snapshot
 * doesn't seem to be read back anywhere we've decoded, but it's
 * preserved verbatim so we don't accidentally drop a side-effect
 * that some not-yet-decoded callee depends on. */
static void volume_set_common(char *input, uint8_t *target)
{
    char     snapshot[0xC0];
    char    *cursor = input;
    uint8_t  parsed;

    if (!console_next_token(&cursor)) {
        g_log_func("Volume %d\r\n", *target);
        return;
    }

    parsed = (uint8_t)strtol(cursor, NULL, 10);
    if (parsed > VOLUME_MAX) {
        g_log_func("Volume 0..64\r\n");
        return;
    }

    /* Drive the audio amp: PE2 high, PD5 low (un-mute / power-up). */
    HAL_GPIO_WritePin((void *)GPIOE_BASE, AUDIO_AMP_ENABLE_PIN, 1);
    HAL_GPIO_WritePin((void *)GPIOD_BASE, AUDIO_AMP_MUTE_PIN,   0);

    *target = parsed;
    g_log_func("Volume %d\r\n", parsed);

    memcpy(snapshot,
           (void *)((char *)g_app_state.ctx_sub + 0x104),
           sizeof snapshot);

    {
        uint32_t res = config_persist_dual_bank(
            g_app_state.ctx_sub->audio_engine_cfg[0],
            g_app_state.ctx_sub->audio_engine_cfg[1],
            g_app_state.ctx_sub->audio_engine_cfg[2],
            g_app_state.ctx_sub->audio_engine_cfg[3],
            *(const struct boot_cfg_block *)snapshot);
        g_log_func("res: %d\r\n", res);
    }

    if (amp_volume_brownout_apply(&parsed) != 0) {
        g_log_func(" ERR set volume\r\n");
    }

    if (parsed == 0) {
        /* Power down: invert the amp pins, print confirmation. */
        HAL_GPIO_WritePin((void *)GPIOE_BASE, AUDIO_AMP_ENABLE_PIN, 0);
        HAL_GPIO_WritePin((void *)GPIOD_BASE, AUDIO_AMP_MUTE_PIN,   1);
        g_log_func("Audio off\r\n");
    }
}

/* The `vollow` / `volmid` / `volhigh` console commands write ctx_sub +0x105 /
 * +0x106 / +0x107 respectively. (The low/medium/high binding is taken from the
 * console command table @ 0x0804F5C4 — the dump labels alone are off by one.) */
void volume_low_set(char *input)
{
    volume_set_common(input, &g_app_state.ctx_sub->volume_low);
}

void volume_medium_set(char *input)
{
    volume_set_common(input, &g_app_state.ctx_sub->volume_medium);
}

void volume_high_set(char *input)
{
    volume_set_common(input, &g_app_state.ctx_sub->volume_high);
}

void console_start_motor_update(char *input)
{
    (void)input;
    g_log_func("Start motor update..");
    update_mode_request(4);
}

void console_soc_set(char *input)
{
    char *cursor = input;
    long parsed;

    if (!console_next_token(&cursor)) {
        return;
    }

    parsed = strtol(cursor, NULL, 10);
    g_app_state.ctx_sub->set_soc = (uint8_t)parsed;
    g_log_func("Set SOC %d\r\n", (uint16_t)parsed);
    /* OEM passes 2 here, which announce_mark() ignores (it only handles
     * channels 0/1). The SOC override is picked up directly by the super-loop
     * each iteration, so the no-op call has no observable effect. */
    announce_mark(2);
}

/* `region` console command (OEM 0x080421CC). Sets the bike's region / speed
 * mode — 0=EU, 1=US, 2=JP, 3=OFFROAD (OFFROAD lifts the regulated speed cap) —
 * then echoes the current lock state and region. The lock state at `+0x144` is
 * read and written back unchanged (only the region is set here); a value < 3 is
 * required to accept a change. Applying the region re-runs the config-apply
 * (config_persist_dual_bank) that pushes the `ctx_sub[0xF4..0x103]` block to the drive
 * subsystem — the same path the volume commands use; the OEM snapshots
 * `ctx_sub[0x104..0x1C4]` onto the stack first because the apply helper reads
 * it back from the caller's frame. */
void console_region_set(char *input)
{
    char     snapshot[0xC0];
    char    *cursor = input;
    uint8_t  lock = g_app_state.ctx_sub->region_lock;

    if (console_next_token(&cursor)) {
        unsigned long parsed = (unsigned long)strtol(cursor, NULL, 10);

        if ((parsed & 0xFFFFu) < 4u && lock < 3u) {
            g_app_state.ctx_sub->region = (uint8_t)parsed;
            g_app_state.ctx_sub->region_lock = lock;

            memcpy(snapshot,
                   (void *)((char *)g_app_state.ctx_sub + 0x104),
                   sizeof snapshot);

            {
                uint32_t res = config_persist_dual_bank(
                    g_app_state.ctx_sub->audio_engine_cfg[0],
                    g_app_state.ctx_sub->audio_engine_cfg[1],
                    g_app_state.ctx_sub->audio_engine_cfg[2],
                    g_app_state.ctx_sub->audio_engine_cfg[3],
                    *(const struct boot_cfg_block *)snapshot);
                g_log_func("Set region and lock, res: %d\r\n", res);
            }
        } else {
            g_log_func("Parameter 0..3 0..2\r\n");
        }
    }

    switch (g_app_state.ctx_sub->region_lock) {
    case 1:  g_log_func("Region (off road disabled): "); break;
    case 2:  g_log_func("Region (locked): ");            break;
    default: g_log_func("Region (unlocked): ");          break;
    }

    switch (g_app_state.ctx_sub->region) {
    case 0: g_log_func("0 = REGION_EU\r\n");      break;
    case 1: g_log_func("1 = REGION_US\r\n");      break;
    case 2: g_log_func("2 = REGION_JP\r\n");      break;
    case 3: g_log_func("3 = REGION_OFFROAD\r\n"); break;
    }
}

/* ===================================================================
 * Additional debug-console command handlers (dispatch table @ flash
 * 0x0804F5C4 — see docs/console.md for the full 49-command map). Each is a
 * void handler(char *args). The logger g_log_func is the shared printf-style
 * fn ptr. The OEM reaches session-context fields by raw byte offset off the
 * ctx_sub pointer; CTXB is that byte view (== *(g_app_state + 0x2DC),
 * session_ctx @ 0x200083A8) and the CTX_* macros name the offsets used here. */
#define APP_CTX_BASE  0x20009368u                      /* g_app_state base */
#define CTXB          ((uint8_t *)g_app_state.ctx_sub) /* the session_ctx byte view */

/* session_ctx (ctx_sub) field offsets touched by the console handlers. */
#define CTX_AUDIO_GROUP_LOW   0x0F4   /* audio config words (persisted to flash) */
#define CTX_AUDIO_GROUP_MED   0x0F8
#define CTX_AUDIO_GROUP_HIGH  0x0FC
#define CTX_AUDIO_GROUP3      0x100
#define CTX_AUDIO_CFG_BLOCK   0x104   /* start of the 0xC0-byte audio config block */
#define CTX_WHEELSIZE         0x10B   /* wheel diameter: 0 = 24 inch, 1 = 28 inch */
#define CTX_LOGGED_IN         0x2D9   /* console session authenticated flag */
#define CTX_LOG_TO_APP        0x313   /* log sink: APP (1) vs Serial (0) */
#define CTX_DISTANCE          0x31C   /* manual trip distance, tenths of a km (u32) */
#define CTX_SHIFT_COUNTER     0x338   /* eShifter operation counter (u32) */
#define CTX_REDIRECT_BLE      0x34C   /* console→UART7 (CC2642 BLE chip) redirect */
#define CTX_REDIRECT_GSM      0x34D   /* console→UART2 (u-blox modem) redirect */
#define CTX_DBG_SHIFTER       0x34E   /* stream Modbus-shifter debug */
#define CTX_DBG_BMS           0x34F   /* stream Modbus-BMS debug */
#define CTX_LOOP_LAST_US      0x358   /* super-loop last period */
#define CTX_LOOP_MS           0x35C
#define CTX_LOOP_PEAK_US      0x360   /* super-loop peak period */
#define CTX_SPEED_OVR_A       0x3C4   /* speed-override fields (two adjacent u16) */
#define CTX_SPEED_OVR_B       0x3C6
#define CTX_RIDE_CHANGE       0x3CB   /* powerchange / "ride change" enable */
#define CTX_MODEM_INFO        0x3E8   /* pointer to the u-blox modem-info block */
#define CTX_SHIFTER_HW        0x520   /* shifter HW-version word (0x201 = MT shifter) */
#define CTX_GEAR_COUNT        0x59C   /* number of valid gear positions */

/* Inter-module Modbus injection (the b... / s... diagnostic console commands). */
#define MB_SLAVE_BAT          0xAA    /* battery / BMS module */
#define MB_SLAVE_SHIFT        0x20    /* eShifter module */
#define MB_FUNC_READ          0x03    /* read holding registers */
#define MB_FUNC_WRITE_SINGLE  0x06    /* write single register */
#define MB_FUNC_WRITE_MULTI   0x10    /* write multiple registers */
#define MB_FRAME_COUNT_OFF    0x84    /* register/byte-count position in the staging buffer */

/* Named SRAM scheduler-slot bytes + console redirect / SMS-restart selectors. */
#define SCHED_SLOT_REC        0x2000010Eu   /* base of the console scheduler-slot record block */
#define SCHED_SLOT_TELEMETRY  0x20000110u   /* shared battery/motor status poll slot (REC+2) */
#define SCHED_SLOT_SHIFTDEBUG 0x20000111u
#define SCHED_SLOT_BATRESET   0x20000112u
#define CONSOLE_REDIRECT_SEL  0x2000019Cu   /* console UART redirect: 2 = shiftware */
#define SMS_INFO_STEP         0x200000E5u   /* sms_info_tracking_state_machine step */

/* Battery- and shifter-side Modbus injectors (the `b*`/`s*` console commands
 * push a raw frame onto the corresponding inter-module bus). Not yet sourced. */
extern int modbus_bat_submit(void *frame);    /* 0x08039DDC — battery slave 0xAA */
extern int modbus_shift_submit(void *frame);  /* 0x080378A0 — shifter slave 0x20 */

/* `distance` — manually set the trip distance (tenths of a km) at ctx+CTX_DISTANCE and
 * echo it as whole.fraction km (OEM 0x08041360). No-arg path is silent. */
void console_cmd_distance(char *args)
{
    char *p = args;
    if (console_next_token(&p) == 0) {
        return;
    }
    uint32_t v = (uint32_t)strtol(p, NULL, 10);
    *(uint32_t *)(CTXB + CTX_DISTANCE) = v;
    g_log_func("Set %u.%u Km\r\n", v / 10u, v % 10u);
}

/* `wheelsize` — set the wheel-diameter flag at ctx+CTX_WHEELSIZE (24->0, 28->1),
 * persist the config to both flash banks, then echo (OEM 0x08042120). */
void console_cmd_wheelsize(char *args)
{
    char *p = args;
    if (console_next_token(&p) != 0) {
        uint8_t  snapshot[0xC0];
        uint32_t val = (uint32_t)strtol(p, NULL, 10) & 0xFFFF;
        if (val == 24) {
            CTXB[CTX_WHEELSIZE] = 0;
        } else if (val == 28) {
            CTXB[CTX_WHEELSIZE] = 1;
        } else {
            g_log_func("wheel 24..28 for 24/28 inch\r\n");
        }
        memcpy(snapshot, CTXB + CTX_AUDIO_CFG_BLOCK, sizeof snapshot);
        uint32_t res = config_persist_dual_bank(*(uint32_t *)(CTXB + CTX_AUDIO_GROUP_LOW), *(uint32_t *)(CTXB + CTX_AUDIO_GROUP_MED),
                                    *(uint32_t *)(CTXB + CTX_AUDIO_GROUP_HIGH), *(uint32_t *)(CTXB + CTX_AUDIO_GROUP3),
                                    *(const struct boot_cfg_block *)snapshot);
        g_log_func("res: %d\r\n", res);
    }
    g_log_func("Wheel: %s\r\n", CTXB[CTX_WHEELSIZE] == 0 ? "24 inch" : "28 inch");
}

/* `speed` — override the speed setpoint, writing the value into two adjacent
 * u16 fields at ctx+CTX_SPEED_OVR_A/+0x3C6 (OEM 0x0804131C). No-arg path is silent. */
void console_cmd_speed(char *args)
{
    char *p = args;
    if (console_next_token(&p) == 0) {
        return;
    }
    long v = strtol(p, NULL, 10);
    *(uint16_t *)(CTXB + CTX_SPEED_OVR_A) = (uint16_t)v;
    *(uint16_t *)(CTXB + CTX_SPEED_OVR_B) = (uint16_t)v;
    g_log_func("Speed %d\r\n", (uint16_t)v);
}

/* `shipping` — request the bike state machine enter shipping mode (state 7),
 * unless it is already in a sticky/locked state (OEM 0x080415D0). */
void console_cmd_shipping(char *args)
{
    (void)args;
    g_log_func("Set shipping mode\r\n");
    maybe_set_state_if_unlocked(7);
}

/* `gsminfo` — dump the cached u-blox modem identity from the modem-info block
 * at *(ctx+CTX_MODEM_INFO) (16-byte text fields), plus the BLE MAC (ctx+0x390..0x395).
 * OEM 0x08040D14. */
void console_cmd_gsminfo(char *args)
{
    (void)args;
    char *m = *(char **)(CTXB + CTX_MODEM_INFO);
    g_log_func("manufacturer %s\r\n", m + 0x00);
    g_log_func("model        %s\r\n", m + 0x10);
    g_log_func("fw version   %s\r\n", m + 0x20);
    g_log_func("imei         %s\r\n", m + 0x30);
    g_log_func("imsi         %s\r\n", m + 0x40);
    g_log_func("iccid        %s\r\n", m + 0x50);
    g_log_func("csq          %s\r\n", m + 0x65);
    g_log_func("BLE MAC      %02X.%02X.%02X.%02X.%02X.%02X\r\n",
               CTXB[0x390], CTXB[0x391], CTXB[0x392], CTXB[0x393], CTXB[0x394], CTXB[0x395]);
}

/* `gsmstart` — log "Start GSM" and restart the modem SMS-info state machine by
 * zeroing its step byte at SRAM 0x200000E5 (OEM 0x08041D38). */
void console_cmd_gsmstart(char *args)
{
    (void)args;
    g_log_func("Start GSM\r\n");
    *(volatile uint8_t *)SMS_INFO_STEP = 0;   /* sms_info_tracking_state_machine step */
}

/* `bwritereg` — inject a Modbus "write single register" (func 0x06) to the
 * battery/BMS (slave 0xAA): bwritereg [register] [data] (OEM 0x08041C84). */
void console_cmd_bwritereg(char *args)
{
    char *p = args;
    if (console_next_token(&p) == 0) {
        g_log_func("usage: writereg [register] [data]\r\n ");
        return;
    }
    uint32_t reg  = (uint32_t)strtol(p, NULL, 10);
    uint32_t data = (console_next_token(&p) == 0) ? 0
                    : ((uint32_t)strtol(p, NULL, 10) & 0xFFFF);
    g_log_func("Register %d data %d\r\n", reg & 0xFFFF, data);

    uint8_t frame[6];
    frame[0] = MB_SLAVE_BAT;                            /* battery/BMS slave */
    frame[1] = MB_FUNC_WRITE_SINGLE;                            /* write single register */
    *(uint16_t *)&frame[2] = (uint16_t)reg;
    frame[4] = (uint8_t)data;
    frame[5] = (uint8_t)(data >> 8);
    if (modbus_bat_submit(frame) != 0) {
        g_log_func("  Error\r\n");
    }
}

/* `breadreg` — inject a Modbus "read holding registers" (func 0x03) to the
 * battery (slave 0xAA): breadreg [register] [length] (OEM 0x08041B30). */
void console_cmd_breadreg(char *args)
{
    char *p = args;
    if (console_next_token(&p) == 0) {
        g_log_func("usage: readreg [register] [length]\r\n");
        return;
    }
    uint32_t reg = (uint32_t)strtol(p, NULL, 10);
    uint32_t len = (console_next_token(&p) == 0) ? 1
                   : ((uint32_t)strtol(p, NULL, 10) & 0xFFFF);
    g_log_func("Read Register %d  length %d\r\n", reg & 0xFFFF, len);

    uint8_t req[0x110];
    req[0] = MB_SLAVE_BAT;                              /* battery slave */
    req[1] = MB_FUNC_READ;                              /* read holding registers */
    *(uint16_t *)&req[2] = (uint16_t)reg;
    req[MB_FRAME_COUNT_OFF] = (uint8_t)len;                   /* register count */
    if (modbus_bat_submit(req) != 0) {
        g_log_func("  MB Error\r\n");
    }
}

/* `swritedata` — inject a Modbus "write multiple registers" (func 0x10) to the
 * shifter (slave 0x20): swritedata [register] [data] [length]; the `length`
 * 16-bit words are filled big-endian with `data` (OEM 0x0804168C). The OEM
 * latent overflow (no bound check; length>64 overruns the frame) is preserved. */
void console_cmd_swritedata(char *args)
{
    char *p = args;
    if (console_next_token(&p) == 0) {
        g_log_func("usage: writedata [register] [data] [length]\r\n");
        return;
    }
    uint32_t reg    = (uint32_t)strtol(p, NULL, 10);
    uint32_t data   = (console_next_token(&p) == 0) ? 0
                      : ((uint32_t)strtol(p, NULL, 10) & 0xFFFF);
    uint32_t length = (console_next_token(&p) == 0) ? 0
                      : ((uint32_t)strtol(p, NULL, 10) & 0xFFFF);
    g_log_func("Register %d data %d length %d\r\n", reg & 0xFFFF, data, length);

    uint8_t frame[0x86];
    frame[0] = MB_SLAVE_SHIFT;                            /* shifter slave */
    frame[1] = MB_FUNC_WRITE_MULTI;                            /* write multiple registers */
    *(uint16_t *)&frame[2] = (uint16_t)reg;
    frame[MB_FRAME_COUNT_OFF] = (uint8_t)(length << 1);       /* byte count */
    memset(&frame[4], 0, 0x80);
    for (uint32_t i = 0; i < length * 2u; i += 2) {
        frame[4 + i]     = (uint8_t)(data >> 8);
        frame[4 + i + 1] = (uint8_t)data;
    }
    if (modbus_shift_submit(frame) != 0) {
        g_log_func("  Error\r\n");
    }
}

/* `sreadreg` — inject a Modbus "read holding registers" (func 0x03) to the
 * shifter (slave 0x20): sreadreg [register] [length] (OEM 0x080414A4). */
void console_cmd_sreadreg(char *args)
{
    char *p = args;
    if (console_next_token(&p) == 0) {
        g_log_func("usage: readreg [register] [length]\r\n");
        return;
    }
    uint32_t reg   = (uint32_t)strtol(p, NULL, 10);
    uint32_t count = (console_next_token(&p) == 0) ? 1
                     : ((uint32_t)strtol(p, NULL, 10) & 0xFFFF);
    g_log_func("Read Register %d  length %d\r\n", reg & 0xFFFF, count);

    uint8_t req[0x110];
    req[0] = MB_SLAVE_SHIFT;                              /* shifter slave */
    req[1] = MB_FUNC_READ;                              /* read holding registers */
    *(uint16_t *)&req[2] = (uint16_t)reg;
    req[MB_FRAME_COUNT_OFF] = (uint8_t)count;
    if (modbus_shift_submit(req) != 0) {
        g_log_func("  MB Error\r\n");
    }
}

/* `factory-shipping` — full factory-shipping powerdown (OEM 0x08041FF8, help
 * "Factory shipping mode (ignores BMS)"). Drops the power/enable GPIO rails,
 * deinits subsystems, sends a BLE notify (cmd 0x112) and a Modbus command
 * (0x14) — draining each TX queue — then powers the board down via
 * shipping_powerdown_deinit(6), which does not return. */
extern int      ble_tx_count_free_slots(void);    /* 0x0803FA98 — 0x80 = all free */
extern void     ble_tx_pump(void);                /* 0x0803F6B4 */
extern char     modbus_tx_count_free_slots(void); /* 0x0803A510 — 0x10 = all free */
extern void     modbus_tx_pump(void);             /* 0x0803A278 */
extern uint32_t maybe_enqueue_tx_message(uint16_t type, uint32_t len, void *payload, uint8_t flags); /* 0x0803A1C4 */
extern void     shipping_powerdown_deinit(int mode); /* 0x080382D0 — no return */
extern void     obj_set_field34(int);
extern void     obj_set_field38(int);
extern void     led_channel3_set_brightness(int);
extern void     display_send_init_cmd(void);
extern int      lis3dh_powerdown(void);
extern void     led_driver_enter_shipping_mode(void);
/* stc_gas_gauge_set_run (sensor.h) / wwdg_apb_clk_disable (watchdog.h). */

void console_cmd_factory_shipping(char *args)
{
    uint16_t modbus_payload;
    uint8_t  ble_payload;
    (void)args;

    g_log_func("Set factory shipping mode\r\n");

    HAL_GPIO_WritePin((void *)GPIOD_BASE, 0x2000, 0);   /* GPIOD */
    HAL_GPIO_WritePin((void *)GPIOD_BASE, 0x8000, 0);
    HAL_GPIO_WritePin((void *)GPIOE_BASE, 0x0004, 0);   /* GPIOE */
    HAL_GPIO_WritePin((void *)GPIOA_BASE, 0x8000, 0);   /* GPIOA */
    HAL_GPIO_WritePin((void *)GPIOA_BASE, 0x1000, 0);
    HAL_GPIO_WritePin((void *)GPIOE_BASE, 0x0040, 1);

    obj_set_field34(0);
    obj_set_field38(0);
    led_channel3_set_brightness(0);
    display_send_init_cmd();
    (void)lis3dh_powerdown();

    HAL_GPIO_WritePin((void *)GPIOE_BASE, 0x0020, 1);
    HAL_GPIO_WritePin((void *)GPIOB_BASE, 0x0200, 1);   /* GPIOB */
    systick_delay(10);
    HAL_GPIO_WritePin((void *)GPIOE_BASE, 0x0020, 0);
    HAL_GPIO_WritePin((void *)GPIOB_BASE, 0x0200, 0);
    systick_delay(50);

    ble_payload = 1;
    (void)ssp_ble_enqueue_tx_packet(0x112, 1, &ble_payload, 0);
    while (ble_tx_count_free_slots() != 0x80) {
        ble_tx_pump();
        watchdog_kick();
    }

    modbus_payload = 0x0001;
    maybe_enqueue_tx_message(0x14, 2, &modbus_payload, 0);
    while (modbus_tx_count_free_slots() != 0x10) {
        modbus_tx_pump();
        watchdog_kick();
    }

    led_driver_enter_shipping_mode();
    stc_gas_gauge_set_run();
    wwdg_apb_clk_disable();
    systick_delay(1000);
    shipping_powerdown_deinit(6);   /* does not return */
}

/* ===================================================================
 * Console status / log / mode handlers (batch 9). HAL read helper + the
 * still-undecoded subsystem workers are kept as externs by OEM address. */
extern int  HAL_GPIO_ReadPin(void *GPIOx, uint16_t pin_mask);     /* 0x08026AB8 */

/* `battery` — refresh the BMS telemetry shadow (ctx+0x3F2, 300 B) + sentinel
 * ctx+0x3FC=0xFFFF, issue two BMS Modbus reads (slave 0xAA func 3, regs
 * 0x00..0x30 and 0x47..0x56), and arm a one-shot 1 s scheduler task that dumps
 * the populated telemetry to the console (OEM 0x08042F28). */
extern void battery_request_telemetry(void);   /* 0x0803EA74 — the two BMS reads */

/* The one-shot 1 s scheduler callback armed by console_cmd_battery (OEM
 * 0x0804175C). Releases its own scheduler slot, then prints the BMS telemetry
 * shadow battery_request_telemetry() populated. All fields live in the session
 * context (CTXB == g_app_state.ctx_sub) at byte offsets +0x3F2..+0x49E.
 *
 * Field encodings (from the disassembly):
 *   - the plain "%d" cells are sign-extended s16 (ldrsh);
 *   - the three temperatures (+0x43C/+0x43E/+0x440) are s16 minus 0xAAB: the BMS
 *     reports tenths of a Kelvin, so -2731 maps 0.1 K -> 0.1 degC;
 *   - "I"/+0x3FE is scaled x10;
 *   - HW_VER/+0x406 and FW_VER/+0x408 print as two args (hi = u16>>8, lo = u16&0xFF);
 *   - the raw-hex cells (+0x3F6 FAULT, +0x3FA UBAT, +0x452 FSR) are zero-extended u16.
 * The PDOCP/+0x498 and PDSCP/+0x49A format strings live in a separate rodata
 * island (0x080507C8/0x080507D4), not the contiguous 0x080542DC block. */
void console_battery_dump(void)
{
    uint8_t *ctx;

    scheduler_release((uint8_t *)SCHED_SLOT_TELEMETRY);
    ctx = CTXB;

    g_log_func("BAT_ID  0x%X\r\n", (int)*(short *)(ctx + 0x3F2));
    g_log_func("FAULT   0x%X\r\n", *(uint16_t *)(ctx + 0x3F6));
    g_log_func("BAT_TP1 %d\r\n",   *(short *)(ctx + 0x43C) - 0xAAB);
    g_log_func("BAT_TP2 %d\r\n",   *(short *)(ctx + 0x43E) - 0xAAB);
    g_log_func("MOS_TMP %d\r\n",   *(short *)(ctx + 0x440) - 0xAAB);
    g_log_func("UBAT    %d\r\n",   *(uint16_t *)(ctx + 0x3FA));
    g_log_func("RSOC    %d\r\n",   (int)*(short *)(ctx + 0x3FC));
    g_log_func("I       %d\r\n",   *(short *)(ctx + 0x3FE) * 10);
    g_log_func("CHARGE  %d\r\n",   (int)*(short *)(ctx + 0x400));
    g_log_func("DSG_PRT %d\r\n",   (int)*(short *)(ctx + 0x402));
    g_log_func("TST_DBG %d\r\n",   (int)*(short *)(ctx + 0x404));
    {
        uint16_t hw = *(uint16_t *)(ctx + 0x406);
        g_log_func("HW_VER  %X.%02X\r\n", hw >> 8, (char)hw);
    }
    {
        uint16_t fw = *(uint16_t *)(ctx + 0x408);
        g_log_func("FW_VER  %X.%02X\r\n", fw >> 8, (char)fw);
    }
    g_log_func("DATE_1  %d\r\n",   (int)*(short *)(ctx + 0x418));
    g_log_func("DATE_2  %d\r\n",   (int)*(short *)(ctx + 0x41A));
    g_log_func("NOM_CAP %d\r\n",   (int)*(short *)(ctx + 0x41C));
    g_log_func("FUL_CAP %d\r\n",   (int)*(short *)(ctx + 0x41E));
    g_log_func("REM_CAP %d\r\n",   (int)*(short *)(ctx + 0x420));
    g_log_func("ABS_SOC %d\r\n",   (int)*(short *)(ctx + 0x422));
    g_log_func("CYL_CNT %d\r\n",   (int)*(short *)(ctx + 0x424));
    g_log_func("CHG_ON  %d\r\n",   (int)*(short *)(ctx + 0x426));
    g_log_func("U_1 %d\r\n",       (int)*(short *)(ctx + 0x428));
    g_log_func("U_2 %d\r\n",       (int)*(short *)(ctx + 0x42A));
    g_log_func("U_3 %d\r\n",       (int)*(short *)(ctx + 0x42C));
    g_log_func("U_4 %d\r\n",       (int)*(short *)(ctx + 0x42E));
    g_log_func("U_5 %d\r\n",       (int)*(short *)(ctx + 0x430));
    g_log_func("U_6 %d\r\n",       (int)*(short *)(ctx + 0x432));
    g_log_func("U_7 %d\r\n",       (int)*(short *)(ctx + 0x434));
    g_log_func("U_8 %d\r\n",       (int)*(short *)(ctx + 0x436));
    g_log_func("U_9 %d\r\n",       (int)*(short *)(ctx + 0x438));
    g_log_func("U_A %d\r\n",       (int)*(short *)(ctx + 0x43A));
    g_log_func("U_MAX %d\r\n",     (int)*(short *)(ctx + 0x444));
    g_log_func("U_MIN %d\r\n",     (int)*(short *)(ctx + 0x446));
    g_log_func("FSR   0x%04X\r\n", *(uint16_t *)(ctx + 0x452));
    g_log_func("DOTP  %d\r\n",     (int)*(short *)(ctx + 0x480));
    g_log_func("DUTP  %d\r\n",     (int)*(short *)(ctx + 0x482));
    g_log_func("COTP  %d\r\n",     (int)*(short *)(ctx + 0x484));
    g_log_func("CUTP  %d\r\n",     (int)*(short *)(ctx + 0x486));
    g_log_func("DOCP1 %d\r\n",     (int)*(short *)(ctx + 0x488));
    g_log_func("DOCP2 %d\r\n",     (int)*(short *)(ctx + 0x48A));
    g_log_func("COCP1 %d\r\n",     (int)*(short *)(ctx + 0x48C));
    g_log_func("COCP2 %d\r\n",     (int)*(short *)(ctx + 0x48E));
    g_log_func("OVP1  %d\r\n",     (int)*(short *)(ctx + 0x490));
    g_log_func("OVP2  %d\r\n",     (int)*(short *)(ctx + 0x492));
    g_log_func("UVP1  %d\r\n",     (int)*(short *)(ctx + 0x494));
    g_log_func("UVP2  %d\r\n",     (int)*(short *)(ctx + 0x496));
    g_log_func("PDOCP %d\r\n",     (int)*(short *)(ctx + 0x498));
    g_log_func("PDSCP %d\r\n",     (int)*(short *)(ctx + 0x49A));
    g_log_func("MOTP  %d\r\n",     (int)*(short *)(ctx + 0x49C));
    g_log_func("SCP   %d\r\n",     (int)*(short *)(ctx + 0x49E));
}

void console_cmd_battery(char *args)
{
    uint8_t *ctx = CTXB;
    (void)args;

    memset(ctx + 0x3F2, 0, 300);                    /* wipe BMS telemetry shadow */
    *(uint16_t *)(ctx + 0x3FC) = 0xFFFF;            /* mark invalid until BMS replies */
    battery_request_telemetry();

    if (*(uint8_t *)SCHED_SLOT_TELEMETRY == SCHED_SLOT_NONE) {   /* 0xFA = no task yet */
        uint8_t slot = scheduler_alloc();
        *(uint8_t *)SCHED_SLOT_TELEMETRY = slot;
        scheduler_start(slot, 1000, console_battery_dump);
    }
}

/* `shifterstatus` — report the eShifter 24 V rail (GPIOB pin14), clear the
 * shifter scratch buffer (ctx+0x51E, 300 B), and (per HW version word at
 * ctx+CTX_SHIFTER_HW) arm a status-poll scheduler task (OEM 0x08042F74). */
extern void shifter_sm_set_step_3(void);   /* arm/refresh a related timer field (=3) */

void console_cmd_shifterstatus(char *args)
{
    uint8_t *ctx = CTXB;
    (void)args;

    short hw_ver = *(short *)(ctx + CTX_SHIFTER_HW);
    memset(ctx + 0x51E, 0, 300);

    g_log_func("Power is: %s\r\n",
               HAL_GPIO_ReadPin((void *)GPIOB_BASE, 0x4000) ? "ON" : "OFF");

    uint8_t *slot_rec = (uint8_t *)SCHED_SLOT_REC;   /* this cmd's slot id at +1 */
    scheduler_release(&slot_rec[1]);
    shifter_sm_set_step_3();
    g_log_func("Wait 3 seconds..\r\n");

    if (slot_rec[1] == SCHED_SLOT_NONE) {
        uint8_t slot = scheduler_alloc();
        slot_rec[1] = slot;
        if (hw_ver == 0x200) {
            scheduler_start(slot, 3000, (sched_cb_t)0x080410C5u);
        } else if (hw_ver == 0x201) {
            scheduler_start(slot, 3500, (sched_cb_t)0x08040EEDu);
        } else {
            g_log_func("No shifter found\r\n");
        }
    }
}

/* The one-shot 500 ms scheduler callback armed by console_cmd_motorstatus (OEM
 * 0x08040DE0; the timer is named "motor_get_tmr"). Releases the motor-poll
 * scheduler slot, then prints the motor-controller telemetry the four Modbus
 * requests populated. Fields live in the session context (CTXB) at byte offsets
 * +0x364..+0x388. If the drive-temp cell (+0x36C) is still zero the motor never
 * answered, so it logs "Motor no resp".
 *
 * The poll slot byte is SCHED_SLOT_REC (0x2000010E), NOT the battery dump's
 * SCHED_SLOT_TELEMETRY (0x20000110) — verified against [0x08040EB0] in the OEM
 * literal pool.
 *
 * Field encodings (from the disassembly):
 *   - +0x364/+0x366/+0x368/+0x370/+0x376/+0x36E/+0x372/+0x374 are zero-extended
 *     u16 (ldrh); the printf widths just differ (%04X vs %d);
 *   - m_tmp/+0x36A and d_tmp/+0x36C are sign-extended s16 (ldrsh);
 *   - fw/+0x388 is a packed u32 version printed as four octets
 *     major.minor.build.rev (major as %c). */
void motor_get_timer_cb(void)
{
    uint8_t *ctx;

    scheduler_release((uint8_t *)SCHED_SLOT_REC);
    ctx = CTXB;

    if (*(short *)(ctx + 0x36C) == 0) {
        g_log_func("Motor no resp\r\n");
        return;
    }

    g_log_func("error 0x%04X\r\n", *(uint16_t *)(ctx + 0x364));
    g_log_func("speed %d\r\n",     *(uint16_t *)(ctx + 0x366));
    g_log_func("I     %d\r\n",     *(uint16_t *)(ctx + 0x368));
    g_log_func("m_tmp %d\r\n",     (int)*(short *)(ctx + 0x36A));
    g_log_func("d_tmp %d\r\n",     (int)*(short *)(ctx + 0x36C));
    g_log_func("Ubat  %d\r\n",     *(uint16_t *)(ctx + 0x370));
    {
        uint32_t fw = *(uint32_t *)(ctx + 0x388);
        g_log_func("fw    %c.%d.%02d.%02d\r\n",
                   fw >> 24, (fw >> 16) & 0xFF, (fw >> 8) & 0xFF, fw & 0xFF);
    }
    g_log_func("io    %04X\r\n",   *(uint16_t *)(ctx + 0x376));
    g_log_func("wlsp  %d\r\n",     *(uint16_t *)(ctx + 0x36E));
    g_log_func("Pdsp  %d\r\n",     *(uint16_t *)(ctx + 0x372));
    g_log_func("Pdtrq %d\r\n",     *(uint16_t *)(ctx + 0x374));
}

/* `motorstatus` — arm a 500 ms "motor_get_tmr" poll task, clear the motor-status
 * accumulators (ctx+0x364..0x376), and enqueue four motor Modbus requests
 * (cmd 0x0C/0x0A/0x0B/0x0D) (OEM 0x08042E54). */
void console_cmd_motorstatus(char *args)
{
    uint8_t *slot = (uint8_t *)SCHED_SLOT_REC;   /* OEM 0x08042F08 = 0x2000010E */
    (void)args;

    if (*slot == SCHED_SLOT_NONE) {
        uint8_t s = scheduler_alloc();
        *slot = s;
        scheduler_set_timer_name(s, 500, "motor_get_tmr");
    }

    uint8_t *ctx = CTXB;
    *(uint32_t *)(ctx + 0x364) = 0;
    *(uint32_t *)(ctx + 0x368) = 0;
    *(uint32_t *)(ctx + 0x36C) = 0;
    *(uint16_t *)(ctx + 0x370) = 0;
    *(uint16_t *)(ctx + 0x376) = 0;

    scheduler_start(*slot, 500, motor_get_timer_cb);
    g_log_func("Send request..\r\n");

    if (maybe_enqueue_tx_message(0x0C, 0, 0, 1) > 0x10) g_log_func("  ERROR SSP place\r\n");
    if (maybe_enqueue_tx_message(0x0A, 0, 0, 1) > 0x10) g_log_func("  ERROR SSP place\r\n");
    if (maybe_enqueue_tx_message(0x0B, 0, 0, 1) > 0x10) g_log_func("  ERROR SSP place\r\n");
    if (maybe_enqueue_tx_message(0x0D, 0, 0, 1) > 0x10) g_log_func("  ERROR SSPB place\r\n");
}

/* `adc` — print the HW-version index and the scaled ADC rail voltages: Vbat
 * (supply_voltage_read), Vgsm, and 5Vsw (only on HW index > 5). ADC sample
 * buffer base 0x20000914; each reader scales raw*3300/4096 mV (OEM 0x08043028). */
extern int      hw_version_lookup(uint8_t *out_idx);  /* 0x08032CE4 */
extern uint32_t adc_read_vgsm(void);                  /* 0x08032DA4 */
extern uint32_t adc_read_5vsw(void);                  /* 0x08032DC0 */

void console_cmd_adc(char *args)
{
    uint8_t hw_idx[5];
    (void)args;

    if (hw_version_lookup(hw_idx) == 0) {
        g_log_func("  ERR HWversion\r\n");
    } else {
        g_log_func("HW version %d\r\n", hw_idx[0]);
    }
    g_log_func("Vbat %d mV\r\n", supply_voltage_read());
    g_log_func("Vgsm %d mV\r\n", adc_read_vgsm());
    if (hw_idx[0] > 5) {
        g_log_func("5Vsw %d mV\r\n", adc_read_5vsw());
    }
}

/* `stc` — read the battery STC fuel-gauge and print V/I/temp/percent
 * ("%d mV, %d mA %d%dC %d.%d%%") (OEM 0x08041614). */
extern uint32_t stc_read(void *ctx, uint32_t *out);   /* STC read; 1 = valid */

void console_cmd_stc(char *args)
{
    uint8_t  ctx[52];
    uint32_t out[12];
    (void)args;

    g_log_func("Read STC\r\n");
    if (stc_read(ctx, out) == 1) {
        int pct = (int)out[2], voltage = (int)out[3], current = (int)out[4], temp = (int)out[5];
        int temp_tens = temp / 10, pct_int = pct / 10;
        g_log_func("%d mV, %d mA %d%dC %d.%d%%\r\n",
                   voltage, current, temp_tens, temp - temp_tens * 10,
                   pct_int, pct - pct_int * 10);
    } else {
        g_log_func(" ERR Read STC\r\n");
    }
}

/* `batware` — start a batteryware (battery-module) firmware update: request bike
 * state 0x19 (if unlocked), log, and arm the update mode byte (OEM 0x08041E50). */
extern void batteryware_update_arm(void);   /* 0x0803F450 — sets *(0x20008A00+6)=4 */

void console_cmd_batware(char *args)
{
    (void)args;
    maybe_set_state_if_unlocked(0x19);
    g_log_func("Start batteryware update\r\n");
    batteryware_update_arm();
}

/* `logprn` — dump the whole circular log buffer between "LOG:"/"<EOF>" markers,
 * suppressing live logging during the dump via the state flag (OEM 0x08041F88).
 * log_buffer_dump is declared in log.h. */
void console_cmd_logprn(char *args)
{
    (void)args;
    state_flag_set(0);              /* suppress live logging (flag @ 0x20000083) */
    g_log_func("LOG:\r\n");
    log_buffer_dump();
    g_log_func("<EOF>\r\n");
    state_flag_set(1);             /* resume */
}

/* `logclr` — clear the circular log buffer, but only when given the confirmation
 * key `6` (guard against accidental wipes) (OEM 0x08041F34). */
void console_cmd_logclr(char *args)
{
    char *p = args;
    if (console_next_token(&p) == 0) {
        g_log_func("Clear: logclr key\r\n");
        return;
    }
    if ((strtol(p, NULL, 10) & 0xFFFF) == 6) {
        g_log_func("Clear log\r\n");
        log_buffer_reset();
    } else {
        g_log_func("Clear: wrong key\r\n");
    }
}

/* `factory` — load factory defaults: reset the settings/config object
 * (OEM 0x08041E70). */
extern void settings_factory_reset(void *cfg, int mode);   /* 0x0803FAD8 */

void console_cmd_factory(char *args)
{
    (void)args;
    g_log_func("Load factory defaults\r\n");
    settings_factory_reset((void *)CTXB, 1);
}

/* `reboot` — defer a restart so the console reply can flush, then run the
 * restart task after 600 ticks (OEM 0x08041DA4). */
extern void reboot_restart_task(void);   /* 0x08038A68 */

void console_cmd_reboot(char *args)
{
    (void)args;
    scheduler_start(scheduler_alloc(), 600, reboot_restart_task);
}

/* `sound` — play a sound: parse a decimal channel/sample index and trigger the
 * channel notify (OEM 0x08041D10). */
void console_cmd_sound(char *args)
{
    char *p = args;
    if (console_next_token(&p) != 0) {
        uint32_t v = (uint32_t)strtol(p, NULL, 10);
        channel_notify_with_status(v & 0xFF);
    }
}

/* `bwritedata` — inject a Modbus "write multiple registers" (func 0x10) to the
 * battery (slave 0xAA): bwritedata [register] [data] [length] (OEM 0x08041BB4). */
void console_cmd_bwritedata(char *args)
{
    char *p = args;
    if (console_next_token(&p) == 0) {
        g_log_func("usage: writedata [register] [data] [length]\r\n");
        return;
    }
    uint32_t reg    = (uint32_t)strtol(p, NULL, 10);
    uint32_t data   = (console_next_token(&p) == 0) ? 0
                      : ((uint32_t)strtol(p, NULL, 10) & 0xFFFF);
    uint32_t length = (console_next_token(&p) == 0) ? 0
                      : ((uint32_t)strtol(p, NULL, 10) & 0xFFFF);
    g_log_func("Register %d data %d length %d\r\n", reg & 0xFFFF, data, length);

    uint8_t frame[0x86];
    frame[0] = MB_SLAVE_BAT;                            /* battery slave */
    frame[1] = MB_FUNC_WRITE_MULTI;                            /* write multiple registers */
    *(uint16_t *)&frame[2] = (uint16_t)reg;
    frame[MB_FRAME_COUNT_OFF] = (uint8_t)(length << 1);
    memset(&frame[4], 0, 0x80);
    for (uint32_t i = 0; i < length * 2u; i += 2) {
        frame[4 + i]     = (uint8_t)(data >> 8);
        frame[4 + i + 1] = (uint8_t)data;
    }
    if (modbus_bat_submit(frame) != 0) {
        g_log_func("  Error\r\n");
    }
}

/* `swritereg` — inject a Modbus "write single register" (func 0x06) to the
 * shifter (slave 0x20): swritereg [register] [data] (OEM 0x08041528). */
void console_cmd_swritereg(char *args)
{
    char *p = args;
    if (console_next_token(&p) == 0) {
        g_log_func("usage: writereg [register] [data]\r\n ");
        return;
    }
    uint32_t reg  = (uint32_t)strtol(p, NULL, 10);
    uint32_t data = (console_next_token(&p) == 0) ? 0
                    : ((uint32_t)strtol(p, NULL, 10) & 0xFFFF);
    g_log_func("Register %d data %d\r\n", reg & 0xFFFF, data);

    uint8_t pdu[6];
    pdu[0] = MB_SLAVE_SHIFT;                              /* shifter slave */
    pdu[1] = MB_FUNC_WRITE_SINGLE;                              /* write single register */
    *(uint16_t *)&pdu[2] = (uint16_t)reg;
    pdu[4] = (uint8_t)data;
    pdu[5] = (uint8_t)(data >> 8);
    if (modbus_shift_submit(pdu) != 0) {
        g_log_func("  Error\r\n");
    }
}

/* ===================================================================
 * Remaining console handlers (batch 10) — completes the 49-command surface.
 * ctx = *(uint8_t**)(0x20009368+0x2DC) (session_ctx). */

/* The console command dispatch table (OEM @ flash 0x0804F5C4): 49 entries. */
typedef struct {
    const char *name;
    const char *help;
    void      (*handler)(char *args);
} console_cmd_t;
#define G_CONSOLE_CMD_TABLE  ((const console_cmd_t *)0x0804F5C4u)
#define CONSOLE_CMD_COUNT    49u

/* `help` — list every command name + one-line help from the dispatch table. */
void console_cmd_help(char *args)
{
    (void)args;
    g_log_func("Available commands:\r\n");
    for (uint8_t i = 0; i < CONSOLE_CMD_COUNT; i++) {
        g_log_func("%-15s   %s\r\n", G_CONSOLE_CMD_TABLE[i].name, G_CONSOLE_CMD_TABLE[i].help);
    }
}

/* `logout` — clear the console session authenticated flag (ctx_sub +0x2D9 =
 * logged_in) directly at the app-context base (OEM 0x08040A4C). */
void console_cmd_logout(char *args)
{
    (void)args;
    *(volatile uint8_t *)(APP_CTX_BASE + CTX_LOGGED_IN) = 0;   /* g_app_state.ctx_sub->logged_in */
}

/* `blereset` — pulse the BLE module reset line (GPIOE PE5) high 10 ms then low
 * (OEM 0x08041FB8). */
void console_cmd_blereset(char *args)
{
    (void)args;
    HAL_GPIO_WritePin((void *)GPIOE_BASE, 0x20, 1);   /* PE5 high (assert) */
    systick_delay(10);
    HAL_GPIO_WritePin((void *)GPIOE_BASE, 0x20, 0);   /* PE5 low (release) */
}

/* `bledebug` — open the CC2642 BLE-chip sub-shell: log "Connect to UART7" and
 * arm the console UART-redirect selector (ctx+CTX_REDIRECT_BLE = 1) (OEM 0x08040C6C). */
void console_cmd_bledebug(char *args)
{
    (void)args;
    g_log_func("Connect to UART7\r\n");
    CTXB[CTX_REDIRECT_BLE] = 1;
}

/* `loop` — print super-loop timing (period/peak us + ms field) and reset the
 * ms + peak accumulators (ctx+CTX_LOOP_LAST_US/0x35C/0x360) (OEM 0x08040C28). */
void console_cmd_loop(char *args)
{
    uint8_t *ctx = CTXB;
    (void)args;
    g_log_func("actual %d us (%d us - %d ms)\r\n",
               1000u / *(uint32_t *)(ctx + CTX_LOOP_LAST_US),
               1000u / *(uint32_t *)(ctx + CTX_LOOP_PEAK_US),
               *(uint32_t *)(ctx + CTX_LOOP_MS));
    ctx = CTXB;
    *(uint32_t *)(ctx + CTX_LOOP_MS) = 0;
    *(uint32_t *)(ctx + CTX_LOOP_PEAK_US) = 0;
}

/* `logapp` — set the "log to APP vs Serial" flag (ctx+CTX_LOG_TO_APP) from the arg,
 * persist the state record to EEPROM, and echo (OEM 0x08041E94). */
extern int  save_state_record_to_eeprom(uint32_t f0, uint32_t f1, uint32_t f2, uint32_t f3,
                                         uint32_t f4, uint32_t f5, uint32_t f6, uint32_t f7,
                                         uint32_t f8, uint32_t f9, uint32_t f10, uint32_t f11,
                                         uint32_t f12, uint32_t f13, uint32_t f14); /* 0x0803E2CC */
/* app_log_sink_enable is declared in log.h. */

void console_cmd_logapp(char *args)
{
    char *p = args;
    if (console_next_token(&p) != 0) {
        uint8_t *ctx = CTXB;
        ctx[CTX_LOG_TO_APP] = ((uint32_t)strtol(p, NULL, 10) & 0xFFFF) != 0;
        if (save_state_record_to_eeprom(
                *(uint32_t *)(ctx + 0x310), *(uint32_t *)(ctx + 0x314), *(uint32_t *)(ctx + 0x318),
                *(uint32_t *)(ctx + CTX_DISTANCE), *(uint32_t *)(ctx + 0x320), *(uint32_t *)(ctx + 0x324),
                *(uint32_t *)(ctx + 0x328), *(uint32_t *)(ctx + 0x32C), *(uint32_t *)(ctx + 0x330),
                *(uint32_t *)(ctx + 0x334), *(uint32_t *)(ctx + CTX_SHIFT_COUNTER), *(uint32_t *)(ctx + 0x33C),
                *(uint32_t *)(ctx + 0x340), *(uint32_t *)(ctx + 0x344), *(uint32_t *)(ctx + 0x348)) != 0) {
            g_log_func(" ERROR Save values\r\n");
        }
        if (ctx[CTX_LOG_TO_APP]) {
            app_log_sink_enable();
        }
    }
    g_log_func("Logging %s\r\n",
               (CTXB)[0x313] ? "APP" : "Serial");
}

/* `powerchange` — set/report the "ride change" enable flag (ctx+CTX_RIDE_CHANGE)
 * (OEM 0x080412BC). */
void console_cmd_powerchange(char *args)
{
    char *p = args;
    if (console_next_token(&p) != 0) {
        (CTXB)[0x3CB] = (strtol(p, NULL, 10) != 0);
    }
    g_log_func("ride change: %s\r\n",
               (CTXB)[0x3CB] ? "Yes" : "No");
}

/* `batreset` — pulse the BMS hardware reset (GPIOB PB5); a 5 s scheduled task
 * then releases the line + frees its slot (OEM 0x08041DD8). Slot byte @ 0x20000112. */
static void bat_reset_release_cb(void)
{
    scheduler_release((uint8_t *)SCHED_SLOT_BATRESET);          /* free this one-shot slot */
    g_log_func("BMS release reset\r\n");
    HAL_GPIO_WritePin((void *)GPIOB_BASE, 0x20, 0);    /* PB5 low (deassert) */
}

void console_cmd_batreset(char *args)
{
    uint8_t *slot = (uint8_t *)SCHED_SLOT_BATRESET;
    (void)args;
    g_log_func("BMS reset\r\n");
    HAL_GPIO_WritePin((void *)GPIOB_BASE, 0x20, 1);    /* PB5 high (assert) */
    if (*slot == SCHED_SLOT_NONE) {
        *slot = scheduler_alloc();
    }
    scheduler_start(*slot, 5000, bat_reset_release_cb);
}

/* `shiftware` — start a shiftware (eShifter) firmware update: log + arm the
 * console redirect selector (global 0x2000019C = 2) (OEM 0x08041DBC). */
void console_cmd_shiftware(char *args)
{
    (void)args;
    g_log_func("Start shiftware update\r\n");
    *(volatile uint8_t *)CONSOLE_REDIRECT_SEL = 2;   /* console redirect: shiftware */
}

/* The shifter-debug pump task (OEM 0x08041FDC): kicks the watchdog and re-arms
 * itself every 0x50 ticks while shifter-debug streaming is on. */
void shiftdebug_pump_task(void)
{
    watchdog_kick();
    scheduler_start(*(volatile uint8_t *)SCHED_SLOT_SHIFTDEBUG, 0x50,
                    (sched_cb_t)shiftdebug_pump_task);
}

/* `shiftdebug` — toggle the "stream Modbus-shifter debug" flag (CTX_DBG_SHIFTER)
 * and arm an 0x50-tick pump task (OEM 0x08041D50). */
void console_cmd_shiftdebug(char *args)
{
    (void)args;
    *(volatile uint8_t *)SCHED_SLOT_SHIFTDEBUG = scheduler_alloc();
    scheduler_start(*(volatile uint8_t *)SCHED_SLOT_SHIFTDEBUG, 0x50, (sched_cb_t)shiftdebug_pump_task);
    uint8_t *ctx = CTXB;
    ctx[CTX_DBG_SHIFTER] ^= 1;
    g_log_func("Show Modbus shifter %s\r\n", ctx[CTX_DBG_SHIFTER] ? "ON" : "OFF");
}

/* `shiftresetcounter` — zero the eShifter operation counter (ctx+CTX_SHIFT_COUNTER)
 * (OEM 0x08040CB4). */
void console_cmd_shiftresetcounter(char *args)
{
    (void)args;
    *(uint32_t *)(CTXB + CTX_SHIFT_COUNTER) = 0;
    g_log_func("Shifter counter reset\r\n");
}

/* `gsmdebug` — open the u-blox modem AT sub-shell: log "Connect to UART2" and
 * set the GSM redirect-enable flag (ctx+CTX_REDIRECT_GSM = 1) (OEM 0x08040C90). */
void console_cmd_gsmdebug(char *args)
{
    (void)args;
    g_log_func("Connect to UART2\r\n");
    CTXB[CTX_REDIRECT_GSM] = 1;
}

/* `bmsdebug` — toggle the Modbus-BMS debug-print flag (ctx+CTX_DBG_BMS) (OEM 0x08040CD8). */
void console_cmd_bmsdebug(char *args)
{
    uint8_t *ctx = CTXB;
    (void)args;
    ctx[CTX_DBG_BMS] ^= 1;
    g_log_func("Show Modbus BMS %s\r\n", ctx[CTX_DBG_BMS] ? "ON" : "OFF");
}

/* `stcreset` — reset the battery STC gas-gauge / coulomb counter (OEM 0x080415EC). */
extern int gas_gauge_reset(void);   /* 0x080396C4 */

void console_cmd_stcreset(char *args)
{
    (void)args;
    if (gas_gauge_reset() == 0) {
        g_log_func("GasGauge_Reset\r\n");
    } else {
        g_log_func("ERROR GasGauge_Reset\r\n");
    }
}

/* `setoad` — request OAD (over-the-air firmware-update) mode: bike state 0x19
 * (if unlocked), then log (OEM 0x080415B4). */
void console_cmd_setoad(char *args)
{
    (void)args;
    maybe_set_state_if_unlocked(0x19);
    g_log_func("Set OAD mode\r\n");
}

/* `setgear` — store a gear-encoder position to the MT shifter via a Modbus
 * write-multiple (func 0x10): setgear <gear> <position>. Requires the MT
 * shifter present (ctx+CTX_SHIFTER_HW == 0x201) and gear in 1..ctx+CTX_GEAR_COUNT (OEM 0x080413B4). */
void console_cmd_setgear(char *args)
{
    char    *p   = args;
    uint8_t *ctx = CTXB;

    if (*(short *)(ctx + CTX_SHIFTER_HW) != 0x201) {
        g_log_func("MT shifter not detected\r\n");
        return;
    }
    if (console_next_token(&p) == 0) {
        g_log_func("use 'shifterstatus' to get positions\r\n");
        return;
    }
    uint32_t gear = (uint32_t)strtol(p, NULL, 10) & 0xFF;
    if (gear == 0 || (int)*(short *)(ctx + CTX_GEAR_COUNT) < (int)gear) {
        g_log_func("invalid gear number\r\n");
        return;
    }
    if (console_next_token(&p) == 0) {
        g_log_func("Enter position\r\n");
        return;
    }
    long pos = strtol(p, NULL, 10);
    g_log_func("Save gear %d position %d\r\n", gear, pos);

    uint8_t frame[0x86];
    frame[0] = MB_SLAVE_SHIFT;                                   /* shifter slave */
    frame[1] = MB_FUNC_WRITE_MULTI;                                   /* write multiple registers */
    *(short *)&frame[2] = (short)((gear + 0x1F) * 2);  /* register address */
    frame[4] = (uint8_t)((uint32_t)pos >> 24);         /* position, big-endian u32 */
    frame[5] = (uint8_t)((uint32_t)pos >> 16);
    frame[6] = (uint8_t)((uint32_t)pos >> 8);
    frame[7] = (uint8_t)pos;
    frame[MB_FRAME_COUNT_OFF] = 4;                                   /* byte count */
    if (modbus_shift_submit(frame) != 0) {
        g_log_func("  Error\r\n");
    }
}

/* ===========================================================================
 *  UART7 — the ES3 debug-console byte transport
 *
 *  The console prompt lives on UART7. Its register-block handle is a pointer-to-
 *  pointer at 0x200097E4 (set by uart7_init); the TX ring handle is at ctx+0x164
 *  and the RX ring at +0x168 (ctx block 0x20003C34, shared with UART8). The five
 *  functions below are installed into the g_log_func dispatch table by
 *  console_io_table_install: [0]=console_printf, [1]=uart7_tx_byte,
 *  [2]=uart7_puts, [3]=uart7_write, [4]=uart_rx_ringbuf_get_byte — so
 *  console_printf is g_log_func[0], the system-wide printf.
 * ======================================================================== */

#define UART7_HANDLE   (*(volatile uint32_t * volatile *)0x200097E4u)
#define UART7_CTX      0x20003C34u
#define UART7_TX_RING  (*(ringbuf_t * volatile *)(UART7_CTX + 0x164u))
#define UART7_RX_RING  (*(ringbuf_t * volatile *)(UART7_CTX + 0x168u))

/* Console line-discipline state block (OEM 0x20004D2C): +0x82 command-mode flag,
 * +0x83 consecutive-ESC counter, +0x84 consecutive-TAB counter. */
#define CONSOLE_STATE     ((volatile uint8_t *)0x20004D2Cu)
/* When non-zero, console_printf also mirrors each line into the SRAM log with an
 * epoch-seconds prefix (OEM 0x20000083). */
#define CONSOLE_LOG_ECHO  (*(volatile uint8_t *)0x20000083u)

/* libc formatting (no stdio.h in the build — declared like modem.c's snprintf). */
extern int vsnprintf(char *s, unsigned int n, const char *fmt, va_list ap);
extern int snprintf(char *s, unsigned int n, const char *fmt, ...);

/* Cross-module leaves the escape handler invokes (OEM addresses noted). */
extern void system_reset(void);             /* 0x08035CE8 — AIRCR SYSRESETREQ, no return */
extern void console_io_table_install(void); /* 0x080430D8 — bind g_log_func to UART7 */

/* Locked single-byte TX into the UART7 TX ring (OEM 0x08036754). Returns the
 * ring-push status (ABI quirk: it survives in r0 through the TXEIE re-enable). */
int uart7_tx_byte(uint8_t b)
{
    volatile uint32_t *dev = UART7_HANDLE;

    dev[0xC / 4] &= ~0x80u;                        /* mask TXEIE */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    uint32_t rc = ringbuf_push_byte(UART7_TX_RING, b);

    dev = UART7_HANDLE;                            /* OEM re-loads the handle */
    dev[0xC / 4] |= 0x80u;                         /* re-enable TXEIE */
    return (int)rc;
}

/* Transmit a NUL-terminated string over UART7 (OEM 0x0803678C). */
void uart7_puts(const char *s)
{
    for (; *s != '\0'; s++) {
        uart7_tx_byte((uint8_t)*s);
    }
}

/* Transmit `len` raw bytes over UART7 (OEM 0x0803679E). The OEM masks the length
 * decrement to 16 bits. */
void uart7_write(const uint8_t *buf, uint16_t len)
{
    while (len != 0) {
        uart7_tx_byte(*buf++);
        len = (uint16_t)(len - 1);
    }
}

/* Locked single-byte RX from the UART7 RX ring (OEM 0x080367B8). Returns the
 * ring-get status (1 = byte dequeued into *out, 0 = ring empty). */
int uart_rx_ringbuf_get_byte(uint8_t *out)
{
    volatile uint32_t *dev = UART7_HANDLE;

    dev[0xC / 4] &= ~0x20u;                        /* mask RXNEIE */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    uint32_t rc = ringbuf_get_byte(UART7_RX_RING, out);

    dev = UART7_HANDLE;                            /* OEM re-loads the handle */
    dev[0xC / 4] |= 0x20u;                         /* re-enable RXNEIE */
    return (int)rc;
}

/* console_printf (OEM 0x080367F0) — installed as g_log_func[0], the firmware-wide
 * printf. Formats into a 256-byte stack buffer; if CONSOLE_LOG_ECHO is set, also
 * mirrors an "<epoch> " prefix and the message into the SRAM log. Then it pushes
 * the message out the UART7 TX ring synchronously: TXEIE is masked while bytes
 * are queued directly, and whenever the ring fills it re-enables TXEIE and busy-
 * waits (kicking the watchdog) until the ISR has fully drained the FIFO before
 * resuming; TXEIE is left enabled on exit so the tail is sent by the ISR. */
int console_printf(const char *fmt, ...)
{
    char    msg[256];
    va_list ap;
    int     len;
    int     i;

    va_start(ap, fmt);
    len = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    if (CONSOLE_LOG_ECHO) {
        char ts[25];
        snprintf(ts, sizeof ts, "%ld ", (long)rtc_now_epoch_seconds());
        log_emit_string(ts);
        log_emit_string(msg);
    }

    UART7_HANDLE[0xC / 4] &= ~0x80u;               /* mask TXEIE */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    for (i = 0; i < len && i <= 255 && msg[i] != '\0'; i++) {
        while (ringbuf_push_byte(UART7_TX_RING, (uint8_t)msg[i]) == 0) {
            /* ring full: let the ISR drain it completely, then retry the push */
            UART7_HANDLE[0xC / 4] |= 0x80u;
            do {
                watchdog_kick();
            } while (ringbuf_free_space(UART7_TX_RING) != 0x400u);
            UART7_HANDLE[0xC / 4] &= ~0x80u;
        }
    }

    UART7_HANDLE[0xC / 4] |= 0x80u;                /* re-enable so the ISR sends the tail */
    return len;
}

/* Clear a latched USART error flag the RM0430 way: if `flag` (PE 0x1 / FE 0x2 /
 * NE 0x4 / ORE 0x8) is set in SR, read SR then DR — that read sequence clears the
 * sticky error bit (and discards the offending byte). Reading DR is what actually
 * de-asserts ORE, so dropping it would wedge RX on an overrun. */
static void usart_clear_error_flag(volatile uint32_t *d, uint32_t flag)
{
    volatile uint32_t tmp;
    if ((d[0] & flag) != 0) {
        tmp = 0;
        tmp = d[0];   /* SR */
        tmp = d[1];   /* DR */
        (void)tmp;
    }
}

/* UART7 RX/TX byte-pump ISR (OEM 0x080368D4), invoked via a thin vector
 * trampoline. RX: on RXNE with no error and RXNEIE set, push DR into the RX ring,
 * then watch the byte for two command keys — ten consecutive ESC (0x1B) writes
 * the bootloader magic to SRAM 0, logs "NVICReset", and resets; ten consecutive
 * TAB (0x09) re-binds the console I/O and enters command mode. (DR is re-read for
 * each comparison exactly as the OEM does.) Then clear any latched PE/FE/NE/ORE
 * error; TX: on TXE with TXEIE set, pop the next byte to DR, disabling TXEIE when
 * the ring drains. The RX/TX gates use SR/CR1 sampled once. */
void uart7_irq_handler(void)
{
    volatile uint32_t *dev = UART7_HANDLE;
    uint32_t sr  = dev[0];
    uint32_t cr1 = dev[0xC / 4];

    if ((sr & 0xFu) == 0 && (sr & 0x20u) != 0 && (cr1 & 0x20u) != 0) {
        ringbuf_push_byte(UART7_RX_RING, (uint8_t)dev[1]);

        volatile uint32_t *rxdev = UART7_HANDLE;   /* OEM re-derefs the handle */
        if ((uint8_t)rxdev[1] == 0x1Bu) {          /* ESC */
            CONSOLE_STATE[0x83]++;
        } else {
            CONSOLE_STATE[0x83] = 0;
        }
        if ((uint8_t)rxdev[1] == 0x09u) {          /* TAB */
            CONSOLE_STATE[0x84]++;
        } else {
            CONSOLE_STATE[0x84] = 0;
        }

        if (CONSOLE_STATE[0x83] == 0x0Au) {        /* 10x ESC -> reset to bootloader */
            *(volatile uint32_t *)0x20000000u = 0x55AA5507u;
            g_log_func("NVICReset\r\n");
            systick_delay(10);
            system_reset();                        /* no return */
        } else if (CONSOLE_STATE[0x84] == 0x0Au) { /* 10x TAB -> command mode */
            CONSOLE_STATE[0x84] = 0;
            console_io_table_install();
            g_log_func("To Commandmode\r\n");
            CONSOLE_STATE[0x82] = 1;
        }
    }

    volatile uint32_t *d = UART7_HANDLE;
    usart_clear_error_flag(d, 0x1u);
    usart_clear_error_flag(d, 0x2u);
    usart_clear_error_flag(d, 0x4u);
    usart_clear_error_flag(d, 0x8u);

    if ((sr & 0x80u) != 0 && (cr1 & 0x80u) != 0) {
        uint8_t b;
        if (ringbuf_get_byte(UART7_TX_RING, &b) == 0) {
            UART7_HANDLE[0xC / 4] &= ~0x80u;
        } else {
            UART7_HANDLE[1] = b;
        }
    }
}

/* ===========================================================================
 *  USART1 — the second ES3 debug-console port (twin of the UART7 block above)
 *
 *  USART1 carries the same console: handle pp at 0x200098A4 (set by usart1_init),
 *   rings in the shared context 0x2000094C at TX `+0x04` / RX `+0x08`. It runs the
 *  same line discipline as UART7 and shares the command-mode flag (g_console_state
 *  +0x82) and the log-echo flag, but tracks its OWN consecutive-ESC/-TAB counters
 *  at g_console_state +0x80 / +0x81. The active console is whichever one's I/O
 *  table is bound into g_log_func: `usart1_io_table_install` selects USART1
 *  (selector 0x20000114 = 1), `console_io_table_install` selects UART7 (= 7); the
 *  command-mode bridge / escape handlers swap between them. The bootloader hand-off
 *  magic differs by port: USART1 writes 0x55AA5501, UART7 writes 0x55AA5507.
 * ======================================================================== */

#define USART1_HANDLE   (*(volatile uint32_t * volatile *)0x200098A4u)
#define USART1_CTX      0x2000094Cu
#define USART1_TX_RING  (*(ringbuf_t * volatile *)(USART1_CTX + 0x4u))
#define USART1_RX_RING  (*(ringbuf_t * volatile *)(USART1_CTX + 0x8u))

extern void usart1_io_table_install(void);  /* 0x0804309C — bind g_log_func to USART1 */

/* Locked single-byte TX into the USART1 TX ring (OEM 0x08035E28). */
int usart1_tx_byte(uint8_t b)
{
    volatile uint32_t *dev = USART1_HANDLE;

    dev[0xC / 4] &= ~0x80u;                        /* mask TXEIE */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    uint32_t rc = ringbuf_push_byte(USART1_TX_RING, b);

    dev = USART1_HANDLE;                           /* OEM re-loads the handle */
    dev[0xC / 4] |= 0x80u;                         /* re-enable TXEIE */
    return (int)rc;
}

/* Transmit a NUL-terminated string over USART1 (OEM 0x08035E5C). */
void usart1_puts(const char *s)
{
    for (; *s != '\0'; s++) {
        usart1_tx_byte((uint8_t)*s);
    }
}

/* Transmit `len` raw bytes over USART1 (OEM 0x08035E6E; len decrement masked to 16 bits). */
void usart1_write(const uint8_t *buf, uint16_t len)
{
    while (len != 0) {
        usart1_tx_byte(*buf++);
        len = (uint16_t)(len - 1);
    }
}

/* Locked single-byte RX from the USART1 RX ring (OEM 0x08035E88). */
int usart1_rx_byte(uint8_t *out)
{
    volatile uint32_t *dev = USART1_HANDLE;

    dev[0xC / 4] &= ~0x20u;                        /* mask RXNEIE */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    uint32_t rc = ringbuf_get_byte(USART1_RX_RING, out);

    dev = USART1_HANDLE;                           /* OEM re-loads the handle */
    dev[0xC / 4] |= 0x20u;                         /* re-enable RXNEIE */
    return (int)rc;
}

/* usart1_printf (OEM 0x08035EBC) — the USART1 twin of console_printf, installed as
 * g_log_func[0] while USART1 is the active console. Identical drain-buffered
 * synchronous write to the USART1 TX ring; same optional epoch-prefixed log echo. */
int usart1_printf(const char *fmt, ...)
{
    char    msg[256];
    va_list ap;
    int     len;
    int     i;

    va_start(ap, fmt);
    len = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    if (CONSOLE_LOG_ECHO) {
        char ts[25];
        snprintf(ts, sizeof ts, "%ld ", (long)rtc_now_epoch_seconds());
        log_emit_string(ts);
        log_emit_string(msg);
    }

    USART1_HANDLE[0xC / 4] &= ~0x80u;              /* mask TXEIE */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    for (i = 0; i < len && i <= 255 && msg[i] != '\0'; i++) {
        while (ringbuf_push_byte(USART1_TX_RING, (uint8_t)msg[i]) == 0) {
            USART1_HANDLE[0xC / 4] |= 0x80u;
            do {
                watchdog_kick();
            } while (ringbuf_free_space(USART1_TX_RING) != 0x400u);
            USART1_HANDLE[0xC / 4] &= ~0x80u;
        }
    }

    USART1_HANDLE[0xC / 4] |= 0x80u;               /* re-enable so the ISR sends the tail */
    return len;
}

/* USART1 RX/TX byte-pump ISR (OEM 0x08035F98) — twin of uart7_irq_handler. RX:
 * push DR into the RX ring, then run the command-key escape handler on the byte —
 * ten consecutive ESC (counter g_console_state+0x80) writes the USART1 boot magic
 * 0x55AA5501 to SRAM 0 and resets; ten consecutive TAB (+0x81) re-binds g_log_func
 * to USART1 and enters command mode (+0x82). Then clear any latched PE/FE/NE/ORE
 * error; TX: pop the next byte to DR, disabling TXEIE when the ring drains. */
void usart1_irq_handler(void)
{
    volatile uint32_t *dev = USART1_HANDLE;
    uint32_t sr  = dev[0];
    uint32_t cr1 = dev[0xC / 4];

    if ((sr & 0xFu) == 0 && (sr & 0x20u) != 0 && (cr1 & 0x20u) != 0) {
        ringbuf_push_byte(USART1_RX_RING, (uint8_t)dev[1]);

        volatile uint32_t *rxdev = USART1_HANDLE;  /* OEM re-derefs the handle */
        if ((uint8_t)rxdev[1] == 0x1Bu) {          /* ESC */
            CONSOLE_STATE[0x80]++;
        } else {
            CONSOLE_STATE[0x80] = 0;
        }
        if ((uint8_t)rxdev[1] == 0x09u) {          /* TAB */
            CONSOLE_STATE[0x81]++;
        } else {
            CONSOLE_STATE[0x81] = 0;
        }

        if (CONSOLE_STATE[0x80] == 0x0Au) {        /* 10x ESC -> reset to bootloader */
            *(volatile uint32_t *)0x20000000u = 0x55AA5501u;
            g_log_func("NVICReset\r\n");
            systick_delay(10);
            system_reset();                        /* no return */
        } else if (CONSOLE_STATE[0x81] == 0x0Au) { /* 10x TAB -> command mode */
            CONSOLE_STATE[0x81] = 0;
            usart1_io_table_install();
            g_log_func("To Commandmode\r\n");
            CONSOLE_STATE[0x82] = 1;
        }
    }

    volatile uint32_t *d = USART1_HANDLE;
    usart_clear_error_flag(d, 0x1u);
    usart_clear_error_flag(d, 0x2u);
    usart_clear_error_flag(d, 0x4u);
    usart_clear_error_flag(d, 0x8u);

    if ((sr & 0x80u) != 0 && (cr1 & 0x80u) != 0) {
        uint8_t b;
        if (ringbuf_get_byte(USART1_TX_RING, &b) == 0) {
            USART1_HANDLE[0xC / 4] &= ~0x80u;
        } else {
            USART1_HANDLE[1] = b;
        }
    }
}

/* Magic-key-sequence detector (OEM 0x08037188, a light_tick_update helper):
 * keeps a 5-entry rolling ring of recently received console bytes at SRAM
 * 0x20005E20 (count at [0], the bytes at [4..8]), reset whenever ESC arrives,
 * and reports a match once they spell the escape sequence "\x1b[14~" (the F4
 * key). The OEM materialises the pattern on the stack and memcmp's it against
 * the ring; reproduced faithfully. */
int console_magic_sequence_match(int key)
{
    static const uint8_t pattern[8] = { 0x1b, 0x5b, 0x31, 0x34, 0x7e, 0, 0, 0 };
    uint8_t *ring = (uint8_t *)0x20005e20u;
    uint8_t  local[8];
    uint8_t  count;

    memcpy(local, pattern, 8);
    if (key == 0x1b) {
        ring[0] = 0;
    }
    count = ring[0];
    ring[4 + count] = (uint8_t)key;
    count = (uint8_t)(count + 1);
    ring[0] = count;
    if (count == 5) {
        ring[0] = 0;
    }
    return memcmp(local, ring + 4, 5) == 0;
}

/* No-op console I/O vectors (OEM 0x08036B78 vararg-discard / 0x08036B74 / 0x08036B76,
 * each a `bx lr`-class stub): while a raw-UART passthrough owns the line, the
 * printf/puts/write slots are silenced so firmware log output cannot corrupt the
 * forwarded byte stream. */
static int  cmdmode_noop_printf(const char *fmt, ...) { (void)fmt; return 0; }
static void cmdmode_noop_puts(const char *s) { (void)s; }
static void cmdmode_noop_write(const uint8_t *buf, uint16_t len) { (void)buf; (void)len; }

/* Install the "silent" console I/O table for the command-mode passthrough: the
 * printf/puts/write slots become no-ops while tx_byte/rx_byte stay bound to the
 * active console port — UART7, or USART1 when `g_console_port_sel` (0x20000114) == 1.
 * `light_tick_update` calls this on entry to the `gsmdebug`/`bledebug` raw passthrough;
 * `console_io_table_install` restores the normal table on the F4-sequence exit.
 * OEM 0x08043148. */
typedef struct {
    int  (*printf)(const char *fmt, ...);   /* [0] */
    int  (*tx_byte)(uint8_t b);             /* [1] */
    void (*puts)(const char *s);            /* [2] */
    void (*write)(const uint8_t *buf, uint16_t len); /* [3] */
    int  (*rx_byte)(uint8_t *out);          /* [4] */
} cmdmode_io_t;

void console_passthrough_io_install(void)
{
    cmdmode_io_t *io = (cmdmode_io_t *)0x20009d98u;        /* g_log_func */

    if (*(volatile uint8_t *)0x20000114u != 1) {          /* UART7 console active */
        io->printf  = cmdmode_noop_printf;
        io->tx_byte = uart7_tx_byte;
        io->puts    = cmdmode_noop_puts;
        io->write   = cmdmode_noop_write;
        io->rx_byte = uart_rx_ringbuf_get_byte;
        return;
    }
    io->printf  = cmdmode_noop_printf;                     /* USART1 (2nd port) active */
    io->tx_byte = usart1_tx_byte;
    io->puts    = cmdmode_noop_puts;
    io->write   = cmdmode_noop_write;
    io->rx_byte = usart1_rx_byte;
}
