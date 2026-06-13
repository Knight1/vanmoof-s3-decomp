#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "app.h"
#include "audio.h"
#include "crc.h"        /* crc_dev_t, HAL_CRC_Accumulate handle */
#include "flash.h"      /* struct boot_cfg_block, config_persist_dual_bank */
#include "i2c.h"
#include "log.h"
#include "scheduler.h"
#include "ssp.h"
#include "systick.h"

/* Subsystem firmware-update mode (OEM: byte at SRAM 0x20000076 — offset +1 of
 * a small update-control block at 0x20000075). The idle/resting value is 2; a
 * request only takes hold from idle. */
uint8_t g_update_mode;

/* Broadcast/announce dirty-flags block (OEM at SRAM 0x20000288). The two
 * channel request flags live at +5 and +6; the loop clears them after it has
 * broadcast the corresponding value. */
struct announce_state {
    uint8_t _pad0[5];      /* +0x00..+0x04 */
    uint8_t ch_dirty[2];   /* +0x05 = channel 0, +0x06 = channel 1 */
};
struct announce_state g_announce;

void update_mode_request(uint8_t mode)
{
    if (g_update_mode == 2) {
        g_update_mode = mode;
    }
}

void announce_mark(int channel)
{
    if (channel == 0) {
        g_announce.ch_dirty[0] = 1;
        return;
    }
    if (channel != 1) {
        return;
    }
    g_announce.ch_dirty[1] = 1;
}

/* Guarded FSM state-byte setter (OEM maybe_set_state_if_unlocked, 0x08029B88).
 * The state byte is at +4 of a small state object at SRAM 0x20000029 (accessed
 * by absolute address, matching the OEM literal). States 6 and 7 are
 * sticky/locked and cannot be overwritten through this path. */
void maybe_set_state_if_unlocked(uint8_t new_state)
{
    volatile uint8_t *state = (volatile uint8_t *)(0x20000029u + 4u);

    if ((uint8_t)(*state - 6u) > 1u) {   /* current state is neither 6 nor 7 */
        *state = new_state;
    }
}

/* Read the bike FSM state byte (OEM maybe_get_bike_state, 0x08029BA0) — the
 * getter twin of maybe_set_state_if_unlocked (both on SRAM 0x2000002D). */
uint8_t maybe_get_bike_state(void)
{
    return *(volatile uint8_t *)(0x20000029u + 4u);
}

/* Latch a requested value and raise its pending flag (OEM maybe_set_pending_request,
 * 0x0803B738). The flag is only ever set here; the servicing code clears it. */
struct request_ctx {
    uint8_t _pad0[0x130];
    uint8_t pending;      /* +0x130 */
    uint8_t _pad1[3];     /* +0x131..+0x133 */
    int32_t value;        /* +0x134 */
};
extern struct request_ctx g_request_ctx;   /* SRAM 0x20008230 */

void maybe_set_pending_request(int32_t value)
{
    g_request_ctx.value = value;
    if (value != 0) {
        g_request_ctx.pending = 1;
    }
}

/* Generic state/mode flag byte at SRAM 0x20000083 (OEM state_flag_get 0x08036B8C,
 * state_flag_set 0x08036B80). Several subsystems write it (subsystem_update_sm,
 * the announce path); the log timestamp prefix saves, zeroes, and restores it
 * around a line. Plain byte accessors — not an interrupt mask. */
uint8_t state_flag_get(void)
{
    return *(volatile uint8_t *)0x20000083u;
}

void state_flag_set(uint8_t value)
{
    *(volatile uint8_t *)0x20000083u = value;
}

/* Resolve a channel's status from three per-channel priority bitmasks in the
 * app context (OEM channel_resolve_status, 0x0802A2B0). The context base lives
 * in a pointer slot at 0x20000944; masks are at +0xF4 (prio 1), +0xF8 (prio 2),
 * +0xFC (prio 3). channel_id selects the bit; returns the highest-priority
 * status (1..3) that has the bit set, else 0. */
extern volatile uint32_t g_app_ctx_ptr;   /* 0x20000944 -> app context base */

