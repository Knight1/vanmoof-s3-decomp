/*
 * lighting.c -- VanMoof S3 mainware front/rear lamp engine + ambient light sensor.
 *
 * The three lamp channels are TIM PWM compare outputs (CCR1/CCR2/CCR3). The
 * animation engine fades each channel toward a target brightness and runs the
 * per-mode flash patterns (table-driven); the target is chosen from the light
 * mode (auto/on/off), the ambient-light sensor reading, and the power state.
 * The ambient sensor is read over I2C2 with retry + fault handling.
 *
 * Behaviour-equivalent reconstruction of the live disassembly (exact control
 * flow, channel offsets, fade math, pattern step-tables, and log strings);
 * transcribed + adversarially verified against the binary.
 *
 * Memory model (resolved from the OEM literal pool):
 *   g_lights 0x20006DC0  three 0x14-byte channels (ch0 +0x00 / ch1 +0x14 /
 *                        ch2 +0x28); per channel: +0x00 brightness callback,
 *                        +0x0C target, +0x0D current, +0x04 pattern selector,
 *                        +0x06 step index, scheduler slots, +0x08 step-table.
 *   light sensor obj 0x20000090  cached lux at +2.
 *   lamp PWM object  0x20009A84  holds the TIM base (CCR1/2/3 = +0x34/+0x38/+0x3C).
 */

#include <stdint.h>

#include "lighting.h"
#include "scheduler.h"  /* scheduler_alloc/start/slot_is_idle/set_timer_name */

/* lamp animation channels + ambient sensor object. */
#define G_LIGHTS  ((uint8_t *)0x20006DC0u)
#define G_LSENS   ((uint8_t *)0x20000090u)
#define I2C2_DEV  0x20009BB8u           /* shared I2C2 device handle */

/* cross-module leaf helpers (K&R: gc-section out; sidestep signature clashes). */
extern int FUN_08024760();          /* I2C2 blocking mem-write (HAL) */
extern int FUN_080248d8();          /* I2C2 blocking mem-read  (HAL) */
extern int FUN_0803c8a8();          /* I2C2 bus-busy check (0 = ok, 2 = busy) */
extern int state_flags_set();
extern int state_flags_clear();
extern int power_state_get_clamped();

/*
 * light_pattern_action_apply @ 0x080379dc
 *
 * Executes one pattern "action" opcode against a lamp channel.
 *  action : opcode byte from the per-pattern step table (see s_light_pattern_*).
 *  level  : points at the channel's brightness pair == &ch[0x0c]
 *           level[0x00] = target brightness, level[0x02] = fade-timer slot id,
 *           level[0x04] = u16 fade interval.
 *  ch     : channel base; *(fn ptr) at ch[0x00] writes the lamp PWM CCR.
 *
 * Opcodes 0..2 set brightness immediately (and pin target/current so the
 * fade engine in light_pattern_step does not drift). Opcodes 3..6 start the
 * fade-timer with a target brightness + interval. action==8 is the table
 * terminator handled by the caller (light_pattern_step), not here; any value
 * > 6 falls through to the default (no-op) arm. Byte arg is uxtb'd, so the
 * switch selector is unsigned.
 */
void light_pattern_action_apply(uint8_t action, uint8_t *level, uint8_t *ch)
{
    void (*cb)(uint32_t) = *(void (**)(uint32_t))ch;   /* ch[0x00] brightness callback */

    switch ((uint8_t)action) {
    case 0:
        cb(100);
        ch[0x0c] = 100;   /* target brightness  */
        ch[0x0d] = 100;   /* current brightness */
        break;
    case 1:
        cb(0x32);
        ch[0x0c] = 0x32;
        ch[0x0d] = 0x32;
        break;
    case 2:
        cb(0);
        ch[0x0c] = 0;
        ch[0x0d] = 0;
        break;
    case 3:
        level[0x00] = 100;                 /* ch[0x0c] target brightness */
        *(uint16_t *)(level + 0x04) = 0x1e; /* ch[0x10] fade interval = 30 */
        scheduler_start(level[0x02], 0x1e, (void *)0x0); /* ch[0x0e] fade slot */
        break;
    case 4:
        level[0x00] = 0;
        *(uint16_t *)(level + 0x04) = 0x1e;
        scheduler_start(level[0x02], 0x1e, (void *)0x0);
        break;
    case 5:
        level[0x00] = 100;
        *(uint16_t *)(level + 0x04) = 6;
        scheduler_start(level[0x02], 6, (void *)0x0);
        break;
    case 6:
        level[0x00] = 0;
        *(uint16_t *)(level + 0x04) = 6;
        scheduler_start(level[0x02], 6, (void *)0x0);
        break;
    default:
        break;
    }
}

