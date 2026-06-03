#include "powerbankware.h"

/*
 * BMS super-loop per-state routines (main's 28-way dispatch on the state byte
 * 0x200005ac). Each is a thin wrapper around the shared BMS core update plus
 * the periodic tick processing keyed off the SysTick flag byte 0x2000077c:
 *   bit0  1 ms      bit1  slower     bit2  IWDG kick     bit3  1 s uptime
 *
 * The heavy shared callees (the BMS core, the AFE status poll) stay forward-
 * declared until their own passes; the state-machine dispatcher is now real C
 * (`bms_state_enter`, src/dispatch.c).
 */

/* ── Peripheral / GPIO bases ─────────────────────────────────────────── */
#define GPIOA_BASE   0x48000000u
#define GPIOB_BASE   0x48000400u
#define GPIOC_BASE   0x48000800u
#define PIN_PA7      0x80u
#define PIN_PB0      0x1u
#define PIN_PB9      0x200u
#define PIN_PB11     0x800u
#define PIN_PC13     0x2000u
#define IWDG_KR_PTR  ((volatile uint32_t **)0x200006ac)  /* IWDG handle -> KR */

/* ── BMS record / fault register ─────────────────────────────────────── */
#define CFG ((volatile uint8_t *)0x200004d0)
#define FAULT (*(volatile uint16_t *)0x20000410)

/* ── Charger round-robin sample array (15 u32 slots) ─────────────────── */
#define CHG_RR ((volatile uint32_t *)0x200003d4)

/* ── SysTick / uptime ────────────────────────────────────────────────── */
static volatile uint8_t  * const s_tick        = (volatile uint8_t  *)0x2000077c;
static volatile uint32_t * const s_evtlog_en   = (volatile uint32_t *)0x2000072c;
static volatile uint32_t * const s_uptime_1s   = (volatile uint32_t *)0x20000594;

/* ── BMS live globals (SRAM) ─────────────────────────────────────────── */
static volatile uint16_t * const s_mode        = (volatile uint16_t *)0x200006a0; /* mode/cfg word */
static volatile uint8_t  * const s_rx          = (volatile uint8_t  *)0x20000614; /* AFE RX frame buf */
static volatile uint16_t * const s_cell_voltage= (volatile uint16_t *)0x20000380; /* [10] */
static volatile uint16_t * const s_cell_sum    = (volatile uint16_t *)0x200003ce;
static volatile uint16_t * const s_cell_max    = (volatile uint16_t *)0x200003a2;
static volatile uint16_t * const s_cell_min    = (volatile uint16_t *)0x200003d2;
static volatile uint8_t  * const s_cell_max_idx= (volatile uint8_t  *)0x20000430;
static volatile uint8_t  * const s_cell_min_idx= (volatile uint8_t  *)0x200003c4;
static volatile uint8_t  * const s_cfgmax_dbnc = (volatile uint8_t  *)0x200003a0;
static volatile uint8_t  * const s_cfgmin_dbnc = (volatile uint8_t  *)0x200003a1;
static volatile uint8_t  * const s_fet_ctrl    = (volatile uint8_t  *)0x20000412; /* AFE reg-9 FET shadow */
static volatile uint8_t  * const s_test_mode   = (volatile uint8_t  *)0x2000069c; /* 0 = normal */
static volatile uint8_t  * const s_afe_substate= (volatile uint8_t  *)0x2000041a; /* AFE round-robin sub-state */
static volatile uint8_t  * const s_scan_dbnc   = (volatile uint8_t  *)0x2000041c; /* scan-gate counter */
static volatile uint8_t  * const s_afe_fault   = (volatile uint8_t  *)0x2000041d; /* AFE status bit1 latch */
static volatile uint16_t * const s_offset      = (volatile uint16_t *)0x20000418; /* zero-current offset */
static volatile uint16_t * const s_ship_cnt    = (volatile uint16_t *)0x200006a2; /* ship-mode tick counter */
static volatile uint32_t * const s_chg_voltage = (volatile uint32_t *)0x2000042c; /* detected charger voltage */
static volatile uint8_t  * const s_mos_state   = (volatile uint8_t  *)0x20000554;

/* current / coulomb integrator (cell_voltage_scan) */
static volatile uint32_t * const s_dsg_i       = (volatile uint32_t *)0x20000420; /* discharge magnitude */
static volatile uint32_t * const s_chg_i       = (volatile uint32_t *)0x200003a8; /* charge magnitude */
static volatile int32_t  * const s_cur_avg     = (volatile int32_t  *)0x20000424; /* signed avg current */
static volatile uint32_t * const s_cur_inst    = (volatile uint32_t *)0x2000039c; /* instantaneous (sign) */
static volatile int32_t  * const s_cur_ring    = (volatile int32_t  *)0x200003ac; /* [4] moving avg ring */
static volatile uint32_t * const s_dsg_mag     = (volatile uint32_t *)0x200003c0; /* discharge magnitude (pos) */
static volatile uint32_t * const s_chg_mag     = (volatile uint32_t *)0x20000414; /* charge magnitude (pos) */
static volatile uint16_t * const s_chg_cal     = (volatile uint16_t *)0x2000023c;
static volatile uint16_t * const s_dsg_cal     = (volatile uint16_t *)0x2000023e;
static volatile uint32_t * const s_rsoc        = (volatile uint32_t *)0x20000250; /* RSOC accumulator */
static volatile uint32_t * const s_rsoc_div    = (volatile uint32_t *)0x20000228; /* RSOC divisor */
static volatile uint8_t  * const s_rsoc_cnt    = (volatile uint8_t  *)0x2000022c; /* 40-sample counter */
static volatile uint8_t  * const s_chg_peak_dbnc = (volatile uint8_t *)0x200003a4;
static volatile uint8_t  * const s_dsg_peak_dbnc = (volatile uint8_t *)0x2000041e;
static volatile uint8_t  * const s_chg_min_dbnc  = (volatile uint8_t *)0x200003bc;
static volatile uint32_t * const s_chg_min       = (volatile uint32_t *)0x200003c8;

