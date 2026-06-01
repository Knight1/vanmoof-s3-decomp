#include "powerbankware.h"

/*
 * BMS super-loop per-state routines (main's 28-way dispatch on the state byte
 * 0x200005ac). Each is a thin wrapper around the shared BMS core update plus
 * the periodic tick processing keyed off the SysTick flag byte 0x2000077c:
 *   bit0  1 ms      bit1  slower     bit2  IWDG kick     bit3  1 s uptime
 *
 * The heavy shared callees (the BMS core, the AFE status poll, the state-machine
 * (re)init) stay forward-declared until their own passes.
 */

static volatile uint8_t * const s_tick = (volatile uint8_t *)0x2000077c;

extern void FUN_0800bad4(void);   /* FEDL5236 status poll (298 B) */
extern void FUN_0800f7b4(int s);  /* state-machine (re)init */
extern void FUN_08013be4(void);   /* GPIO pulse */

/* FUN_080100a0 — tail tick: IWDG kick (bit2) and the 1 s uptime counters (bit3). */
void tick_uptime(void)
{
    if ((*s_tick & 4) != 0) {
        *s_tick &= (uint8_t)~4u;
        **(volatile uint32_t **)0x200006ac = 0x0000AAAAu;       /* IWDG_KR */
    }
    if ((*s_tick & 8) != 0) {
        *s_tick &= (uint8_t)~8u;
        (*(volatile uint32_t *)0x2000072c)++;
        (*(volatile uint32_t *)0x20000594)++;
    }
}

/* FUN_08014310 — charger present resets the ship countdown; absent for >99
 * ticks enters shipping mode. */