uint32_t channel_resolve_status(uint32_t channel_id)
{
    uint8_t *ctx = (uint8_t *)(uintptr_t)g_app_ctx_ptr;
    uint32_t bit = channel_id & 0xFFu;

    if ((*(volatile uint32_t *)(ctx + 0xF4) >> bit) & 1u) return 1;
    if ((*(volatile uint32_t *)(ctx + 0xF8) >> bit) & 1u) return 2;
    if ((*(volatile uint32_t *)(ctx + 0xFC) >> bit) & 1u) return 3;
    return 0;
}

/* Emit a channel/sound notification (OEM channel_notify_emit, 0x0802A064).
 *
 * For the four sound channel codes (5/0x1B/0x1C/0x1D), and only while the
 * app-context mode byte (*g_app_ctx_ptr + 0x310) is 0x0B, it runs a sound/
 * clocking sub-path: it records the prior bike state and an aux byte into the
 * record at 0x20000029, force-sets bike state 0x3D, allocates a scheduler slot
 * if none is held, arms it for a channel-specific duration, and selects a
 * clocking sub-mode via set_mode_state_byte.
 *
 * The common tail always runs: release a (separate) scheduler slot, read the
 * per-channel volume byte at ctx+volume_index+0x104, push it through the amp
 * brownout limiter (logging "ERR set volume" and bumping a counter on failure),
 * print the timestamp prefix + "SOUND_S%c vol %d", and — every third volume
 * failure — print "Clocking %d". Finally enqueue a 2-byte BLE notify {code, 1}
 * as SSP command 0xC8, logging "ERROR SSPB place" if the queue is full. */
void channel_notify_emit(uint32_t channel_code, int32_t volume_index)
{
    uint8_t *ctx = (uint8_t *)(uintptr_t)g_app_ctx_ptr;
    uint8_t *rec = (uint8_t *)0x20000029u;             /* sound/clocking record */
    volatile int8_t *clk_ctr = (volatile int8_t *)(0x200001D8u + 0xC); /* volume-fail / clocking counter */

    if ((channel_code == 5 || channel_code == 0x1B ||
         channel_code == 0x1C || channel_code == 0x1D) &&
        *(volatile int8_t *)(ctx + 0x310) == 0x0B) {

        uint8_t st = maybe_get_bike_state();
        if (st != 0x3D) {
            rec[0x5] = st;
            rec[0x9] = aux_mode_byte_get();
            maybe_set_state_if_unlocked(0x3D);
        }
        if ((int8_t)rec[0xA] == (int8_t)SCHED_SLOT_NONE) {   /* no timer slot held */
            rec[0xA] = scheduler_alloc();
        }
        switch (channel_code) {
        case 5:
            scheduler_start(rec[0xA], 5000, 0);
            if (aux_mode_byte_get() != 0x26) {
                set_mode_state_byte(0x26);
            }
            break;
        case 0x1B:
            scheduler_start(rec[0xA], 0x672, 0);
            set_mode_state_byte(0x29);
            break;
        case 0x1C:
            scheduler_start(rec[0xA], 0x708, 0);
            set_mode_state_byte(0x28);
            break;
        case 0x1D:
            scheduler_start(rec[0xA], 0x79E, 0);
            set_mode_state_byte(0x27);
            break;
        }
    }

    /* common tail (always executed) */
    scheduler_release((uint8_t *)0x20000031u);

    uint8_t vol_level = *(volatile uint8_t *)(ctx + (uintptr_t)volume_index + 0x104);
    if (amp_volume_brownout_apply(&vol_level) != 0) {
        g_log_func("ERR set volume\r\n");
        *clk_ctr = (int8_t)(*clk_ctr + 1);
    }

    log_print_timestamp_prefix();
    int digit = (channel_code < 10) ? (int)(channel_code + 0x30)
                                    : (int)(channel_code + 0x37);
    g_log_func("SOUND_S%c vol %d\r\n", digit, vol_level);

    if (*clk_ctr == 3) {
        *clk_ctr = 0;
        g_log_func("Clocking %d\r\n", clock_pulse_gpioa8_until_pc9());
    }

    uint8_t notify[2] = { (uint8_t)channel_code, 1 };
    if (ssp_ble_enqueue_tx_packet(0xC8, 2, notify, 0) > 0x80) {
        g_log_func("  ERROR SSPB place\r\n");
    }
}