/* temperature step (bms_temp_step) */
static volatile uint8_t  * const s_ts          = (volatile uint8_t  *)0x20000218; /* +1 chg TS, +2 dsg TS */
static volatile uint8_t  * const s_ts_max_dbnc = (volatile uint8_t  *)0x2000021c;
static volatile uint8_t  * const s_ts_min_dbnc = (volatile uint8_t  *)0x20000212;
static volatile uint8_t  * const s_ts2_cal     = (volatile uint8_t  *)0x20000205;
static volatile uint8_t  * const s_ts1_cal     = (volatile uint8_t  *)0x2000021b;

/* protection-cascade debounce counters (bms_periodic_update / cell_voltage_scan) */
static volatile uint16_t * const s_ovp1_set_dbnc = (volatile uint16_t *)0x200005a0;
static volatile uint16_t * const s_ovp1_clr_dbnc = (volatile uint16_t *)0x2000058a;
static volatile uint16_t * const s_ovp2_set_dbnc = (volatile uint16_t *)0x20000586;
static volatile uint16_t * const s_ovp2_clr_dbnc = (volatile uint16_t *)0x20000598;
static volatile uint16_t * const s_uvp1_set_dbnc = (volatile uint16_t *)0x200004ba;
static volatile uint16_t * const s_uvp1_clr_dbnc = (volatile uint16_t *)0x200004c0;
static volatile uint16_t * const s_uvp2_set_dbnc = (volatile uint16_t *)0x2000058e;
static volatile uint16_t * const s_uvp2_clr_dbnc = (volatile uint16_t *)0x20000590;
static volatile uint16_t * const s_imbal_dbnc    = (volatile uint16_t *)0x200004c6;
static volatile uint16_t * const s_chg_volt_dbnc = (volatile uint16_t *)0x200003cc;
static volatile uint16_t * const s_cocp1_set_dbnc= (volatile uint16_t *)0x200005a2;
static volatile uint16_t * const s_cocp2_set_dbnc= (volatile uint16_t *)0x200004c8;
static volatile uint16_t * const s_cocp1_clr_dbnc= (volatile uint16_t *)0x200004cc;
static volatile uint16_t * const s_cocp2_clr_dbnc= (volatile uint16_t *)0x200004be;
static volatile uint16_t * const s_docp1_set_dbnc= (volatile uint16_t *)0x200004c4;
static volatile uint16_t * const s_docp2_set_dbnc= (volatile uint16_t *)0x2000059e;
static volatile uint16_t * const s_docp1_clr_dbnc= (volatile uint16_t *)0x200004b2;
static volatile uint16_t * const s_docp2_clr_dbnc= (volatile uint16_t *)0x200004b6;
static volatile uint16_t * const s_ts_clr_dbnc   = (volatile uint16_t *)0x2000059c;
static volatile uint16_t * const s_docp_clr_a    = (volatile uint16_t *)0x2000059a;
static volatile uint16_t * const s_docp_clr_b    = (volatile uint16_t *)0x200004c2;

/* FUN_080100a0 — tail tick: IWDG kick (bit2) and the 1 s uptime counters (bit3). */
void tick_uptime(void)
{
    if ((*s_tick & 4) != 0) {
        *s_tick &= (uint8_t)~4u;
        **IWDG_KR_PTR = 0x0000AAAAu;       /* IWDG_KR */
    }
    if ((*s_tick & 8) != 0) {
        *s_tick &= (uint8_t)~8u;
        (*s_evtlog_en)++;
        (*s_uptime_1s)++;
    }
}

/* FUN_08014310 — charger present resets the ship countdown; absent for >99
 * ticks enters shipping mode. */
void charger_shipping_check(void)
{
    volatile uint16_t * const cnt = s_ship_cnt;
    if (*s_chg_voltage > 0x4e1f) {
        *cnt = 0;
    } else {
        uint16_t v = *cnt;
        *cnt = (uint16_t)(v + 1);
        if ((uint16_t)(v + 1) > 99) {
            shipping_enter();
        }
    }
}

/* State 0 / out-of-range default (FUN_08010360) — drive the safe GPIO pattern,
 * reset the AFE and re-init the state machine to state 1. */
void bms_state_idle(void)
{
    gpio_bit_write(GPIOA_BASE, PIN_PA7, 0);     /* PA7 = 0 */
    gpio_bit_write(GPIOB_BASE, PIN_PB0, 1);     /* PB0 = 1 */
    gpio_bit_write(GPIOB_BASE, PIN_PB11, 0);    /* PB11 = 0 */
    bypass_fet_off();
    gpio_bit_write(GPIOB_BASE, PIN_PB9, 0);     /* PB9 = 0 */
    *s_fet_ctrl = 0;
    fedl5236_command_write(9, 0);
    bms_state_enter(1);
}

/* State 5 (FUN_08011480) — idle, no work. */
void bms_state_5(void)
{
}

