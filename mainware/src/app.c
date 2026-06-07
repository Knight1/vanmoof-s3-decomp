#include <stdint.h>

#include "app.h"
#include "audio.h"
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