/* Resolve a channel's status and forward (id, status) to the notify emitter —
 * drives the display + enqueues a BLE notify. OEM channel_notify_with_status
 * at 0x0802A2F0. */
void channel_notify_with_status(uint32_t channel_id)
{
    int status = (int)channel_resolve_status(channel_id);
    channel_notify_emit(channel_id, status);
}

/* Global mode/sub-mode state byte at SRAM 0x20000068 (OEM aux_mode_byte_get
 * 0x0802F0F8, set_mode_state_byte 0x0802E7F4). Also byte[0] of the mode/state
 * block that enter_mode3_arm_show_timer drives. */
#define G_MODE_BYTE (*(volatile uint8_t *)0x20000068u)

uint8_t aux_mode_byte_get(void)
{
    return G_MODE_BYTE;
}

void set_mode_state_byte(uint8_t mode)
{
    G_MODE_BYTE = mode;
}

/* Enter "show"/display mode 3 and arm its periodic task (OEM
 * enter_mode3_arm_show_timer, 0x0802F104). Mode/state block @ 0x20000068
 * (byte[0]=mode, byte[7]=scheduler slot, 0xFA=unallocated); display block @
 * 0x20000288 (byte[4]=display sub-field, overlapping g_announce). Latches the
 * prior mode into the display field, lazily allocates+registers the "ssp_show_tmr"
 * task, (re)arms it at 4000 ticks, then commits mode 3. */
void enter_mode3_arm_show_timer(void)
{
    volatile uint8_t *st   = (volatile uint8_t *)0x20000068u;
    volatile uint8_t *disp = (volatile uint8_t *)0x20000288u;
    uint8_t mode = st[0];

    if ((uint8_t)(mode - 3u) > 1u) {        /* current mode not in {3,4} */
        disp[4] = mode;
    }
    if ((uint8_t)(disp[4] - 1u) < 2u) {     /* display sub-field is 1 or 2 */
        disp[4] = 6;
    }
    if (st[7] == SCHED_SLOT_NONE) {         /* no timer slot held yet */
        uint8_t slot = scheduler_alloc();
        st[7] = slot;
        scheduler_set_timer_name(slot, 4000u, (const char *)0x08050820u);  /* "ssp_show_tmr" */
    }
    scheduler_start(st[7], 4000u, 0);
    st[0] = 3;                              /* commit mode 3 */
    reset_dual_buffers_and_flags();
}

/* I2C3 bus recovery (OEM clock_pulse_gpioa8_until_pc9, 0x0803C8F4 — the value
 * logged as "Clocking %d"). The EEPROM/I2C3 bus can hang with a slave holding
 * SDA low; this de-inits the I2C3 peripheral, manually clocks SCL (PA8, 3 ms
 * high / 3 ms low, watchdog-kicked) up to 200 times until SDA (PC9) releases
 * high, then re-inits I2C3. Returns the number of recovery clocks issued.
 * GPIOA 0x40020000, GPIOC 0x40020800. */
struct gpio_af_init { uint32_t Pin, Mode, Pull, Speed, Alternate; };
extern void HAL_GPIO_Init(void *GPIOx, struct gpio_af_init *init);     /* 0x080267D0 */
extern void HAL_GPIO_WritePin(void *GPIOx, uint16_t pin_mask, int state); /* 0x08026AC6 */
extern int  HAL_GPIO_ReadPin(void *GPIOx, uint16_t pin_mask);          /* 0x08026AB8 */
extern void watchdog_kick(void);                                       /* 0x080314D8 */

