#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "app_state.h"
#include "audio.h"
#include "console.h"
#include "log.h"
#include "scheduler.h"
#include "ssp.h"
#include "stm32f413_gpio.h"
#include "systick.h"
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
extern uint32_t FUN_08031728(uint32_t a, uint32_t b, uint32_t c, uint32_t d);

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
        uint32_t res = FUN_08031728(
            g_app_state.ctx_sub->audio_engine_cfg[0],
            g_app_state.ctx_sub->audio_engine_cfg[1],
            g_app_state.ctx_sub->audio_engine_cfg[2],
            g_app_state.ctx_sub->audio_engine_cfg[3]);
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

    (void)snapshot;  /* keep snapshot live across the apply call */
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
 * (FUN_08031728) that pushes the `ctx_sub[0xF4..0x103]` block to the drive
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
                uint32_t res = FUN_08031728(
                    g_app_state.ctx_sub->audio_engine_cfg[0],
                    g_app_state.ctx_sub->audio_engine_cfg[1],
                    g_app_state.ctx_sub->audio_engine_cfg[2],
                    g_app_state.ctx_sub->audio_engine_cfg[3]);
                g_log_func("Set region and lock, res: %d\r\n", res);
            }
            (void)snapshot;
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
 * void handler(char *args). Session-context fields reached by raw offset use
 * a byte view of g_app_state.ctx_sub (== *(0x20009368 + 0x2DC), session_ctx
 * @ 0x200083A8). The logger g_log_func is the shared printf-style fn ptr. */
#define CTXB  ((uint8_t *)g_app_state.ctx_sub)

/* Battery- and shifter-side Modbus injectors (the `b*`/`s*` console commands
 * push a raw frame onto the corresponding inter-module bus). Not yet sourced. */
extern int modbus_bat_submit(void *frame);    /* 0x08039DDC — battery slave 0xAA */
extern int modbus_shift_submit(void *frame);  /* 0x080378A0 — shifter slave 0x20 */

/* `distance` — manually set the trip distance (tenths of a km) at ctx+0x31C and
 * echo it as whole.fraction km (OEM 0x08041360). No-arg path is silent. */
void console_cmd_distance(char *args)
{
    char *p = args;
    if (console_next_token(&p) == 0) {
        return;
    }
    uint32_t v = (uint32_t)strtol(p, NULL, 10);
    *(uint32_t *)(CTXB + 0x31C) = v;
    g_log_func("Set %u.%u Km\r\n", v / 10u, v % 10u);
}

/* `wheelsize` — set the wheel-diameter flag at ctx+0x10B (24->0, 28->1),
 * persist the config to both flash banks, then echo (OEM 0x08042120). */
void console_cmd_wheelsize(char *args)
{
    char *p = args;
    if (console_next_token(&p) != 0) {
        uint8_t  snapshot[0xC0];
        uint32_t val = (uint32_t)strtol(p, NULL, 10) & 0xFFFF;
        if (val == 24) {
            CTXB[0x10B] = 0;
        } else if (val == 28) {
            CTXB[0x10B] = 1;
        } else {
            g_log_func("wheel 24..28 for 24/28 inch\r\n");
        }
        memcpy(snapshot, CTXB + 0x104, sizeof snapshot);
        uint32_t res = FUN_08031728(*(uint32_t *)(CTXB + 0xF4), *(uint32_t *)(CTXB + 0xF8),
                                    *(uint32_t *)(CTXB + 0xFC), *(uint32_t *)(CTXB + 0x100));
        g_log_func("res: %d\r\n", res);
        (void)snapshot;
    }
    g_log_func("Wheel: %s\r\n", CTXB[0x10B] == 0 ? "24 inch" : "28 inch");
}

/* `speed` — override the speed setpoint, writing the value into two adjacent
 * u16 fields at ctx+0x3C4/+0x3C6 (OEM 0x0804131C). No-arg path is silent. */
void console_cmd_speed(char *args)
{
    char *p = args;
    if (console_next_token(&p) == 0) {
        return;
    }
    long v = strtol(p, NULL, 10);
    *(uint16_t *)(CTXB + 0x3C4) = (uint16_t)v;
    *(uint16_t *)(CTXB + 0x3C6) = (uint16_t)v;
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
 * at *(ctx+0x3E8) (16-byte text fields), plus the BLE MAC (ctx+0x390..0x395).
 * OEM 0x08040D14. */
void console_cmd_gsminfo(char *args)
{
    (void)args;
    char *m = *(char **)(CTXB + 0x3E8);
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
    *(volatile uint8_t *)0x200000E5u = 0;   /* sms_info_tracking_state_machine step */
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
    frame[0] = 0xAA;                            /* battery/BMS slave */
    frame[1] = 0x06;                            /* write single register */
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
    req[0] = 0xAA;                              /* battery slave */
    req[1] = 0x03;                              /* read holding registers */
    *(uint16_t *)&req[2] = (uint16_t)reg;
    req[0x84] = (uint8_t)len;                   /* register count */
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
    frame[0] = 0x20;                            /* shifter slave */
    frame[1] = 0x10;                            /* write multiple registers */
    *(uint16_t *)&frame[2] = (uint16_t)reg;
    frame[0x84] = (uint8_t)(length << 1);       /* byte count */
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
    req[0] = 0x20;                              /* shifter slave */
    req[1] = 0x03;                              /* read holding registers */
    *(uint16_t *)&req[2] = (uint16_t)reg;
    req[0x84] = (uint8_t)count;
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
extern void     FUN_0803c5f0(int);
extern void     FUN_0803c5fc(int);
extern void     FUN_0803c608(int);
extern void     FUN_0803b2c4(void);
extern int      FUN_0803d110(void);
extern void     FUN_0803c1c0(void);
extern void     FUN_080398b8(void);
extern void     FUN_080314a4(void);

void console_cmd_factory_shipping(char *args)
{
    uint16_t modbus_payload;
    uint8_t  ble_payload;
    (void)args;

    g_log_func("Set factory shipping mode\r\n");

    HAL_GPIO_WritePin((void *)0x40020C00u, 0x2000, 0);   /* GPIOD */
    HAL_GPIO_WritePin((void *)0x40020C00u, 0x8000, 0);
    HAL_GPIO_WritePin((void *)0x40021000u, 0x0004, 0);   /* GPIOE */
    HAL_GPIO_WritePin((void *)0x40020000u, 0x8000, 0);   /* GPIOA */
    HAL_GPIO_WritePin((void *)0x40020000u, 0x1000, 0);
    HAL_GPIO_WritePin((void *)0x40021000u, 0x0040, 1);

    FUN_0803c5f0(0);
    FUN_0803c5fc(0);
    FUN_0803c608(0);
    FUN_0803b2c4();
    (void)FUN_0803d110();

    HAL_GPIO_WritePin((void *)0x40021000u, 0x0020, 1);
    HAL_GPIO_WritePin((void *)0x40020400u, 0x0200, 1);   /* GPIOB */
    systick_delay(10);
    HAL_GPIO_WritePin((void *)0x40021000u, 0x0020, 0);
    HAL_GPIO_WritePin((void *)0x40020400u, 0x0200, 0);
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

    FUN_0803c1c0();
    FUN_080398b8();
    FUN_080314a4();
    systick_delay(1000);
    shipping_powerdown_deinit(6);   /* does not return */
}