/* Ambient light-sensor periodic step.
 * G_LSENS layout used here:
 *   +0x00 (u8)  scheduler slot id  (0xFA == 'no slot' sentinel; ARM char is unsigned)
 *   +0x02 (u16) last good lux value (returned)
 * G_LIGHTS+0x3c (u8) = consecutive-read fault counter.
 * The 0x20009d98 cell is a runtime-populated fault-callback fn-ptr (vtable cell);
 * 0x08052770 = " ERR CM2323\r\n" log string passed to it. */
uint16_t light_sensor_read_step(void)
{
    uint16_t lux = 0;

    /* allocate + arm the 1500ms (0x5dc) poll timer on first use */
    if (*(uint8_t *)G_LSENS == 0xFA) {
        uint8_t slot = scheduler_alloc();
        *(uint8_t *)G_LSENS = slot;
        scheduler_start(slot, 0x5dc, (void *)0x0);
        scheduler_set_timer_name(*(uint8_t *)G_LSENS, 0x5dc, "lux_tmr"); /* 0x08052768 */
    }

    if (scheduler_slot_is_idle(*(uint8_t *)G_LSENS) != 0) {
        if (light_sensor_fault_count_get() > 10) {
            scheduler_start(*(uint8_t *)G_LSENS, 0x5dc, (void *)0x0);
            if (light_sensor_i2c_read(&lux) == 0) {
                /* success: store fresh lux, clear fault counter + fault flag */
                *(uint16_t *)(G_LSENS + 2) = lux;
                *(uint8_t *)(G_LIGHTS + 0x3c) = 0;
                state_flags_clear(0, 0x80);
            } else {
                /* failure: bump fault counter (unsigned 8-bit wrap) */
                uint8_t fc = (uint8_t)(*(uint8_t *)(G_LIGHTS + 0x3c) + 1);
                *(uint8_t *)(G_LIGHTS + 0x3c) = fc;
                if (fc > 4) {
                    state_flags_set(0, 0x80);
                    (*(void (**)(uint32_t))0x20009d98)((uint32_t)0x08052770); /* " ERR CM2323\r\n" */
                }
            }
        }
    }

    return *(uint16_t *)(G_LSENS + 2);
}

/*
 * light_pattern_step @ 0x08037b64
 *
 * Lamp animation state machine, driven once per scheduler tick. Three lamp
 * channels live in G_LIGHTS (0x20006DC0) at a 0x14 stride:
 *   ch0 = G_LIGHTS+0x00 (cb obj_set_field38 / TIM CCR1),
 *   ch1 = G_LIGHTS+0x14 (cb obj_set_field34 / TIM CCR2),
 *   ch2 = G_LIGHTS+0x28 (cb led_channel3_set_brightness / TIM CCR3).
 * Per-channel layout (relative to the channel base):
 *   +0x00 u32  brightness callback (void(*)(uint32_t))
 *   +0x04 u8   pattern state/case selector (0..10 = "load table N", 0x0e = run)
 *   +0x05 u8   latched request value (1/3/0x0b gate ambient/power evaluation)
 *   +0x06 u8   current pattern step index
 *   +0x07 u8   pattern-step timer slot id
 *   +0x08 u32  active pattern step-table pointer
 *   +0x0c u8   target brightness
 *   +0x0d u8   current brightness (single-step fade toward target)
 *   +0x0e u8   fade timer slot id
 *   +0x10 u16  fade tick interval
 * G_LIGHTS+0x3d is the one-shot init flag for the whole object.
 *
 * params: trigger -> *trigger is a pending request byte (consumed -> 0);
 *         channel  -> 0/1/else select ch0/ch1/ch2;
 *         threshold-> ambient-light threshold for auto on/off;
 *         mode     -> 0 auto(ambient), 1 force-on(100), 2 force-off(0).
 */