uint8_t clock_pulse_gpioa8_until_pc9(void)
{
    struct gpio_af_init init = { 0, 0, 0, 0, 0 };
    uint8_t count = 0;

    i2c3_handle_deinit();                  /* release I2C3 so SCL can be driven */

    init.Pin = 0x100; init.Mode = 1; init.Pull = 0; init.Speed = 0;   /* PA8 = SCL output */
    HAL_GPIO_Init((void *)0x40020000u, &init);

    while (count < 200 && HAL_GPIO_ReadPin((void *)0x40020800u, 0x200) == 0) {  /* PC9 = SDA */
        watchdog_kick();
        HAL_GPIO_WritePin((void *)0x40020000u, 0x100, 1);
        systick_delay(3);
        HAL_GPIO_WritePin((void *)0x40020000u, 0x100, 0);
        systick_delay(3);
        count++;
    }

    i2c3_handle_init();                    /* re-init I2C3 */
    return count;
}

/* Read the subsystem update-mode / link-ready byte at SRAM 0x20000076 (OEM
 * get_link_state, 0x080313D8 — the getter twin of update_mode_request; callers
 * such as maybe_enqueue_tx_message treat value 2 = idle/ready as "connected"). */
uint8_t update_mode_get(void)
{
    return *(volatile uint8_t *)0x20000076u;
}

/* Reset the request-context dual buffers + flags (OEM reset_dual_buffers_and_flags,
 * 0x0803B780): zero the two 0x96-byte buffers at g_request_ctx (0x20008230) +0x01
 * and +0x99, set the status byte at +0x132, then clear the +0x130/+0x131 flags. */
extern void *FUN_08020ff4(void *dst, int val, unsigned int len);   /* memset */
extern void  display_request_clear(void);                                   /* clears +0x130/+0x131 */

void reset_dual_buffers_and_flags(void)
{
    uint8_t *base = (uint8_t *)0x20008230u;

    FUN_08020ff4(base + 0x01, 0, 0x96);
    FUN_08020ff4(base + 0x99, 0, 0x96);
    *(volatile uint8_t *)(base + 0x132) = 1;
    display_request_clear();
}

/* ── bike-state derivations ──────────────────────────────────────────────────
 * All read the FSM state object at SRAM 0x20000029 (state byte at +4) that
 * maybe_get/set_bike_state and status_process share, and feed the BLE read
 * surface (ble_read_request_dispatch / ble_telemetry_change_broadcast). */

extern int bike_is_locked(void);   /* physical lock-pin sense, decoded elsewhere */

/* Collapse the fine state byte into the coarse status enum reported over BLE
 * (OEM bike_status_coarse_get, 0x08029bac — a TBB jump table on state-3).
 * State 0x0D maps to 3 only while a subsystem update is in progress
 * (app-ctx +0x32C via the pointer at 0x20000944), else 0. */
uint8_t bike_status_coarse_get(void)
{
    uint8_t state = ((volatile uint8_t *)0x20000029u)[4];

    switch (state) {
    case 3:
    case 4:    return 2;
    case 6:
    case 7:    return 4;
    case 0x0c: return 1;
    case 0x0d:
        if (((const uint8_t *)(*(void **)0x20000944u))[0x32c] != 0) {
            return 3;
        }
        return 0;
    case 0x0e:
    case 0x15:
    case 0x1b:
    case 0x1d: return 0;
    case 0x0f: return 6;
    case 0x19:
    case 0x1a: return 3;
    default:   return 7;
    }
}

/* 1 iff the bike state byte == 0x0E (alarm-armed / standby) (OEM 0x08029b74). */
int bike_state_is_standby(void)
{
    return ((volatile uint8_t *)0x20000029u)[4] == 0x0Eu ? 1 : 0;
}

/* Coarse lock state for BLE: 0 unlocked / 1 locked / 2 pin-lock (OEM 0x0802a8e8).
 * States 0x28..0x37 are the pin-lock window (the OEM (uint8_t)(state-0x28) <= 0xF
 * unsigned-wrap test); otherwise defer to the physical lock sense. */
uint8_t ble_lock_state_get(void)
{
    uint8_t state = ((volatile uint8_t *)0x20000029u)[4];

    if ((uint8_t)(state - 0x28u) <= 0x0Fu) {
        return 2;
    }
    return bike_is_locked() != 0 ? 1 : 0;
}

/* Coarse unlock/alarm state for BLE (OEM ble_unlock_state_get, 0x0802a90c).
 * While the readiness byte (0x20000029+4) is < 2 the module is still booting (2);
 * otherwise map the app-ctx fine state at +0x310 through a small jump table. */