/* States 23/24/25 (FUN_0800f204) and 26 (FUN_0800a558, identical) — fault hold:
 * run the core, then the slow-tick LED/AFE refresh. */
void bms_state_fault(void)
{
    bms_core_update();
    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
    }
    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }
    tick_uptime();
}

/* State 27 (FUN_08008dc8) — shipping wait: count slow ticks, enter shipping
 * after 99, else refresh the LED bar. */
void bms_state_shipping_wait(void)
{
    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
    }
    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        volatile uint16_t * const cnt = s_ship_cnt;
        uint16_t v = *cnt;
        *cnt = (uint16_t)(v + 1);
        if ((uint16_t)(v + 1) > 99) {
            shipping_enter();
            return;
        }
        extend_io_update();
    }
    tick_uptime();
}

/* ----------------------------------------------------------------------- *
 * bms_core_update — OEM FUN_0800bc18, the central per-tick BMS update run by
 * every operating-state routine. Reads the FEDL5236 status, runs the AFE
 * round-robin cell/charger measurement sub-state machine (sub-state byte
 * 0x2000041a, two 12-entry jump tables), scans the 10 cell voltages
 * (sum/max/min), runs the protection cascade (OVP1/2, UVP1/2, cell-imbalance,
 * charge/discharge-OC, MOS/temperature — debounced into the fault register
 * 0x20000410) and the current + coulomb-counter / RSOC accumulation.
 *
 * Thresholds, debounce limits and fault-bit positions are transcribed verbatim
 * from the OEM. The AFE per-step handlers (jump tables 0x801e598 / 0x801e5c8)
 * and the cell-voltage capture (FUN_0800c6ec) stay forward-declared.
 */
static void bms_afe_cell_step(uint8_t step);     /* cell-read sub-states (jt 0x801e598) */
static void bms_afe_charger_step(uint8_t step);  /* AFE channel-select steps (jt 0x801e5c8) */
void cell_voltage_scan(uint8_t status);          /* FUN_0800c6ec — current/coulomb integrator */
/* FUN_0800d526 is a compiler tail-merged shared function epilogue (mov sp,r7;
 * add sp,#0x4c; pop {r4-r7,pc}) — not a callable routine. Every OEM tail-branch
 * to it (the decompiler renders them as calls) is simply a `return` from the
 * enclosing handler, and is inlined as such below. */
/* The protection cascade + current/coulomb counter + temperature + charger
 * step (OEM 0x0800bd0e..end). Safety-critical: it sets the MOSFET-cutoff fault
 * bits in 0x20000410 from the exact OEM thresholds/debounce limits, with
 * internal tail-calls to the shared temp-step epilogue. Implemented at the
 * bottom of this file; its trailing temp step exits the whole update. */
extern void bms_periodic_update(uint8_t status);

extern const char s_chg_cal_ok[], s_dsg_cal_ok[];

void bms_core_update(void)
{
    volatile uint8_t  * const rx   = s_rx;
    volatile uint16_t * const vmax = s_cell_max;
    volatile uint16_t * const vmin = s_cell_min;
    volatile uint16_t * const vsum = s_cell_sum;
    volatile uint16_t * const cells = s_cell_voltage;
    volatile uint16_t * const mode = s_mode;

    /* --- FEDL5236 status read --- */
    if ((*s_test_mode & 1) == 0 &&
        gpio_bit_read(GPIOC_BASE, PIN_PC13)) {
        return;
    }
    *s_test_mode &= (uint8_t)~1u;
    if (fedl5236_read_data(3, 2) == 0) {
        return;
    }
    uint8_t status = rx[2];
    *s_afe_fault = (uint8_t)(rx[3] & 2);
    if (*s_afe_fault != 0) {
        FAULT |= 0x400;
        fedl5236_command_write(4, 0);
        fedl5236_command_write(3, 0);
        return;
    }
    if ((status & 0xf) != 0) {
        fedl5236_command_write(3, 0);
    }
    if ((status & 1) == 0) {
        cell_voltage_scan(status);
    }

    /* AFE cell-measurement sub-state dispatch (jump table 0x801e598). Sub-states
     * 2..10 each read one cell and return; sub-state 11 reads the last cell then
     * falls through to the full aggregate update; sub-states 0/1 (and any >=0xc)
     * alias straight to the full update — exactly the OEM jump table where
     * jt[0]=jt[1] point at the full-update entry. */
    uint8_t ss = *s_afe_substate;
    if (ss >= 2 && ss < 0xc) {
        bms_afe_cell_step(ss);
        if (ss < 11) {
            return;
        }
    }

    /* --- cell sum / max / min --- */
    int scan = ((*mode & 0x20) != 0) ? (*s_scan_dbnc == 2) : 1;
    if (scan) {
        *vmax = 0;
        *s_cell_max_idx = 0;
        *vmin = 0xffff;
        *s_cell_min_idx = 0;
        *vsum = 0;
        for (uint8_t i = 0; i < 10; i++) {
            uint16_t v = cells[i];
            *vsum = (uint16_t)(v + *vsum);
            if (*vmax < v) { *vmax = v; *s_cell_max_idx = (uint8_t)(i + 1); }
            if (v < *vmin) { *vmin = v; *s_cell_min_idx = (uint8_t)(i + 1); }
        }
    }

    /* cfg max/min trackers (5-tick debounce). */
    if (*(volatile uint16_t *)(CFG + 0x2c) < *vmax) {
        if ((uint8_t)(++*s_cfgmax_dbnc) > 5) {
            *s_cfgmax_dbnc = 0;
            *(volatile uint16_t *)(CFG + 0x2c) = *vmax;
        }
    } else {
        *s_cfgmax_dbnc = 0;
    }
    if (*vmin < *(volatile uint16_t *)(CFG + 0x2e) || *(volatile uint16_t *)(CFG + 0x2e) == 0) {
        if ((uint8_t)(++*s_cfgmin_dbnc) > 5) {
            *s_cfgmin_dbnc = 0;
            *(volatile uint16_t *)(CFG + 0x2e) = *vmin;
        }
    } else {
        *s_cfgmin_dbnc = 0;
    }

    /* Protection cascade + current/coulomb + temperature + charger step.
     * Its internal charger-step early-return ends this update. */
    bms_periodic_update(status);
}