void light_pattern_step(uint8_t *trigger, int channel, uint32_t threshold, int mode)
{
    /*
     * Per-pattern step tables (flash rodata). Each entry is 4 bytes:
     *   {uint8_t action; uint8_t pad; uint16_t interval;}
     * action==8 terminates a table. Decoded byte-exact from flash; see the
     * source address noted on each table below.
     */
    typedef struct { uint8_t action; uint8_t pad; uint16_t interval; } light_step_t;

    static const light_step_t s_light_pat0[] = {            /* 0x0804f178 */
        {0, 0, 0}, {8, 0, 0},
    };
    static const light_step_t s_light_pat1[] = {            /* 0x0804f180 */
        {2, 0, 0}, {8, 0, 0},
    };
    static const light_step_t s_light_pat2[] = {            /* 0x0804f188 */
        {3, 0, 50}, {8, 0, 0},
    };
    static const light_step_t s_light_pat3[] = {            /* 0x0804f190 */
        {4, 0, 50}, {8, 0, 0},
    };
    static const light_step_t s_light_pat4[] = {            /* 0x0804f198 */
        {1, 0, 2000}, {2, 0, 1000}, {2, 0, 0}, {8, 0, 0},
    };
    static const light_step_t s_light_pat5[] = {            /* 0x0804f1a8 */
        {0, 0, 200}, {2, 0, 200}, {0, 0, 500}, {4, 0, 15}, {8, 0, 0},
    };
    static const light_step_t s_light_pat6[] = {            /* 0x0804f1bc */
        {0, 0, 200}, {2, 0, 200}, {0, 0, 500}, {2, 0, 500}, {8, 0, 0},
    };
    static const light_step_t s_light_pat7[] = {            /* 0x0804f1d0 */
        {2, 0, 80},  {5, 0, 18},  {6, 0, 18},  {2, 0, 30},
        {0, 0, 50},  {2, 0, 50},  {0, 0, 50},  {2, 0, 60},
        {0, 0, 50},  {2, 0, 270}, {0, 0, 50},  {2, 0, 580},
        {0, 0, 50},  {2, 0, 590}, {0, 0, 50},  {2, 0, 590},
        {0, 0, 50},  {2, 0, 580}, {0, 0, 50},  {2, 0, 220},
        {8, 0, 0},
    };
    static const light_step_t s_light_pat8[] = {            /* 0x0804f224 */
        {2, 0, 100},
        {5, 0, 60}, {6, 0, 60}, {5, 0, 60}, {6, 0, 60}, {5, 0, 60},
        {6, 0, 60}, {5, 0, 60}, {6, 0, 60}, {5, 0, 60}, {6, 0, 60},
        {5, 0, 60}, {6, 0, 60}, {5, 0, 60}, {6, 0, 60}, {5, 0, 60},
        {6, 0, 60}, {5, 0, 60}, {6, 0, 60}, {5, 0, 60}, {6, 0, 60},
        {2, 0, 50},
        {0, 0, 45}, {2, 0, 45}, {0, 0, 45}, {2, 0, 45}, {0, 0, 50}, {2, 0, 50},
        {0, 0, 45}, {2, 0, 45}, {0, 0, 45}, {2, 0, 45}, {0, 0, 50}, {2, 0, 50},
        {0, 0, 45}, {2, 0, 45}, {0, 0, 45}, {2, 0, 45}, {0, 0, 50}, {2, 0, 50},
        {0, 0, 45}, {2, 0, 45}, {0, 0, 50}, {2, 0, 45}, {0, 0, 45}, {2, 0, 45},
        {0, 0, 45}, {2, 0, 45}, {0, 0, 50}, {2, 0, 50},
        {8, 0, 0},
    };
    static const light_step_t s_light_pat9[] = {            /* 0x0804f2f0 */
        {0, 0, 200}, {2, 0, 200}, {0, 0, 200}, {2, 0, 200},
        {0, 0, 200}, {2, 0, 200}, {0, 0, 600}, {2, 0, 200},
        {0, 0, 600}, {2, 0, 200}, {0, 0, 600}, {2, 0, 200},
        {0, 0, 200}, {2, 0, 200}, {0, 0, 200}, {2, 0, 200},
        {0, 0, 200}, {2, 0, 200}, {2, 0, 3000}, {8, 0, 0},
    };

    uint8_t *base = (uint8_t *)0x20006DC0;   /* G_LIGHTS (DAT_08037dc4) */
    uint8_t *ch;
    char trig;
    char state5;
    uint8_t cur, tgt;
    uint32_t lux;
    uint8_t sel;
    const light_step_t *tbl;

    /* One-shot init of all three channels (flag at G_LIGHTS+0x3d). */
    if (base[0x3d] == 0) {
        base[0x3d] = 1;
        /* ch0 */
        base[0x07] = scheduler_alloc();
        base[0x04] = 0;
        base[0x05] = 0;
        base[0x0e] = scheduler_alloc();
        *(uint32_t *)(base + 0x00) = (uint32_t)&obj_set_field38;        /* DAT_08037dcc = 0x0803c5fd */
        /* ch1 (base+0x14) */
        base[0x1b] = scheduler_alloc();
        base[0x18] = 0;
        base[0x19] = 0;
        base[0x22] = scheduler_alloc();
        *(uint32_t *)(base + 0x14) = (uint32_t)&obj_set_field34;        /* DAT_08037dd0 = 0x0803c5f1 */
        /* ch2 (base+0x28) */
        base[0x2f] = scheduler_alloc();
        base[0x2c] = 0;
        base[0x2d] = 0;
        base[0x36] = scheduler_alloc();
        *(uint32_t *)(base + 0x28) = (uint32_t)&led_channel3_set_brightness; /* DAT_08037dd4 = 0x0803c609 */
    }

    /* Select the channel struct. */
    if (channel == 0) {
        ch = base;                              /* DAT_08037dc4 = 0x20006DC0 (ch0) */
    } else if (channel == 1) {
        ch = (uint8_t *)0x20006DD4;             /* DAT_08037dd8 (ch1 = base+0x14) */
    } else {
        ch = (uint8_t *)0x20006DE8;             /* DAT_08037dc8 (ch2 = base+0x28) */
    }

    /* Latch the incoming request. */
    trig = (char)trigger[0];
    if (trig != 0 && (char)ch[0x04] == 0) {
        ch[0x04] = (uint8_t)trig;
    }
    if (trig != 0) {
        ch[0x05] = (uint8_t)trig;
    }

    /* Requests 1, 0x0b, 3 re-evaluate target brightness vs mode/ambient/power. */
    state5 = (char)ch[0x05];
    if (state5 == 1 || state5 == 0x0b || state5 == 3) {
        if (mode == 1) {
            ch[0x0c] = 100;
        } else if (mode == 2) {
            ch[0x0c] = 0;
        } else if (mode == 0 && light_sensor_read_step() != 0xfffe) {
            lux = (uint32_t)light_sensor_read_step();
            if (lux < threshold) {
                ch[0x0c] = 100;
            } else {
                ch[0x0c] = 0;
            }
        }
        if (ch[0x0c] != 0 && power_state_get_clamped() == 2) {
            ch[0x0c] = 0x32;
        } else if (power_state_get_clamped() == 1) {
            ch[0x0c] = 0;
        }
    }

    /* Consume the request byte. */
    trigger[0] = 0;

    /* Fade engine: when the fade-timer slot is idle, step current toward target. */
    if (scheduler_slot_is_idle(ch[0x0e]) != 0) {
        scheduler_start(ch[0x0e], *(uint16_t *)(ch + 0x10), (void *)0x0);
        tgt = ch[0x0c];
        cur = ch[0x0d];
        if (tgt != cur) {
            if (cur < tgt) {                  /* fade up: cur+1, push to PWM (OEM 0x08037c90) */
                cur = (uint8_t)(cur + 1);
                ch[0x0d] = cur;
                (*(void (**)(uint32_t))ch)(cur);
            }
            if (ch[0x0c] < ch[0x0d]) {        /* fade down: cur-1, push to PWM (OEM 0x08037ca2) */
                cur = (uint8_t)(ch[0x0d] - 1);
                ch[0x0d] = cur;
                (*(void (**)(uint32_t))ch)(cur);
            }
        }
    }

    /* Pattern state machine on ch[0x04]. Cases 0..10 load a step-table and
     * advance to the runner state (0x0e). Cases 11..13 fall through (default).
     * Selector is unsigned (cmp against 0xe). */
    sel = ch[0x04];
    switch (sel) {
    case 0:
        ch[0x06] = 0;   /* reset step index */
        break;
    case 1:  *(const light_step_t **)(ch + 0x08) = s_light_pat0;
             scheduler_start(ch[0x07], 1, (void *)0x0); ch[0x04] = 0x0e; break;
    case 2:  *(const light_step_t **)(ch + 0x08) = s_light_pat1;
             scheduler_start(ch[0x07], 1, (void *)0x0); ch[0x04] = 0x0e; break;
    case 3:  *(const light_step_t **)(ch + 0x08) = s_light_pat2;
             scheduler_start(ch[0x07], 1, (void *)0x0); ch[0x04] = 0x0e; break;
    case 4:  *(const light_step_t **)(ch + 0x08) = s_light_pat3;
             scheduler_start(ch[0x07], 1, (void *)0x0); ch[0x04] = 0x0e; break;
    case 5:  *(const light_step_t **)(ch + 0x08) = s_light_pat4;
             scheduler_start(ch[0x07], 1, (void *)0x0); ch[0x04] = 0x0e; break;
    case 6:  *(const light_step_t **)(ch + 0x08) = s_light_pat5;
             scheduler_start(ch[0x07], 1, (void *)0x0); ch[0x04] = 0x0e; break;
    case 7:  *(const light_step_t **)(ch + 0x08) = s_light_pat6;
             scheduler_start(ch[0x07], 1, (void *)0x0); ch[0x04] = 0x0e; break;
    case 8:  *(const light_step_t **)(ch + 0x08) = s_light_pat7;
             scheduler_start(ch[0x07], 1, (void *)0x0); ch[0x04] = 0x0e; break;
    case 9:  *(const light_step_t **)(ch + 0x08) = s_light_pat8;
             scheduler_start(ch[0x07], 1, (void *)0x0); ch[0x04] = 0x0e; break;
    case 10: *(const light_step_t **)(ch + 0x08) = s_light_pat9;
             scheduler_start(ch[0x07], 1, (void *)0x0); ch[0x04] = 0x0e; break;
    case 0x0e:
        if (scheduler_slot_is_idle(ch[0x07]) != 0) {
            tbl = *(const light_step_t **)(ch + 0x08);
            if (tbl[ch[0x06]].action == 8) {
                ch[0x04] = 0;   /* terminator -> back to idle (pattern done) */
            } else {
                scheduler_start(ch[0x07], tbl[ch[0x06]].interval, (void *)0x0);
                light_pattern_action_apply(tbl[ch[0x06]].action, ch + 0x0c, ch);
                ch[0x06] = (uint8_t)(ch[0x06] + 1);
            }
        }
        break;
    default:
        break;
    }
}