uint8_t ble_unlock_state_get(void)
{
    if (((volatile uint8_t *)0x20000029u)[4] < 2u) {
        return 2;
    }
    switch ((*(volatile uint8_t **)0x20000944u)[0x310]) {
    case 0:
    case 1:    return 2;
    case 2:    return 3;
    case 3:
    case 4:    return 4;
    case 0x0b: return bike_is_locked() ? 1 : 0;
    default:   return 0;
    }
}

/* Zero the app-context word at 0x200083A8 + 0x328 on the state-4 transition
 * (OEM app_ctx_clear_field_328, 0x0803dbb8). No header decl: ble.c/states.c keep
 * their own externs for it. */
void app_ctx_clear_field_328(void)
{
    *(volatile uint32_t *)(0x200083a8u + 0x328u) = 0;
}

/* Reset the SMS/GSM tracking-state flag byte at SRAM 0x200000E5 (OEM
 * clear_flag_00e5, 0x0803ce08). Byte access; a standalone flag, not a ctx field. */
void clear_flag_00e5(void)
{
    *(volatile uint8_t *)0x200000E5u = 0;
}

/* ── region speed-preset loader (OEM region_speed_preset_table_load, 0x0803FC24).
 * Copies a 6-word (24-byte) speed-preset record for the selected region into *out
 * (the caller passes out = ctx+0x1C4, the active speed-preset block). Each record
 * is 6 little-endian 32-bit words (functionally u16 ramp/limit pairs, copied raw).
 * Three flash source tables (OEM 0x0804F54C/0x0804F564/0x0804F57C) materialised
 * here as named arrays; region 0/2/default => EU/JP, 1 => US, 3 => OffRoad. */
static const uint32_t k_region_preset_eu_jp[6] = {   /* OEM 0x0804F54C */
    0x00000000u, 0x0064001Eu, 0x0082001Eu, 0x00BE001Eu, 0x0109001Eu, 0x01090064u,
};
static const uint32_t k_region_preset_us[6] = {      /* OEM 0x0804F564 */
    0x00000000u, 0x00A0001Eu, 0x00DC001Eu, 0x010E001Eu, 0x0140001Eu, 0x01400065u,
};
static const uint32_t k_region_preset_offroad[6] = { /* OEM 0x0804F57C */
    0x00000000u, 0x00A0001Eu, 0x00DC001Eu, 0x010E001Eu, 0x0168001Eu, 0x017C0065u,
};

void region_speed_preset_table_load(void *out, int region)
{
    uint32_t *dst = (uint32_t *)out;
    const uint32_t *src;

    switch (region) {
    case 1:  src = k_region_preset_us;      break;
    case 3:  src = k_region_preset_offroad; break;
    case 0:
    case 2:
    default: src = k_region_preset_eu_jp;   break;
    }

    /* OEM copies 4 words then 2 words via two ldmia; a 6-word copy is equivalent. */
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
    dst[4] = src[4];
    dst[5] = src[5];
}

/* Persisted bike-configuration block, living at session_ctx + 0xF4 (0xD0 bytes).
 * Field meanings are those printed by the `show` console command (docs/console.md).
 * config_persist_dual_bank takes the first 16 bytes as four scalar words and the
 * remaining 0xC0 bytes (from +0x104, the embedded boot_cfg_block) by value. */