void charger_shipping_check(void)
{
    volatile uint16_t * const cnt = (volatile uint16_t *)0x200006a2;
    if (*(volatile uint32_t *)0x2000042c > 0x4e1f) {
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
    gpio_bit_write(0x48000000, 0x80, 0);     /* PA7 = 0 */
    gpio_bit_write(0x48000400, 1, 1);        /* PB0 = 1 */
    gpio_bit_write(0x48000400, 0x800, 0);    /* PB11 = 0 */
    FUN_08013be4();
    gpio_bit_write(0x48000400, 0x200, 0);    /* PB9 = 0 */
    *(volatile uint8_t *)0x20000412 = 0;
    fedl5236_command_write(9, 0);
    FUN_0800f7b4(1);
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
        FUN_0800bad4();
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
        volatile uint16_t * const cnt = (volatile uint16_t *)0x200006a2;
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
extern void afe_reset_return(void);              /* FUN_0800d526 (empty epilogue) */
/* The protection cascade + current/coulomb counter + temperature + charger
 * step (OEM 0x0800bd0e..end). Safety-critical: it sets the MOSFET-cutoff fault
 * bits in 0x20000410 from the exact OEM thresholds/debounce limits, with
 * internal tail-calls to the shared temp-step epilogue. Implemented at the
 * bottom of this file; its trailing temp step exits the whole update. */
extern void bms_periodic_update(uint8_t status);

extern const char s_chg_cal_ok[], s_dsg_cal_ok[];

#define CFG ((volatile uint8_t *)0x200004d0)
#define FAULT (*(volatile uint16_t *)0x20000410)

void bms_core_update(void)
{
    volatile uint8_t  * const rx   = (volatile uint8_t  *)0x20000614;
    volatile uint16_t * const vmax = (volatile uint16_t *)0x200003a2;
    volatile uint16_t * const vmin = (volatile uint16_t *)0x200003d2;
    volatile uint16_t * const vsum = (volatile uint16_t *)0x200003ce;
    volatile uint16_t * const cells = (volatile uint16_t *)0x20000380;
    volatile uint16_t * const mode = (volatile uint16_t *)0x200006a0;

    /* --- FEDL5236 status read --- */
    if ((*(volatile uint8_t *)0x2000069c & 1) == 0 &&
        gpio_bit_read(0x48000800, 0x2000)) {
        afe_reset_return();
    }
    *(volatile uint8_t *)0x2000069c &= (uint8_t)~1u;
    if (fedl5236_read_data(3, 2) == 0) {
        afe_reset_return();
    }
    uint8_t status = rx[2];
    *(volatile uint8_t *)0x2000041d = (uint8_t)(rx[3] & 2);
    if (*(volatile uint8_t *)0x2000041d != 0) {
        FAULT |= 0x400;
        fedl5236_command_write(4, 0);
        fedl5236_command_write(3, 0);
        afe_reset_return();
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
    uint8_t ss = *(volatile uint8_t *)0x2000041a;
    if (ss >= 2 && ss < 0xc) {
        bms_afe_cell_step(ss);
        if (ss < 11) {
            return;
        }
    }

    /* --- cell sum / max / min --- */
    int scan = ((*mode & 0x20) != 0) ? (*(volatile uint8_t *)0x2000041c == 2) : 1;
    if (scan) {
        *vmax = 0;
        *(volatile uint8_t *)0x20000430 = 0;
        *vmin = 0xffff;
        *(volatile uint8_t *)0x200003c4 = 0;
        *vsum = 0;
        for (uint8_t i = 0; i < 10; i++) {
            uint16_t v = cells[i];
            *vsum = (uint16_t)(v + *vsum);
            if (*vmax < v) { *vmax = v; *(volatile uint8_t *)0x20000430 = (uint8_t)(i + 1); }
            if (v < *vmin) { *vmin = v; *(volatile uint8_t *)0x200003c4 = (uint8_t)(i + 1); }
        }
    }

    /* cfg max/min trackers (5-tick debounce). */
    if (*(volatile uint16_t *)(CFG + 0x2c) < *vmax) {
        if ((uint8_t)(++*(volatile uint8_t *)0x200003a0) > 5) {
            *(volatile uint8_t *)0x200003a0 = 0;
            *(volatile uint16_t *)(CFG + 0x2c) = *vmax;
        }
    } else {
        *(volatile uint8_t *)0x200003a0 = 0;
    }
    if (*vmin < *(volatile uint16_t *)(CFG + 0x2e) || *(volatile uint16_t *)(CFG + 0x2e) == 0) {
        if ((uint8_t)(++*(volatile uint8_t *)0x200003a1) > 5) {
            *(volatile uint8_t *)0x200003a1 = 0;
            *(volatile uint16_t *)(CFG + 0x2e) = *vmin;
        }
    } else {
        *(volatile uint8_t *)0x200003a1 = 0;
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
    volatile uint8_t  * const rx  = (volatile uint8_t  *)0x20000614;
    volatile uint16_t * const mode = (volatile uint16_t *)0x200006a0;
    volatile uint8_t  * const ts  = (volatile uint8_t  *)0x20000218; /* +1 charge TS, +2 discharge TS */
    volatile uint8_t  * const cnt_max = (volatile uint8_t *)0x2000021c;
    volatile uint8_t  * const cnt_min = (volatile uint8_t *)0x20000212;

    if ((status & 4) != 0) {
        if ((*mode & 0x8) != 0) {                          /* discharge TS — reg 0x32 */
            if (fedl5236_read_data(0x32, 2) != 0) {
                ts[2] = ntc_temp_read();
                fedl5236_command_write(7, 0);
                *mode &= 0xfff7u;
                *(volatile uint16_t *)0x20000418 = 0;
                fedl5236_command_write(6, 0x92);
                if (*(volatile uint32_t *)0x2000072c > 4) {
                    uint8_t cal = *(volatile uint8_t *)0x20000205;
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
                if (*(volatile uint32_t *)0x2000072c > 4) {
                    uint8_t cal = *(volatile uint8_t *)0x2000021b;
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
        afe_reset_return();
        return;
    }
    if (fedl5236_read_data(0x34, 2) == 0) {
        afe_reset_return();
        return;
    }

    volatile uint8_t * const substate = (volatile uint8_t *)0x2000041a;
    uint32_t cv = (19536u * (uint32_t)(rx[2] | (rx[3] << 8))) / 1000u;
    *mode &= 0xfff7u;
    *(volatile uint32_t *)((uint32_t)*substate * 4 + 0x200003d4) = cv;
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
    volatile uint8_t * const rx = (volatile uint8_t *)0x20000614;
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
    volatile uint16_t * const cells = (volatile uint16_t *)0x20000380;
    volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;

    if (fedl5236_read_data(rd_reg[step], 2) != 0) {
        int scan = ((*mode & 0x20) != 0) ? (*(volatile uint8_t *)0x2000041c == 2) : 1;
        if (scan) {
            cells[step - 2] = afe_adc_to_mv();
        }
    }
    if (step >= 11) {
        return;                                  /* falls through to the full update */
    }
    (*(volatile uint8_t *)0x2000041a)++;
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
    volatile uint16_t * const vmax = (volatile uint16_t *)0x200003a2;
    volatile uint16_t * const vmin = (volatile uint16_t *)0x200003d2;
    volatile uint32_t * const dsg_i = (volatile uint32_t *)0x20000420;
    volatile uint32_t * const chg_i = (volatile uint32_t *)0x200003a8;
    volatile uint16_t * const mode = (volatile uint16_t *)0x200006a0;

    /* --- OVP1: max cell > 4249 mV (60-tick set) / < 4150 mV (6-tick clear) -- */
    if (*vmax > 0x1099) {
        if (!(FAULT & 1)) {
            if (++*(volatile uint16_t *)0x200005a0 > 0x3b) {
                *(volatile uint16_t *)0x200005a0 = 0;
                FAULT |= 1u;
            }
        }
        *(volatile uint16_t *)0x2000058a = 0;
    } else {
        *(volatile uint16_t *)0x200005a0 = 0;
        if (FAULT & 1) {
            if (*vmax > 0x1036) {
                *(volatile uint16_t *)0x2000058a = 0;
            } else if (++*(volatile uint16_t *)0x2000058a > 5) {
                FAULT &= 0xfffeu;
            }
        }
    }

    /* --- OVP2: max cell > 4299 mV (6-tick set) / < 4150 mV (6-tick clear) --- */
    if (*vmax > 0x10cb) {
        if (!(FAULT & 2)) {
            if (++*(volatile uint16_t *)0x20000586 > 5) {
                *(volatile uint16_t *)0x20000586 = 0;
                FAULT |= 2u;
            }
        }
        *(volatile uint16_t *)0x20000598 = 0;
    } else {
        *(volatile uint16_t *)0x20000586 = 0;
        if (FAULT & 2) {
            if (*vmax > 0x1036) {
                *(volatile uint16_t *)0x20000598 = 0;
            } else if (++*(volatile uint16_t *)0x20000598 > 5) {
                FAULT &= 0xfffdu;
            }
        }
    }

    /* --- UVP1: min cell <= 3000 mV (60-tick set) / > 3299 mV (6-tick clear) - */
    if (*vmin > 0xbb8) {
        *(volatile uint16_t *)0x200004ba = 0;
        if (FAULT & 4) {
            if (*vmin > 0xce3) {
                if (++*(volatile uint16_t *)0x200004c0 > 5) {
                    FAULT &= 0xfffbu;
                }
            } else {
                *(volatile uint16_t *)0x200004c0 = 0;
            }
        }
    } else {
        if (!(FAULT & 4)) {
            if (++*(volatile uint16_t *)0x200004ba > 0x3b) {
                *(volatile uint16_t *)0x200004ba = 0;
                FAULT |= 4u;
            }
        }
        *(volatile uint16_t *)0x200004c0 = 0;
    }

    /* --- UVP2: min cell < 2801 mV (6-tick set) / > 3299 mV (6-tick clear) --- */
    if (*vmin < 0xaf1) {
        if (!(FAULT & 8)) {
            if (++*(volatile uint16_t *)0x2000058e > 5) {
                *(volatile uint16_t *)0x2000058e = 0;
                FAULT |= 8u;
            }
        }
        *(volatile uint16_t *)0x20000590 = 0;
    } else {
        *(volatile uint16_t *)0x2000058e = 0;
        if (FAULT & 8) {
            if (*vmin > 0xce3) {
                if (++*(volatile uint16_t *)0x20000590 > 5) {
                    FAULT &= 0xfff7u;
                }
            } else {
                *(volatile uint16_t *)0x20000590 = 0;
            }
        }
    }

    /* --- cell imbalance: max>3599 mV and (max-min)>=501 mV, 100-tick (bit11) - */
    if (*vmax > 0xe0f) {
        if ((int)((unsigned)*vmax - (unsigned)*vmin) < 0x1f5) {
            *(volatile uint16_t *)0x200004c6 = 0;
        } else {
            if (++*(volatile uint16_t *)0x200004c6 > 99) {
                *(volatile uint16_t *)0x200004c6 = 0;
                FAULT |= 0x800u;
            }
        }
    } else {
        *(volatile uint16_t *)0x200004c6 = 0;
    }

    /* --- current-direction clears: hard discharge clears OVP, hard charge UVP */
    if (*dsg_i > 199) {
        FAULT &= 0xfffeu;
        FAULT &= 0xfffdu;
        *(volatile uint16_t *)0x200005a0 = 0;
        *(volatile uint16_t *)0x20000586 = 0;
        *(volatile uint16_t *)0x2000058a = 0;
        *(volatile uint16_t *)0x20000598 = 0;
    }
    if (*chg_i > 199) {
        FAULT &= 0xfffbu;
        FAULT &= 0xfff7u;
        *(volatile uint16_t *)0x200004ba = 0;
        *(volatile uint16_t *)0x2000058e = 0;
        *(volatile uint16_t *)0x200004c0 = 0;
        *(volatile uint16_t *)0x20000590 = 0;
    }

    /* --- charger-voltage detect: max of the 15-slot round-robin array; held
     *     above 19999, dropped to 0 after 900 ticks below it. ------------------ */
    *mode |= 0x10u;
    for (uint8_t i = 0; i < 0xf; i++) {
        if (*(volatile uint32_t *)((uint32_t)i * 4 + 0x200003d4) > 0x4e1f) {
            *(volatile uint32_t *)0x2000042c = *(volatile uint32_t *)((uint32_t)i * 4 + 0x200003d4);
            *(volatile uint16_t *)0x200003cc = 0;
        } else {
            if (++*(volatile uint16_t *)0x200003cc > 0x383) {
                *(volatile uint16_t *)0x200003cc = 0;
                *(volatile uint32_t *)0x2000042c = 0;
            }
        }
        *(volatile uint32_t *)((uint32_t)i * 4 + 0x200003d4) = 0;
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
    volatile uint8_t  * const rx   = (volatile uint8_t  *)0x20000614;
    volatile uint32_t * const dsg_i = (volatile uint32_t *)0x20000420;
    volatile uint32_t * const chg_i = (volatile uint32_t *)0x200003a8;
    volatile int32_t  * const avg  = (volatile int32_t  *)0x20000424;
    volatile uint32_t * const inst = (volatile uint32_t *)0x2000039c;
    volatile int32_t  * const ring = (volatile int32_t  *)0x200003ac;
    volatile uint16_t * const mode = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const chgcal = (volatile uint16_t *)0x2000023c;
    volatile uint16_t * const dsgcal = (volatile uint16_t *)0x2000023e;
    volatile uint32_t * const rsoc = (volatile uint32_t *)0x20000250;

    if (!(status & 2)) {
        bms_temp_step(status);
        return;
    }

    /* --- zero-current offset capture (AFE reg 0x2e). Until the offset is
     *     latched the update tail-returns without integrating current. ------- */
    volatile uint16_t * const offset = (volatile uint16_t *)0x20000418;
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
    *(volatile uint32_t *)0x20000414 = 0;
    *(volatile uint32_t *)0x200003c0 = 0;
    {
        uint32_t sample = *(volatile uint16_t *)(rx + 2);
        if (*offset < sample) {
            uint32_t diff = sample - *offset;
            if (diff != 0) {
                diff = (uint32_t)(((uint64_t)diff * 69000u) >> 16);
            }
            *inst += diff;
            *(volatile uint32_t *)0x200003c0 = *inst;     /* discharge magnitude (positive) */
            *inst = ~*inst;
            *inst += 1;                                    /* negate: discharge < 0 */
        } else {
            uint32_t diff = *offset - sample;
            if (diff != 0) {
                diff = (uint32_t)(((uint64_t)diff * 69000u) >> 16);
            }
            *inst += diff;
            *(volatile uint32_t *)0x20000414 = *inst;      /* charge magnitude (positive) */
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
            if (++*(volatile uint8_t *)0x2000041e > 0x13) {
                *(volatile uint8_t *)0x2000041e = 0;
                *(volatile uint16_t *)(CFG + 0x76) = (uint16_t)(*dsg_i / 10u);
            }
        } else {
            *(volatile uint8_t *)0x2000041e = 0;
        }
        *avg = (int32_t)(~*dsg_i + 1);
        if (*mode & 0x8000) {
            if (++*(volatile uint8_t *)0x2000022c < 0x29) {
                *rsoc += *dsg_i;
            } else {
                *chgcal = 0;
                *rsoc *= 0x19;
                *rsoc = *rsoc / *(volatile uint32_t *)0x20000228;
                if (*rsoc > 0x3e7) {
                    *rsoc += 0xfffffc18u;          /* -1000 */
                    *rsoc = 1000u - *rsoc;
                } else {
                    *rsoc = 1000u - *rsoc;
                    *rsoc += 1000u;
                }
                *chgcal = (uint16_t)((int16_t)*rsoc + *chgcal);
                *(volatile uint16_t *)(CFG + 0x58) = *chgcal;
                *(volatile uint8_t *)0x2000022c = 0;
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
            if (++*(volatile uint8_t *)0x200003a4 > 0x13) {
                *(volatile uint16_t *)(CFG + 0x74) = (uint16_t)(*chg_i / 10u);
            }
        } else {
            *(volatile uint8_t *)0x200003a4 = 0;
        }
        if (*chg_i < *(volatile uint32_t *)0x200003c8 &&
            *(volatile uint32_t *)0x20000594 > 0x13) {
            if (++*(volatile uint8_t *)0x200003bc > 0x13) {
                *(volatile uint8_t *)0x200003bc = 0;
                *(volatile uint32_t *)0x200003c8 = *chg_i;
            }
        }
        if (*mode & 0x4000) {
            if (++*(volatile uint8_t *)0x2000022c < 0x29) {
                *rsoc += *chg_i;
            } else {
                *dsgcal = 0;
                *rsoc *= 0x19;
                *rsoc = *rsoc / *(volatile uint32_t *)0x20000228;
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
                *(volatile uint8_t *)0x2000022c = 0;
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
        *(volatile uint8_t  *)0x200003a4 = 0;
        *(volatile uint16_t *)0x200005a2 = 0;
        *(volatile uint16_t *)0x200004c8 = 0;
        if (((FAULT & 0x10) || (FAULT & 0x20)) && *(volatile uint8_t *)0x20000554 < 3) {
            if (FAULT & 0x10) {
                if (++*(volatile uint16_t *)0x200004cc > 0x59) {
                    *(volatile uint16_t *)0x200004cc = 0;
                    FAULT &= 0xffefu;
                }
            } else {
                *(volatile uint16_t *)0x200004cc = 0;
            }
            if (FAULT & 0x20) {
                if (++*(volatile uint16_t *)0x200004be > 0x59) {
                    *(volatile uint16_t *)0x200004be = 0;
                    FAULT &= 0xffdfu;
                }
            } else {
                *(volatile uint16_t *)0x200004be = 0;
            }
        }
    } else {
        *(volatile uint8_t  *)0x2000041e = 0;
        *(volatile uint16_t *)0x200004c4 = 0;
        *(volatile uint16_t *)0x2000059e = 0;
        *(volatile uint16_t *)0x200004c2 = 0;
        *(volatile uint16_t *)0x2000059a = 0;
        FAULT &= 0xffbfu;
        FAULT &= 0xff7fu;
        FAULT &= 0xfbffu;
        FAULT &= 0xfeffu;
        FAULT &= 0xfdffu;
        if (!(FAULT & 0x10)) {
            if (*chg_i > 0x1193) {
                if (++*(volatile uint16_t *)0x200005a2 > 0x3b) {
                    *(volatile uint16_t *)0x200005a2 = 0;
                    FAULT |= 0x10u;
                }
            } else {
                *(volatile uint16_t *)0x200005a2 = 0;
            }
        }
        if (!(FAULT & 0x20)) {
            if (*chg_i > 0x176f) {
                if (++*(volatile uint16_t *)0x200004c8 > 5) {
                    *(volatile uint16_t *)0x200004c8 = 0;
                    FAULT |= 0x20u;
                }
            } else {
                *(volatile uint16_t *)0x200004c8 = 0;
            }
        }
    }

    /* --- discharge over-current (bits 6/7): set above 7999 / 9999 mA when
     *     discharging hard, debounce-cleared (89 ticks) when idle; bit10 (TS)
     *     also auto-recovers on the idle side. -------------------------------- */
    if (*dsg_i < 200) {
        *(volatile uint8_t  *)0x2000041e = 0;
        *(volatile uint16_t *)0x200004c4 = 0;
        *(volatile uint16_t *)0x2000059e = 0;
        *(volatile uint16_t *)0x200004c2 = 0;
        *(volatile uint16_t *)0x2000059a = 0;
        if (FAULT & 0x40) {
            if (++*(volatile uint16_t *)0x200004b2 > 0x59) {
                *(volatile uint16_t *)0x200004b2 = 0;
                FAULT &= 0xffbfu;
            }
        } else {
            *(volatile uint16_t *)0x200004b2 = 0;
        }
        if (FAULT & 0x80) {
            if (++*(volatile uint16_t *)0x200004b6 > 0x59) {
                *(volatile uint16_t *)0x200004b6 = 0;
                FAULT &= 0xff7fu;
            }
        } else {
            *(volatile uint16_t *)0x200004b6 = 0;
        }
        if (FAULT & 0x400) {
            if (++*(volatile uint16_t *)0x2000059c > 0x59) {
                *(volatile uint16_t *)0x2000059c = 0;
                FAULT &= 0xfbffu;
            }
        } else {
            *(volatile uint16_t *)0x2000059c = 0;
        }
    } else {
        *(volatile uint8_t  *)0x200003a4 = 0;
        *(volatile uint16_t *)0x200005a2 = 0;
        *(volatile uint16_t *)0x200004c8 = 0;
        FAULT &= 0xffefu;
        FAULT &= 0xffdfu;
        if (!(FAULT & 0x40)) {
            if (*dsg_i > 0x1f3f) {
                if (++*(volatile uint16_t *)0x200004c4 > 0x3b) {
                    *(volatile uint16_t *)0x200004c4 = 0;
                    FAULT |= 0x40u;
                }
            } else {
                *(volatile uint16_t *)0x200004c4 = 0;
            }
        }
        if (!(FAULT & 0x80)) {
            if (*dsg_i > 0x270f) {
                if (++*(volatile uint16_t *)0x2000059e > 5) {
                    *(volatile uint16_t *)0x2000059e = 0;
                    FAULT |= 0x80u;
                }
            } else {
                *(volatile uint16_t *)0x2000059e = 0;
            }
        }
    }

    fedl5236_command_write(8, 0x91);
    bms_temp_step(status);
}