/* ----------------------------------------------------------------------- *
 * bms_temp_step — OEM FUN_0800d178, the shared function epilogue of the BMS
 * core update. The decompiler renders every tail-branch to it inside
 * FUN_0800bc18 as a `FUN_0800d178()` call followed by dead continuation; in
 * reality each is a `b` that RETURNS (proof: the offset-calibration path sets
 * the offset, "calls" it, then zeroes the offset again — only a return makes
 * that consistent). So each call below maps to `bms_temp_step(status); return;`.
 *
 * It reads the AFE temperature channels (charge TS via reg 0x30, discharge TS
 * via reg 0x32, alternating on mode bit 3), applies the per-channel TS
 * calibration offset after a 4-tick warm-up and tracks the temp peak/min into
 * CFG+0x78/+0x79 with a 5-tick debounce; then reads the charger temperature
 * (reg 0x34), scales it (·19536/1000) into the charger round-robin array
 * 0x200003d4 and advances the AFE charger sub-state machine (jump table
 * 0x801e5c8). `status` is the FEDL5236 status byte (rx[2]) read by the caller.
 */
static void bms_temp_step(uint8_t status)
{
    volatile uint8_t  * const rx  = s_rx;
    volatile uint16_t * const mode = s_mode;
    volatile uint8_t  * const ts  = s_ts; /* +1 charge TS, +2 discharge TS */
    volatile uint8_t  * const cnt_max = s_ts_max_dbnc;
    volatile uint8_t  * const cnt_min = s_ts_min_dbnc;

    if ((status & 4) != 0) {
        if ((*mode & 0x8) != 0) {                          /* discharge TS — reg 0x32 */
            if (fedl5236_read_data(0x32, 2) != 0) {
                ts[2] = ntc_temp_read();
                fedl5236_command_write(7, 0);
                *mode &= 0xfff7u;
                *s_offset = 0;
                fedl5236_command_write(6, 0x92);
                if (*s_evtlog_en > 4) {
                    uint8_t cal = *s_ts2_cal;
                    if (cal != 0) {
                        if ((int8_t)cal < 0) {
                            ts[2] = (uint8_t)(ts[2] - (uint8_t)(~cal + 1u));
                        } else {
                            ts[2] = (uint8_t)(cal + ts[2]);
                        }
                    }
                    if (CFG[0x78] < ts[2]) {
                        if (++*cnt_max > 5) { *cnt_max = 0; CFG[0x78] = ts[2]; }
                    }
                    if (ts[2] < CFG[0x79]) {
                        if (++*cnt_min > 5) { *cnt_min = 0; CFG[0x79] = ts[2]; }
                    }
                }
            }
        } else {                                           /* charge TS — reg 0x30 */
            if (fedl5236_read_data(0x30, 2) != 0) {
                ts[1] = ntc_temp_read();
                fedl5236_command_write(7, 0x83);
                *mode |= 0x8u;
                if (*s_evtlog_en > 4) {
                    uint8_t cal = *s_ts1_cal;
                    if (cal != 0) {
                        if ((int8_t)cal < 0) {
                            ts[1] = (uint8_t)(ts[1] - (uint8_t)(~cal + 1u));
                        } else {
                            ts[1] = (uint8_t)(cal + ts[1]);
                        }
                    }
                    if (CFG[0x78] < ts[1]) {
                        if (++*cnt_max > 5) { *cnt_max = 0; CFG[0x78] = ts[1]; }
                    }
                    if (ts[1] < CFG[0x79]) {
                        if (++*cnt_min > 5) { *cnt_min = 0; CFG[0x79] = ts[1]; }
                    }
                }
            }
        }
    }

    if ((status & 8) == 0) {
        return;
    }
    if (fedl5236_read_data(0x34, 2) == 0) {
        return;
    }

    volatile uint8_t * const substate = s_afe_substate;
    uint32_t cv = (19536u * (uint32_t)(rx[2] | (rx[3] << 8))) / 1000u;
    *mode &= 0xfff7u;
    *(volatile uint32_t *)((uint32_t)*substate * 4 + (uintptr_t)CHG_RR) = cv;
    *substate = (uint8_t)(*substate + 1);
    if (*substate < 0xc) {
        bms_afe_charger_step(*substate);
        return;
    }
    fedl5236_command_write(8, 0x91);
}

/* FUN_0800e2d4 — convert the latest AFE register sample (rx[2..3], 12-bit ADC)
 * to millivolts: raw * 5000 / 4095. */
static uint16_t afe_adc_to_mv(void)
{
    volatile uint8_t * const rx = s_rx;
    uint32_t raw = (uint32_t)(rx[2] | (rx[3] << 8));
    return (uint16_t)((5000u * raw) / 4095u);
}