typedef struct {
    uint32_t sound_group_low;       /* +0xF4  "Group low"    sound-effect mask, quiet volume tier */
    uint32_t sound_group_medium;    /* +0xF8  "Group medium"                                       */
    uint32_t sound_group_high;      /* +0xFC  "Group high"                                         */
    uint16_t backup_code;           /* +0x100 owner unlock code (d0+d1*10+d2*100); 0x00FF="not set"*/
    uint16_t dark_threshold_lux;    /* +0x102 "Dark %d Lx" auto-light turn-on threshold            */
    /* ── boot_cfg_block begins here (+0x104), persisted by value ───────────────*/
    uint8_t  cfg_byte_104;          /* +0x104 (unidentified; always 0)                            */
    uint8_t  volume_low;            /* +0x105 "Volume Low"                                        */
    uint8_t  volume_medium;         /* +0x106 "Volume Medium"                                     */
    uint8_t  volume_high;           /* +0x107 "Volume High"                                       */
    uint8_t  transmission_mode;     /* +0x108 e-shifter auto/manual                               */
    uint8_t  region;                /* +0x109 legal region — selects the assist row used at runtime*/
    uint8_t  units;                 /* +0x10A 0 = metric (km/h)                                   */
    uint8_t  wheel_size;            /* +0x10B wheel-size index                                    */
    uint8_t  light_mode;            /* +0x10C 0 = off, 1 = auto, 2 = on                           */
    uint8_t  _pad_10d;              /* +0x10D                                                     */
    uint16_t assist_up_hmh  [4][3]; /* +0x10E region[4] × moment[3]: assist *engage*  speed (0.1 km/h) */
    uint16_t assist_down_hmh[4][3]; /* +0x126 region[4] × moment[3]: assist *release* speed (0.1 km/h) */
    uint8_t  _pad_13e[2];           /* +0x13E                                                     */
    uint32_t saved_schema_version;  /* +0x140 config schema/version stamp                         */
    uint8_t  persisted_rest[0x80];  /* +0x144 remainder of boot_cfg_block (not touched here)      */
} bike_config_t;                    /* total 0xD0 bytes (== ctx+0xF4 .. ctx+0x1C3) */

/* Pin the overlay to the OEM byte offsets — a future field edit that shifts the
 * layout fails the build instead of silently corrupting persisted config. */
_Static_assert(sizeof(bike_config_t) == 0xD0,                          "config block is 0xD0 bytes");
_Static_assert(offsetof(bike_config_t, backup_code)         == 0x0c,   "backup_code @ ctx+0x100");
_Static_assert(offsetof(bike_config_t, cfg_byte_104)        == 0x10,   "boot_cfg_block @ ctx+0x104");
_Static_assert(offsetof(bike_config_t, assist_up_hmh)       == 0x1a,   "assist_up @ ctx+0x10E");
_Static_assert(offsetof(bike_config_t, assist_down_hmh)     == 0x32,   "assist_down @ ctx+0x126");
_Static_assert(offsetof(bike_config_t, saved_schema_version) == 0x4c,  "schema @ ctx+0x140");

/* Seed the three sound-group volume-tier bitmasks ("Group low/medium/high" at
 * cfg+0/+4/+8 == ctx+0xF4/F8/FC): low={}, medium=0x383F33FE, high=0x47C0CC00
 * (medium|high partition sound bits 1..30 disjointly). OEM 0x0803FAC0 — seeds
 * the audio groups, NOT the backup code despite this call site's neighbours. */
extern void sound_groups_init_default(void *cfg);

/* Load factory defaults into the config block (OEM settings_factory_reset,
 * 0x0803FAD8). mode==1 is a full wipe — clear the whole block and reset every
 * owner-settable preference (backup code → 0x00FF "not set", so there is no
 * built-in default code; region/units/wheel/dark-threshold to defaults). mode!=1
 * only refreshes the per-region speed presets, volumes, sound groups and schema,
 * leaving the user's code/region/units/wheel intact. Either way it persists the
 * record to both flash banks.
 *
 * The backup code (ctx+0x100) is a 3-digit owner code programmed over BLE
 * (CMD_BLE_SECURITY_BACKUP_CODE); value = d0 + d1*10 + d2*100, range 0..999. */