/* Blocking I2C2 read of the CM2323 ambient light sensor (dev addr 0x20).
 * Writes register pointer 0x50, then reads back 2 data bytes; composes them
 * little-endian into *out. Returns 0 on success, 2 if the bus is busy,
 * else the OR of the two HAL status codes. Timeout = 0x32 (50). */
uint8_t light_sensor_i2c_read(uint16_t *out)
{
    uint8_t status;
    uint8_t buf[4]; /* matches Ghidra local_1c[4] (undefined2[4]); only [0],[1] used */

    if (FUN_0803c8a8() == 0) {
        void *h = (void *)I2C2_DEV; /* 0x20009BB8 */
        buf[0] = 0x50;
        status  = (uint8_t)FUN_08024760(h, 0x20, buf, 1, 0x32); /* write reg pointer */
        status |= (uint8_t)FUN_080248d8(h, 0x20, buf, 2, 0x32); /* read 2 data bytes */
        *out = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    } else {
        status = 2; /* bus busy */
    }
    return status;
}

/* Returns the light-sensor fault/throttle counter byte at 0x200000DF + 5 = 0x200000E4. */
uint8_t light_sensor_fault_count_get(void)
{
    return *(uint8_t *)(0x200000DF + 5);
}

/* OEM obj_set_field34 0x0803C5F0 -- lamp PWM CCR1 (TIM base @ *0x20009A84 + 0x34). */
void obj_set_field34(uint32_t duty)
{
    *(volatile uint32_t *)(*(volatile uint32_t *)0x20009A84u + 0x34) = duty;
}

/* OEM obj_set_field38 0x0803C5FC -- lamp PWM CCR2 (TIM base @ *0x20009A84 + 0x38). */
void obj_set_field38(uint32_t duty)
{
    *(volatile uint32_t *)(*(volatile uint32_t *)0x20009A84u + 0x38) = duty;
}

/* OEM led_channel3_set_brightness 0x0803C608 -- lamp PWM CCR3 (TIM base @ *0x20009A84 + 0x3C). */
void led_channel3_set_brightness(uint32_t duty)
{
    *(volatile uint32_t *)(*(volatile uint32_t *)0x20009A84u + 0x3c) = duty;
}