/* bms_afe_cell_step — the cell-measurement sub-state handlers (OEM jump table
 * 0x801e598, cases inside FUN_0800bc18 at 0x0800bcf0..). Sub-state `step`
 * (2..11) reads cell `step-2` from AFE register 0x1a+2*(step-2), captures it
 * (gated by the same scan rule as the aggregate scan), then for steps 2..10
 * advances the sub-state and selects the next AFE channel; step 11 reads the
 * last cell only and the caller falls through to the full update. */
static void bms_afe_cell_step(uint8_t step)
{
    static const uint8_t rd_reg[12] =
        { 0, 0, 0x1a, 0x1c, 0x1e, 0x20, 0x22, 0x24, 0x26, 0x28, 0x2a, 0x2c };
    volatile uint16_t * const cells = s_cell_voltage;
    volatile uint16_t * const mode  = s_mode;

    if (fedl5236_read_data(rd_reg[step], 2) != 0) {
        int scan = ((*mode & 0x20) != 0) ? (*s_scan_dbnc == 2) : 1;
        if (scan) {
            cells[step - 2] = afe_adc_to_mv();
        }
    }
    if (step >= 11) {
        return;                                  /* falls through to the full update */
    }
    (*s_afe_substate)++;
    switch (step) {
    case 2:  fedl5236_command_write(5, 0x85); break;
    case 3:  fedl5236_command_write(5, 0x86); break;
    case 4:  fedl5236_command_write(8, 0x91); break;
    case 5:  fedl5236_command_write(5, 0x88); break;
    case 6:  fedl5236_command_write(5, 0x89); break;
    case 7:  fedl5236_command_write(8, 0x91); break;
    case 8:  fedl5236_command_write(5, 0x8b); break;
    case 9:  fedl5236_command_write(8, 0x91); break;
    case 10: fedl5236_command_write(8, 0x91); break;
    default: break;
    }
}

/* bms_afe_charger_step — the AFE channel-select sub-state handlers (OEM jump
 * table 0x801e5c8, cases at 0x0800d4ae..). Each issues the command that selects
 * the next charger/cell channel for the FEDL5236 to convert, then returns. */
static void bms_afe_charger_step(uint8_t step)
{
    switch (step) {
    case 0:  fedl5236_command_write(8, 0x91); break;
    case 1:  fedl5236_command_write(7, 0x81); break;
    case 2:  fedl5236_command_write(5, 0x84); break;
    case 3:  fedl5236_command_write(5, 0x85); break;
    case 4:  fedl5236_command_write(5, 0x86); break;
    case 5:  fedl5236_command_write(5, 0x87); break;
    case 6:  fedl5236_command_write(5, 0x88); break;
    case 7:  fedl5236_command_write(5, 0x89); break;
    case 8:  fedl5236_command_write(5, 0x8a); break;
    case 9:  fedl5236_command_write(5, 0x8b); break;
    case 10: fedl5236_command_write(5, 0x8c); break;
    case 11: fedl5236_command_write(5, 0x8d); break;
    default: break;
    }
}

/* ----------------------------------------------------------------------- *
 * bms_periodic_update — the tail of OEM FUN_0800bc18 (0x0800bd0e..end): the
 * safety-critical protection cascade, the current → coulomb-counter / RSOC
 * integration, the over-current protection, and the temp/charger epilogue.
 *
 * FAULT register 0x20000410 bit layout (derived from THIS image — note it
 * differs from batteryware's: there bit0=UVP1/bit2=OVP1/bit10=imbalance):
 *   bit0  OVP1   bit1  OVP2   bit2  UVP1   bit3  UVP2
 *   bit4  COCP1  bit5  COCP2  (charge over-current, 1st/2nd level)
 *   bit6  DOCP1  bit7  DOCP2  (discharge over-current, 1st/2nd level)
 *   bit10 TS/temperature      bit11 cell imbalance
 * mode word 0x200006a0: bit14 (DSG-cal accumulate), bit15 (CHG-cal accumulate).
 *
 * Each protection branch debounces a count: the SET branch resets its counter
 * when it fires, the CLEAR branch does not — that asymmetry is the OEM's.
 */