void settings_factory_reset(void *ctx_, int mode)
{
    bike_config_t *cfg = (bike_config_t *)((uint8_t *)ctx_ + 0xf4);

    /* Per-region pedal-assist "moment" presets (4 legal regions × 3 power
     * moments), in 0.1 km/h. up = speed the motor engages assist, down = speed
     * it releases it (hysteresis). Region 2 carries the raised speed cap. */
    static const uint16_t k_assist_up_hmh[4][3] = {
        { 100, 190, 240 },   /* region 0 */
        { 100, 190, 240 },   /* region 1 */
        { 100, 190, 280 },   /* region 2 — raised cap */
        { 100, 190, 240 },   /* region 3 */
    };
    static const uint16_t k_assist_down_hmh[4][3] = {
        {  80, 170, 220 },
        {  80, 170, 220 },
        {  80, 170, 250 },   /* region 2 */
        {  80, 170, 220 },
    };

    if (mode == 1) {
        memset(cfg, 0, sizeof *cfg);
        cfg->backup_code        = 0x00ff;   /* "not set" — no factory default code */
        cfg->dark_threshold_lux = 200;
        cfg->region             = 0;
        cfg->units              = 0;        /* metric */
        cfg->wheel_size         = 1;
    }

    memcpy(cfg->assist_up_hmh,   k_assist_up_hmh,   sizeof k_assist_up_hmh);
    memcpy(cfg->assist_down_hmh, k_assist_down_hmh, sizeof k_assist_down_hmh);

    cfg->cfg_byte_104      = 0;
    cfg->volume_low        = 20;
    cfg->volume_medium     = 30;
    cfg->volume_high       = 38;
    cfg->transmission_mode = 0;
    cfg->light_mode        = 1;             /* auto */
    sound_groups_init_default(cfg);         /* seed sound_group_low/medium/high */
    cfg->saved_schema_version = 0;

    /* arg 4 == the u32 at +0x100: backup_code (low half) | dark threshold (high). */
    uint8_t res = config_persist_dual_bank(
        cfg->sound_group_low, cfg->sound_group_medium, cfg->sound_group_high,
        ((uint32_t)cfg->dark_threshold_lux << 16) | cfg->backup_code,
        *(const struct boot_cfg_block *)&cfg->cfg_byte_104);
    g_log_func("res: %s\r\n", res ? "ERROR" : "OK");
}

/* Persist a 0x3C-byte "state record" to BOTH copies in the on-board EEPROM (OEM
 * save_state_record_to_eeprom, 0x0803E2CC). Like config_persist_dual_bank the
 * record arrives as 4 register words ([a][b][c][d] = the first 16 bytes) plus a
 * 0x2C-byte by-value tail; its last word (offset 0x38) is reserved for the CRC
 * over the first 0xE words. Written at EEPROM offset 0 and 0x40 (a 5 ms gap +
 * watchdog kick between). Returns the OR of the two per-write status bytes.
 *
 * ABI note: the OEM is called both with all 15 record words spread as positional
 * args (states.c/update.c/shifter.c — ABI-faithful to this by-value form) and with
 * 4 args + a caller-side staging block (ble.c/console.c/main.c); each caller keeps
 * its own extern, so this definition links with all of them. */
extern uint32_t HAL_CRC_Accumulate(crc_dev_t *hcrc, uint32_t *buf, uint32_t len);  /* 0x08023234 */
extern uint32_t eeprom_write_region(uint32_t addr, const uint8_t *src, uint32_t len); /* 0x0803E258 */
extern void     watchdog_kick(void);                                              /* 0x080314D8 */

struct state_record_tail { uint8_t bytes[0x2C]; };   /* the 0x2C-byte by-value tail */

uint8_t save_state_record_to_eeprom(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                                    struct state_record_tail tail)
{
    union {
        uint32_t w[0x0F];                                  /* 0x3C bytes = 15 words */
        struct { uint32_t hdr[4]; struct state_record_tail body; } rec;
    } u;

    u.rec.hdr[0] = a;
    u.rec.hdr[1] = b;
    u.rec.hdr[2] = c;
    u.rec.hdr[3] = d;
    u.rec.body   = tail;

    u.w[0x0E] = HAL_CRC_Accumulate((crc_dev_t *)0x20009D90u, u.w, 0x0E);  /* CRC words 0..0xD -> word 0xE */

    uint8_t rc1 = (uint8_t)eeprom_write_region(0x00, (const uint8_t *)u.w, 0x3C);
    watchdog_kick();
    systick_delay(5);
    uint8_t rc2 = (uint8_t)eeprom_write_region(0x40, (const uint8_t *)u.w, 0x3C);
    return (uint8_t)((rc1 | rc2) & 0xFF);
}