void bms_periodic_update(uint8_t status)
{
    volatile uint16_t * const vmax = s_cell_max;
    volatile uint16_t * const vmin = s_cell_min;
    volatile uint32_t * const dsg_i = s_dsg_i;
    volatile uint32_t * const chg_i = s_chg_i;
    volatile uint16_t * const mode = s_mode;

    /* --- OVP1: max cell > 4249 mV (60-tick set) / < 4150 mV (6-tick clear) -- */
    if (*vmax > 0x1099) {
        if (!(FAULT & 1)) {
            if (++*s_ovp1_set_dbnc > 0x3b) {
                *s_ovp1_set_dbnc = 0;
                FAULT |= 1u;
            }
        }
        *s_ovp1_clr_dbnc = 0;
    } else {
        *s_ovp1_set_dbnc = 0;
        if (FAULT & 1) {
            if (*vmax > 0x1036) {
                *s_ovp1_clr_dbnc = 0;
            } else if (++*s_ovp1_clr_dbnc > 5) {
                FAULT &= 0xfffeu;
            }
        }
    }

    /* --- OVP2: max cell > 4299 mV (6-tick set) / < 4150 mV (6-tick clear) --- */
    if (*vmax > 0x10cb) {
        if (!(FAULT & 2)) {
            if (++*s_ovp2_set_dbnc > 5) {
                *s_ovp2_set_dbnc = 0;
                FAULT |= 2u;
            }
        }
        *s_ovp2_clr_dbnc = 0;
    } else {
        *s_ovp2_set_dbnc = 0;
        if (FAULT & 2) {
            if (*vmax > 0x1036) {
                *s_ovp2_clr_dbnc = 0;
            } else if (++*s_ovp2_clr_dbnc > 5) {
                FAULT &= 0xfffdu;
            }
        }
    }

    /* --- UVP1: min cell <= 3000 mV (60-tick set) / > 3299 mV (6-tick clear) - */
    if (*vmin > 0xbb8) {
        *s_uvp1_set_dbnc = 0;
        if (FAULT & 4) {
            if (*vmin > 0xce3) {
                if (++*s_uvp1_clr_dbnc > 5) {
                    FAULT &= 0xfffbu;
                }
            } else {
                *s_uvp1_clr_dbnc = 0;
            }
        }
    } else {
        if (!(FAULT & 4)) {
            if (++*s_uvp1_set_dbnc > 0x3b) {
                *s_uvp1_set_dbnc = 0;
                FAULT |= 4u;
            }
        }
        *s_uvp1_clr_dbnc = 0;
    }

    /* --- UVP2: min cell < 2801 mV (6-tick set) / > 3299 mV (6-tick clear) --- */
    if (*vmin < 0xaf1) {
        if (!(FAULT & 8)) {
            if (++*s_uvp2_set_dbnc > 5) {
                *s_uvp2_set_dbnc = 0;
                FAULT |= 8u;
            }
        }
        *s_uvp2_clr_dbnc = 0;
    } else {
        *s_uvp2_set_dbnc = 0;
        if (FAULT & 8) {
            if (*vmin > 0xce3) {
                if (++*s_uvp2_clr_dbnc > 5) {
                    FAULT &= 0xfff7u;
                }
            } else {
                *s_uvp2_clr_dbnc = 0;
            }
        }
    }

    /* --- cell imbalance: max>3599 mV and (max-min)>=501 mV, 100-tick (bit11) - */
    if (*vmax > 0xe0f) {
        if ((int)((unsigned)*vmax - (unsigned)*vmin) < 0x1f5) {
            *s_imbal_dbnc = 0;
        } else {
            if (++*s_imbal_dbnc > 99) {
                *s_imbal_dbnc = 0;
                FAULT |= 0x800u;
            }
        }
    } else {
        *s_imbal_dbnc = 0;
    }

    /* --- current-direction clears: hard discharge clears OVP, hard charge UVP */
    if (*dsg_i > 199) {
        FAULT &= 0xfffeu;
        FAULT &= 0xfffdu;
        *s_ovp1_set_dbnc = 0;
        *s_ovp2_set_dbnc = 0;
        *s_ovp1_clr_dbnc = 0;
        *s_ovp2_clr_dbnc = 0;
    }
    if (*chg_i > 199) {
        FAULT &= 0xfffbu;
        FAULT &= 0xfff7u;
        *s_uvp1_set_dbnc = 0;
        *s_uvp2_set_dbnc = 0;
        *s_uvp1_clr_dbnc = 0;
        *s_uvp2_clr_dbnc = 0;
    }

    /* --- charger-voltage detect: max of the 15-slot round-robin array; held
     *     above 19999, dropped to 0 after 900 ticks below it. ------------------ */
    *mode |= 0x10u;
    for (uint8_t i = 0; i < 0xf; i++) {
        if (*(volatile uint32_t *)((uint32_t)i * 4 + (uintptr_t)CHG_RR) > 0x4e1f) {
            *s_chg_voltage = *(volatile uint32_t *)((uint32_t)i * 4 + (uintptr_t)CHG_RR);
            *s_chg_volt_dbnc = 0;
        } else {
            if (++*s_chg_volt_dbnc > 0x383) {
                *s_chg_volt_dbnc = 0;
                *s_chg_voltage = 0;
            }
        }
        *(volatile uint32_t *)((uint32_t)i * 4 + (uintptr_t)CHG_RR) = 0;
    }

    cell_voltage_scan(status);
}

/* ----------------------------------------------------------------------- *
 * cell_voltage_scan — OEM FUN_0800c6ec, the current/coulomb integrator. It is
 * the exact shared tail of the BMS core update (byte-identical to the inline
 * code at FUN_0800bc18 0x0800bd0e..end, same literal pool) and is also called
 * directly from bms_core_update when AFE status bit0 is clear. (The OEM name is
 * a misnomer — it integrates current, it does not scan cell voltages.) Captures
 * the zero-current offset, scales the instantaneous current ((|sample-offset|*
 * 69000)>>16), runs the 4-deep moving average + charge/discharge calibration +
 * 40-sample RSOC accumulation, the charge/discharge over-current protection,
 * then the temperature/charger step. */
void cell_voltage_scan(uint8_t status)
{
    volatile uint8_t  * const rx   = s_rx;
    volatile uint32_t * const dsg_i = s_dsg_i;
    volatile uint32_t * const chg_i = s_chg_i;
    volatile int32_t  * const avg  = s_cur_avg;
    volatile uint32_t * const inst = s_cur_inst;
    volatile int32_t  * const ring = s_cur_ring;
    volatile uint16_t * const mode = s_mode;
    volatile uint16_t * const chgcal = s_chg_cal;
    volatile uint16_t * const dsgcal = s_dsg_cal;
    volatile uint32_t * const rsoc = s_rsoc;

    if (!(status & 2)) {
        bms_temp_step(status);
        return;
    }

    /* --- zero-current offset capture (AFE reg 0x2e). Until the offset is
     *     latched the update tail-returns without integrating current. ------- */
    volatile uint16_t * const offset = s_offset;
    if (*offset == 0) {
        if (fedl5236_read_data(0x2e, 2) != 0) {
            *offset = 0;
            *offset = (uint16_t)(rx[3] + *offset);
            *offset = (uint16_t)(*offset << 8);
            *offset = (uint16_t)(rx[2] + *offset);
            fedl5236_command_write(6, 0x90);
            bms_temp_step(status);
            return;
        }
        *offset = 0;
        fedl5236_command_write(6, 0x92);
        bms_temp_step(status);
        return;
    }
    if (fedl5236_read_data(0x2e, 2) == 0) {
        bms_temp_step(status);
        return;
    }

    /* --- instantaneous current: (|sample-offset| * 69000) >> 16; sign stored
     *     in `inst` (negative=discharge), magnitudes split to the stage cells. */
    *inst = 0;
    *s_chg_mag = 0;
    *s_dsg_mag = 0;
    {
        uint32_t sample = *(volatile uint16_t *)(rx + 2);
        if (*offset < sample) {
            uint32_t diff = sample - *offset;
            if (diff != 0) {
                diff = (uint32_t)(((uint64_t)diff * 69000u) >> 16);
            }
            *inst += diff;
            *s_dsg_mag = *inst;     /* discharge magnitude (positive) */
            *inst = ~*inst;
            *inst += 1;                                    /* negate: discharge < 0 */
        } else {
            uint32_t diff = *offset - sample;
            if (diff != 0) {
                diff = (uint32_t)(((uint64_t)diff * 69000u) >> 16);
            }
            *inst += diff;
            *s_chg_mag = *inst;      /* charge magnitude (positive) */
        }
    }

    /* --- 4-deep moving average of the signed instantaneous current. --------- */
    ring[3] = ring[2];
    ring[2] = ring[1];
    ring[1] = ring[0];
    ring[0] = (int32_t)*inst;
    {
        int32_t isum = 0;
        *avg = 0;
        *chg_i = 0;
        *dsg_i = 0;
        for (uint8_t i = 0; i < 4; i++) {
            isum += ring[i];
        }
        if (isum < 0) {
            isum += 3;
        }
        *avg = isum >> 2;
    }

    if (*avg < 0) {
        /* DISCHARGE branch: magnitude = -avg; CHG-cal applied (OEM crossing). */
        *dsg_i = (uint32_t)*avg;
        *dsg_i = ~*dsg_i;
        *dsg_i += 1;
        if (*chgcal != 0 && !(*mode & 0x8000)) {
            *dsg_i = *dsg_i * (uint32_t)*chgcal;
            *dsg_i = *dsg_i / 1000u;
        }
        if (*(volatile uint16_t *)(CFG + 0x76) < *dsg_i / 10u) {
            if (++*s_dsg_peak_dbnc > 0x13) {
                *s_dsg_peak_dbnc = 0;
                *(volatile uint16_t *)(CFG + 0x76) = (uint16_t)(*dsg_i / 10u);
            }
        } else {
            *s_dsg_peak_dbnc = 0;
        }
        *avg = (int32_t)(~*dsg_i + 1);
        if (*mode & 0x8000) {
            if (++*s_rsoc_cnt < 0x29) {
                *rsoc += *dsg_i;
            } else {
                *chgcal = 0;
                *rsoc *= 0x19;
                *rsoc = *rsoc / *s_rsoc_div;
                if (*rsoc > 0x3e7) {
                    *rsoc += 0xfffffc18u;          /* -1000 */
                    *rsoc = 1000u - *rsoc;
                } else {
                    *rsoc = 1000u - *rsoc;
                    *rsoc += 1000u;
                }
                *chgcal = (uint16_t)((int16_t)*rsoc + *chgcal);
                *(volatile uint16_t *)(CFG + 0x58) = *chgcal;
                *s_rsoc_cnt = 0;
                *mode &= 0x7fffu;
                *dsg_i = *dsg_i * (uint32_t)*chgcal;
                *dsg_i = *dsg_i / 1000u;
                *avg = (int32_t)(~*dsg_i + 1);
                log_print(2, s_dsg_cal_ok);
            }
        }
    } else {
        /* CHARGE branch: magnitude = avg; DSG-cal applied (OEM crossing). */
        *chg_i = (uint32_t)*avg;
        if (*dsgcal != 0 && !(*mode & 0x4000)) {
            *chg_i = *chg_i * (uint32_t)*dsgcal;
            *chg_i = *chg_i / 1000u;
        }
        *avg = (int32_t)*chg_i;
        if (*(volatile uint16_t *)(CFG + 0x74) < *chg_i / 10u) {
            if (++*s_chg_peak_dbnc > 0x13) {
                *(volatile uint16_t *)(CFG + 0x74) = (uint16_t)(*chg_i / 10u);
            }
        } else {
            *s_chg_peak_dbnc = 0;
        }
        if (*chg_i < *s_chg_min &&
            *s_uptime_1s > 0x13) {
            if (++*s_chg_min_dbnc > 0x13) {
                *s_chg_min_dbnc = 0;
                *s_chg_min = *chg_i;
            }
        }
        if (*mode & 0x4000) {
            if (++*s_rsoc_cnt < 0x29) {
                *rsoc += *chg_i;
            } else {
                *dsgcal = 0;
                *rsoc *= 0x19;
                *rsoc = *rsoc / *s_rsoc_div;
                if (*rsoc > 0x3e7) {
                    *rsoc += 0xfffffc18u;          /* -1000 */
                    *rsoc = 1000u - *rsoc;
                } else {
                    *rsoc = 1000u - *rsoc;
                    *rsoc += 1000u;
                }
                *dsgcal = (uint16_t)((int16_t)*rsoc + *dsgcal);
                *(volatile uint16_t *)(CFG + 0x56) = *dsgcal;
                *chgcal = *dsgcal;
                *(volatile uint16_t *)(CFG + 0x58) = *chgcal;
                *s_rsoc_cnt = 0;
                *mode &= 0xbfffu;
                *chg_i = *chg_i * (uint32_t)*dsgcal;
                *chg_i = *chg_i / 1000u;
                *avg = (int32_t)*chg_i;
                log_print(2, s_chg_cal_ok);
            }
        }
    }

    /* --- charge over-current (bits 4/5): set above 4499 / 5999 mA when
     *     charging hard, debounce-cleared (89 ticks, MOS<3) when idle. ------- */
    if (*chg_i < 200) {
        *s_chg_peak_dbnc = 0;
        *s_cocp1_set_dbnc = 0;
        *s_cocp2_set_dbnc = 0;
        if (((FAULT & 0x10) || (FAULT & 0x20)) && *s_mos_state < 3) {
            if (FAULT & 0x10) {
                if (++*s_cocp1_clr_dbnc > 0x59) {
                    *s_cocp1_clr_dbnc = 0;
                    FAULT &= 0xffefu;
                }
            } else {
                *s_cocp1_clr_dbnc = 0;
            }
            if (FAULT & 0x20) {
                if (++*s_cocp2_clr_dbnc > 0x59) {
                    *s_cocp2_clr_dbnc = 0;
                    FAULT &= 0xffdfu;
                }
            } else {
                *s_cocp2_clr_dbnc = 0;
            }
        }
    } else {
        *s_dsg_peak_dbnc = 0;
        *s_docp1_set_dbnc = 0;
        *s_docp2_set_dbnc = 0;
        *s_docp_clr_b = 0;
        *s_docp_clr_a = 0;
        FAULT &= 0xffbfu;
        FAULT &= 0xff7fu;
        FAULT &= 0xfbffu;
        FAULT &= 0xfeffu;
        FAULT &= 0xfdffu;
        if (!(FAULT & 0x10)) {
            if (*chg_i > 0x1193) {
                if (++*s_cocp1_set_dbnc > 0x3b) {
                    *s_cocp1_set_dbnc = 0;
                    FAULT |= 0x10u;
                }
            } else {
                *s_cocp1_set_dbnc = 0;
            }
        }
        if (!(FAULT & 0x20)) {
            if (*chg_i > 0x176f) {
                if (++*s_cocp2_set_dbnc > 5) {
                    *s_cocp2_set_dbnc = 0;
                    FAULT |= 0x20u;
                }
            } else {
                *s_cocp2_set_dbnc = 0;
            }
        }
    }

    /* --- discharge over-current (bits 6/7): set above 7999 / 9999 mA when
     *     discharging hard, debounce-cleared (89 ticks) when idle; bit10 (TS)
     *     also auto-recovers on the idle side. -------------------------------- */
    if (*dsg_i < 200) {
        *s_dsg_peak_dbnc = 0;
        *s_docp1_set_dbnc = 0;
        *s_docp2_set_dbnc = 0;
        *s_docp_clr_b = 0;
        *s_docp_clr_a = 0;
        if (FAULT & 0x40) {
            if (++*s_docp1_clr_dbnc > 0x59) {
                *s_docp1_clr_dbnc = 0;
                FAULT &= 0xffbfu;
            }
        } else {
            *s_docp1_clr_dbnc = 0;
        }
        if (FAULT & 0x80) {
            if (++*s_docp2_clr_dbnc > 0x59) {
                *s_docp2_clr_dbnc = 0;
                FAULT &= 0xff7fu;
            }
        } else {
            *s_docp2_clr_dbnc = 0;
        }
        if (FAULT & 0x400) {
            if (++*s_ts_clr_dbnc > 0x59) {
                *s_ts_clr_dbnc = 0;
                FAULT &= 0xfbffu;
            }
        } else {
            *s_ts_clr_dbnc = 0;
        }
    } else {
        *s_chg_peak_dbnc = 0;
        *s_cocp1_set_dbnc = 0;
        *s_cocp2_set_dbnc = 0;
        FAULT &= 0xffefu;
        FAULT &= 0xffdfu;
        if (!(FAULT & 0x40)) {
            if (*dsg_i > 0x1f3f) {
                if (++*s_docp1_set_dbnc > 0x3b) {
                    *s_docp1_set_dbnc = 0;
                    FAULT |= 0x40u;
                }
            } else {
                *s_docp1_set_dbnc = 0;
            }
        }
        if (!(FAULT & 0x80)) {
            if (*dsg_i > 0x270f) {
                if (++*s_docp2_set_dbnc > 5) {
                    *s_docp2_set_dbnc = 0;
                    FAULT |= 0x80u;
                }
            } else {
                *s_docp2_set_dbnc = 0;
            }
        }
    }

    fedl5236_command_write(8, 0x91);
    bms_temp_step(status);
}
