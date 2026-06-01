#include "powerbankware.h"

/*
 * BMS super-loop per-state routines, states 1..22 (OEM jump table 0x0801E6EC,
 * indexed by the state byte 0x200005AC). Each runs the shared BMS core update
 * then services the SysTick sub-ticks (flag byte 0x2000077c: bit0 1ms, bit1
 * slow, bit2 IWDG-kick, bit3 1s uptime) and decides state transitions. The deep
 * transition / GPIO-sequence callees remain forward-declared (own passes).
 * Translated from this image; structure cross-checked vs batteryware (F0 vs L0
 * addresses differ — never copied).
 */

static volatile uint8_t * const s_tick = (volatile uint8_t *)0x2000077c;
#define CFG   ((volatile uint8_t *)0x200004d0)
#define FAULT (*(volatile uint16_t *)0x20000410)

/* flash log strings (materialised in src/strings.c) */
extern const char s_charger_absent[], s_charger_load_absent[], s_charger_load_exist[], s_charger_voltage_l[], s_dac_over_range[], s_dd_on[], s_no_load_bypass_off[], s_no_load_d[], s_no_load_d2[], s_state6_charge_mode[], s_state6_charger_present[], s_state6_powerdown_a[], s_state6_powerdown_b[], s_state6_progress[], s_vout_under_20v_3s[], s_vout_under_30v_30min[];

/* untranslated transition / sequence callees (forward-declared; own passes) */
void FUN_0800823c(void);

/* State 1 (bms_state_1) — primary operating state: run the BMS core update,
 * bail to the AFE-latch handler on a hard AFE fault, then process the three
 * SysTick sub-ticks. The 1 ms tick (bit0) runs the mode-gated protection
 * dispatch (OVP2/OVP1/UVP2/UVP1/imbalance -> dedicated fault states) and the
 * comm step; the slow tick (bit1) runs the GPIO/IO refresh chain, the host
 * command dispatch (0x200006e4 request bits), the charger-present debounce
 * (-> shipping/boot decision after 20 ticks), and the charger/extend-IO/AFE
 * refresh; bit2 kicks the IWDG and runs the coulomb push; bit3 advances the
 * 1 s uptime counters. Every transition is a tail-branch (call + return). */
void bms_state_1(void)
{
    volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;
    volatile uint16_t * const req   = (volatile uint16_t *)0x200006e4;

    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if ((*mode & 0x10) != 0) {                         /* mode bit4 */
            *mode &= 0xffefu;
            if ((*fault & 2) != 0) {                        /* OVP2 */
                bms_enter_ovp2();
                return;
            }
            if ((*fault & 1) != 0) {                        /* OVP1 */
                bms_enter_ovp1();
                return;
            }
            if ((*fault & 8) != 0) {                        /* UVP2 */
                bms_enter_uvp2();
                return;
            }
            if ((*fault & 4) != 0) {                        /* UVP1 */
                bms_enter_uvp1();
                return;
            }
            if ((*fault & 0x800) != 0) {                    /* cell imbalance */
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        alarm_scan_b2();
        alarm_scan_b3();
        alarm_scan_b4();
        alarm_scan_b5();
        alarm_scan_b6();
        alarm_scan_b7();
        if (*req != 0) {
            if ((*req & 4) != 0) {                          /* req bit2 */
                bms_enter_dotp();
                return;
            }
            if ((*req & 8) != 0) {                          /* req bit3 */
                bms_enter_dutp();
                return;
            }
            if ((*req & 0x10) != 0) {                       /* req bit4 */
                bms_enter_state_16();
                return;
            }
            if ((*req & 0x20) != 0 ||                       /* req bit5/6/7 */
                (*req & 0x40) != 0 ||
                (*req & 0x80) != 0) {
                bms_enter_ov2_record();
                return;
            }
        }
        if (*(volatile uint32_t *)0x2000042c > 0x4e1f) {    /* charger present */
            volatile uint16_t * const cnt = (volatile uint16_t *)0x200006a8;
            uint16_t v = *cnt;
            *cnt = (uint16_t)(v + 1);
            if ((uint16_t)(v + 1) > 0x13) {
                *cnt = 0;
                if ((*(volatile uint32_t *)0x20000724 & 0x1000000u) != 0) {  /* bit24 */
                    boot_enter_operating(0);
                    return;
                }
                if ((*mode & 0x1000) != 0) {                /* mode bit12 */
                    boot_enter_operating(1);
                    return;
                }
                boot_enter_operating(0);
                return;
            }
        } else {
            *(volatile uint16_t *)0x200006a8 = 0;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }

    if ((*s_tick & 4) != 0) {
        *s_tick &= (uint8_t)~4u;
        **(volatile uint32_t **)0x200006ac = 0x0000AAAAu;   /* IWDG_KR */
        coulomb_integrate(*(volatile uint32_t *)0x20000424);
    }

    if ((*s_tick & 8) != 0) {
        *s_tick &= (uint8_t)~8u;
        (*(volatile uint32_t *)0x2000072c)++;
        (*(volatile uint32_t *)0x20000594)++;
    }
}

/* State 2 (bms_state_2 @ 0x08009600, 1044 B) — the normal-operation /
 * discharge super-loop state. Runs the shared BMS core update; if the AFE
 * raised the latch fault (0x2000041d) it tail-branches to the fault-entry
 * routine. On the 1 ms tick it services the mode-word fault dispatch
 * (0x200006a0 bit4 gate -> fault register 0x20000410 OVP/OVP-class/DOCP/
 * imbalance branches), each a tail transition. On the slow tick it runs the
 * I/O refresh chain, the secondary fault-byte dispatch (0x200006e4), and the
 * charge/discharge cut-off + charger-detect / boot-mode logic keyed on the
 * pack-voltage sum (0x200003ce). The IWDG-kick (bit2) and 1 s uptime (bit3)
 * tail ticks close it out. */
void bms_state_2(void)
{
    volatile uint16_t * const mode   = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const fault  = (volatile uint16_t *)0x20000410;
    volatile uint8_t  * const fault2 = (volatile uint8_t  *)0x200006e4;
    volatile uint16_t * const vsum   = (volatile uint16_t *)0x200003ce;

    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if ((*mode & (1u << 4)) != 0) {                  /* mode bit4 gate */
            *mode &= 0xffefu;
            if ((*fault & (1u << 1)) != 0) {             /* OVP2 */
                bms_enter_ovp2();
                return;
            }
            if ((*fault & 1) != 0) {                     /* OVP1 */
                bms_enter_ovp1();
                return;
            }
            if ((*fault & (1u << 4)) != 0) {             /* COCP1 */
                bms_enter_state_b();
                return;
            }
            if ((*fault & (1u << 5)) != 0) {             /* COCP2 */
                bms_enter_state_c();
                return;
            }
            if ((*fault & (1u << 11)) != 0) {            /* cell imbalance */
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        alarm_scan_b0();
        alarm_scan_b1();
        alarm_scan_b4();
        alarm_scan_b5();
        if (*fault2 != 0) {
            if ((*fault2 & 1) != 0) {                    /* bit0 */
                bms_enter_cotp();
                return;
            }
            if ((*fault2 & (1u << 1)) != 0) {            /* bit1 */
                bms_enter_cutp();
                return;
            }
            if ((*fault2 & (1u << 4)) != 0) {            /* bit4 */
                bms_enter_state_16();
                return;
            }
            if ((*fault2 & (1u << 5)) != 0 ||            /* bit5 */
                (*fault2 & (1u << 6)) != 0 ||            /* bit6 */
                (*fault2 & (1u << 7)) != 0) {            /* bit7 */
                bms_enter_ov2_record();
                return;
            }
        }

        if (*vsum > 0x752f) {                            /* pack sum above the discharge floor */
            if (*(volatile uint8_t *)0x20000412 == 2 &&
                *(volatile uint8_t *)0x20000220 != 0) {
                (*(volatile uint8_t *)0x20000220)--;
            } else if (*(volatile uint32_t *)0x200003a8 < 200) {   /* not charging hard */
                gpio_bit_write(0x48000400, 0x200, 0);   /* PB9 = 0 */
                if (*(volatile uint32_t *)0x2000042c > 0x4e1f) {    /* charger present */
                    *(volatile uint16_t *)0x200006a2 = 0;
                    *(volatile uint16_t *)0x200006a8 = 0;
                    if ((*mode & (1u << 12)) != 0) {     /* mode bit12 */
                        if (*(volatile uint16_t *)0x200001ae < 300) {
                            (*(volatile uint16_t *)0x2000069e)++;
                            if (*(volatile uint16_t *)0x2000069e % 0x14 == 0) {
                                log_print(2, s_no_load_d2,
                                          (*(volatile uint16_t *)0x2000069e) / 0x14);
                            }
                            if (*(volatile uint16_t *)0x2000069e > 199) {
                                *(volatile uint16_t *)0x2000069e = 0;
                                *mode &= 0xefffu;        /* clear mode bit12 */
                                log_print(2, s_no_load_bypass_off);
                                boot_mode_enter(2);
                                return;
                            }
                        } else {
                            *(volatile uint16_t *)0x2000069e = 0;
                        }
                    } else {
                        *(volatile uint16_t *)0x2000069e = 0;
                    }
                } else {                                  /* charger absent */
                    *(volatile uint16_t *)0x2000069e = 0;
                    uint16_t v = *(volatile uint16_t *)0x200006a8;
                    *(volatile uint16_t *)0x200006a8 = (uint16_t)(v + 1);
                    if ((uint16_t)(v + 1) > 0x3b) {
                        *(volatile uint16_t *)0x200006a8 = 0;
                        log_print(2, s_charger_absent);
                        boot_mode_enter(1);
                        return;
                    }
                }
            } else {                                      /* charging hard */
                gpio_bit_write(0x48000400, 0x200, 1);    /* PB9 = 1 */
                *(volatile uint16_t *)0x200006a2 = 0;
                *(volatile uint16_t *)0x200006a8 = 0;
                *(volatile uint16_t *)0x2000069e = 0;
            }

            if (gpio_bit_read(0x48000400, 0x100) == 0) { /* PB8 low */
                if ((*mode & (1u << 8)) != 0) {          /* mode bit8 */
                    *(volatile uint8_t *)0x20000710 = 0;
                } else {
                    uint8_t b = *(volatile uint8_t *)0x20000710;
                    *(volatile uint8_t *)0x20000710 = (uint8_t)(b + 1);
                    if ((uint8_t)(b + 1) > 3) {
                        *(volatile uint8_t *)0x20000710 = 0;
                        *mode |= 0x100u;
                        boot_mode_enter(3);
                        return;
                    }
                }
            } else if ((*mode & (1u << 8)) != 0) {       /* PB8 high, mode bit8 */
                uint8_t b = *(volatile uint8_t *)0x20000710;
                *(volatile uint8_t *)0x20000710 = (uint8_t)(b + 1);
                if ((uint8_t)(b + 1) > 3) {
                    *(volatile uint8_t *)0x20000710 = 0;
                    *mode &= 0xfeffu;
                    *mode &= 0xf7ffu;
                }
            } else {
                *(volatile uint8_t *)0x20000710 = 0;
            }
        } else {                                          /* pack sum at/below the floor */
            if ((*(volatile uint8_t *)0x20000412 & 2) != 2) {
                *(volatile uint8_t *)0x20000412 = 2;
                fedl5236_command_write(9, *(volatile uint8_t *)0x20000412);
            }
            *(volatile uint8_t *)0x20000220 = 0;
            gpio_bit_write(0x48000400, 0x200, 0);        /* PB9 = 0 */
        }

        if (*vsum > 0x752f || *(volatile uint32_t *)0x200003a8 < 0x33) {
            if (*vsum > 0x752f || *(volatile uint32_t *)0x200003a8 > 0x31) {
                charger_shipping_check();
            } else {
                charger_shipping_check();
            }
        }
        cell_balance_update();
        extend_io_update();
        charger_afe_refresh();
    }

    if ((*s_tick & 4) != 0) {
        *s_tick &= (uint8_t)~4u;
        **(volatile uint32_t **)0x200006ac = 0x0000AAAAu;   /* IWDG_KR */
        if ((*mode & (1u << 12)) == 0) {                    /* mode bit12 clear */
            coulomb_integrate(*(volatile uint32_t *)0x20000424);
        }
    }

    if ((*s_tick & 8) != 0) {
        *s_tick &= (uint8_t)~8u;
        (*(volatile uint32_t *)0x2000072c)++;
        (*(volatile uint32_t *)0x20000594)++;
    }
}

/* State 3 (bms_state_3) — the active "charger present / bypass / output" operating
 * state. Runs the BMS core every loop, then three tick-gated phases off the SysTick
 * flag byte 0x2000077c:
 *   bit0 1ms  — mode-bit4 (gated) fault refresh: re-issue the latched protection
 *               handlers (fault bits 3/2/6/7/11), then bms_measure_update.
 *   bit1 slow — the bypass / no-load / DD-on / output-enable logic: protection-mask
 *               (0x200006e4) dispatch, charger-voltage timeout (>10s) -> boot mode,
 *               20-tick "No Load" report, PB8 (0x100) edge-debounced output enable
 *               (mode bit8), charger/extend IO refresh, DD-on pulse (mode bit10),
 *               shipping entry when CFG+0x5a == 0.
 *   bit2 IWDG — kick IWDG_KR (=0xAAAA) + run coulomb_integrate(current-avg @0x20000424).
 *   bit3 1s   — uptime counters; >=180s timeout -> boot mode 1; then the DAC/Vout
 *               regulation cascade keyed on charger voltage (0x20000202), pack voltage
 *               (0x200001ae), TS temps (0x20000218[0..2]) and mode bits 6/7 (charge/
 *               discharge allowed) -> vout_bypass_off/vout_set_dac(target)/vout_enable,
 *               plus the <20V/3s and <30V/30min shutdown timers (-> shipping_enter).
 *
 * Bit-test idiom (uint)X<<K < 0 tests bit (31-K); translated to direct masks below.
 * bms_state_noop here is a plain epilogue helper (uart/LED flush) — only the variants
 * IMMEDIATELY followed by `return;` are tail-transitions; the others fall through.
 */
void bms_state_3(void)
{
    volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;
    volatile uint32_t * const ext   = (volatile uint32_t *)0x200006e4;  /* protection-request mask */

    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        bms_state_noop();
    }

    /* --- bit0: fault-handler refresh, gated on mode bit4 --- */
    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if ((*mode & (1u << 4)) != 0) {
            *mode &= 0xffefu;
            if ((*fault & (1u << 3)) != 0) {
                bms_enter_uvp2();
                bms_state_noop();
            }
            if ((*fault & (1u << 2)) != 0) {
                bms_enter_uvp1();
                bms_state_noop();
            }
            if ((*fault & (1u << 6)) != 0) {
                bms_enter_state_d();
                bms_state_noop();
            }
            if ((*fault & (1u << 7)) != 0) {
                bms_enter_state_e();
                bms_state_noop();
            }
            if ((*fault & (1u << 11)) != 0) {
                bms_enter_ov2_record();
                bms_state_noop();
            }
        }
        bms_measure_update();
    }

    /* --- bit1: bypass / output / DD-on slow logic --- */
    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        alarm_scan_b0();
        alarm_scan_b1();
        alarm_scan_b2();
        alarm_scan_b3();
        alarm_scan_b4();
        alarm_scan_b5();
        if (*ext != 0) {
            if ((*ext & (1u << 2)) != 0) {
                bms_enter_dotp();
                bms_state_noop();
            }
            if ((*ext & (1u << 3)) != 0) {
                bms_enter_dutp();
                bms_state_noop();
            }
            if ((*ext & (1u << 4)) != 0) {
                bms_enter_state_16();
                bms_state_noop();
            }
            if ((*ext & (1u << 5)) != 0 || (*ext & (1u << 6)) != 0 || (*ext & (1u << 7)) != 0) {
                bms_enter_ov2_record();
                bms_state_noop();
                return;
            }
        }

        /* charger present (>20000 mV) resets the absent counter; absent for 40
         * slow ticks -> log & enter boot mode (2 if pack<200 mV, else 3). */
        if (*(volatile uint32_t *)0x2000042c > 0x4e1f) {
            volatile uint16_t * const cnt = (volatile uint16_t *)0x200006a8;
            uint16_t v = *cnt;
            *cnt = (uint16_t)(v + 1);
            *(volatile uint16_t *)0x200006a2 = 0;
            if ((uint16_t)(v + 1) > 0x27) {
                *cnt = 0;
                log_print(2, s_charger_voltage_l, *(volatile uint32_t *)0x2000042c);
                if (*(volatile uint16_t *)0x200001ae < 200) {
                    log_print(2, s_charger_load_absent);
                    boot_mode_enter(2);
                    bms_state_noop();
                    return;
                }
                log_print(2, s_charger_load_exist);
                boot_mode_enter(3);
                bms_state_noop();
                return;
            }
        } else {
            *(volatile uint16_t *)0x200006a8 = 0;
        }

        /* "No Load" report every 20 of the discharge-idle slow ticks. */
        if (*(volatile uint32_t *)0x20000420 < 200) {
            volatile uint16_t * const noload = (volatile uint16_t *)0x200006a2;
            /* FUN_0800823c is the divmod helper; only its remainder is used. */
            if ((uint16_t)(*noload % 0x14) == 0 && *noload > 0x13) {
                log_print(2, s_no_load_d, *noload / 0x14);
            }
        } else {
            *(volatile uint16_t *)0x200006a2 = 0;
        }

        /* PB8 (0x100) edge-debounced output-enable (mode bit8). */
        if (gpio_bit_read(0x48000400, 0x100) == 0) {
            if ((*mode & (1u << 8)) != 0) {
                *(volatile uint8_t *)0x20000710 = 0;
            } else {
                uint8_t b = *(volatile uint8_t *)0x20000710;
                *(volatile uint8_t *)0x20000710 = (uint8_t)(b + 1);
                if ((uint8_t)(b + 1) > 3) {
                    *(volatile uint8_t *)0x20000710 = 0;
                    *mode |= 0x100u;
                    boot_mode_enter(1);
                    bms_state_noop();
                    return;
                }
            }
        } else if ((*mode & (1u << 8)) != 0) {
            uint8_t b = *(volatile uint8_t *)0x20000710;
            *(volatile uint8_t *)0x20000710 = (uint8_t)(b + 1);
            if ((uint8_t)(b + 1) > 3) {
                *(volatile uint8_t *)0x20000710 = 0;
                *mode &= 0xfeffu;
                *mode &= 0xf7ffu;
            }
        } else {
            *(volatile uint8_t *)0x20000710 = 0;
        }

        charger_shipping_check();
        extend_io_update();

        /* DD-on: when mode bit10 is set, after two slow ticks drop PB0 / raise
         * PB11, pulse, report, and reset the no-load counter. */
        if ((*mode & (1u << 10)) != 0) {
            int8_t c = (int8_t)*(volatile uint8_t *)0x20000720;
            *(volatile uint8_t *)0x20000720 = (uint8_t)(c + 1);
            if ((int8_t)(c + 1) == 2) {
                *(volatile uint8_t *)0x20000720 = 0;
                gpio_bit_write(0x48000400, 1, 0);       /* PB0 = 0 */
                gpio_bit_write(0x48000400, 0x800, 1);   /* PB11 = 1 */
                bypass_fet_off();
                log_print(2, s_dd_on);
                *(volatile uint16_t *)0x200006a2 = 0;
            }
        }

        /* shipping when CFG+0x5a config byte is clear. */
        if (*(volatile int8_t *)(0x200004d0 + 0x5a) == 0) {
            *(volatile uint32_t *)0x20000724 |= 0x1000000u;   /* mode bit24 */
            shipping_enter();
            bms_state_noop();
            return;
        }
        charger_afe_refresh();
    }

    /* --- bit2: IWDG kick + current-average service --- */
    if ((*s_tick & 4) != 0) {
        *s_tick &= (uint8_t)~4u;
        **(volatile uint32_t **)0x200006ac = 0x0000AAAAu;       /* IWDG_KR */
        coulomb_integrate(*(volatile uint32_t *)0x20000424);
    }

    /* --- bit3: 1 s uptime + DAC/Vout regulation cascade --- */
    if ((*s_tick & 8) == 0) {
        bms_state_noop();
        return;
    }
    *s_tick &= (uint8_t)~8u;
    (*(volatile uint32_t *)0x2000072c)++;
    {
        uint32_t up = *(volatile uint32_t *)0x20000594;
        *(volatile uint32_t *)0x20000594 = up + 1;
        if (up + 1 > 0xb3) {                          /* >= 180 s -> boot mode 1 */
            boot_mode_enter(1);
            bms_state_noop();
            return;
        }
    }

    volatile uint16_t * const pv = (volatile uint16_t *)0x200001ae;  /* pack voltage */
    volatile uint8_t  * const ts = (volatile uint8_t  *)0x20000218;  /* [0]=mos [1]=chg [2]=dsg TS */

    if (*(volatile uint32_t *)0x20000202 > 0x752f) {  /* charger > 30000 mV */
        if (ts[0] < 0x79 && ts[1] < 0x6d && ts[2] < 0x6d) {
            if (ts[0] < 0x73 && ts[1] < 0x69 && ts[2] < 0x69) {
                *(volatile uint16_t *)0x200006bc = 0;
                vout_bypass_off();
                bms_state_noop();
                return;
            }
            if ((*mode & (1u << 6)) != 0) {                  /* charge allowed */
                if (*pv > 0xbb8) {                           /* > 3000 mV */
                    if ((*mode & (1u << 7)) == 0) {          /* discharge not active */
                        uint16_t dac = *(volatile uint16_t *)(0x20000254 + 2);
                        if (dac > 0xce3) {
                            vout_bypass_off();
                            *mode |= 0x80u;
                            log_print(2, s_dac_over_range);
                            bms_state_noop();
                            return;
                        }
                        if ((int)(dac + 5) > 0xce4) {
                            vout_set_dac(0xce4);
                            bms_state_noop();
                            return;
                        }
                        vout_set_dac(dac + 5);
                        bms_state_noop();
                        return;
                    }
                } else if (*pv <= 0xaef) {                   /* <= 2799 mV */
                    uint16_t dac = *(volatile uint16_t *)(0x20000254 + 2);
                    if (dac == 0) {
                        vout_bypass_off();
                        bms_state_noop();
                        return;
                    }
                    if (dac < 5) {
                        vout_set_dac(0);
                        bms_state_noop();
                        return;
                    }
                    vout_set_dac(dac - 5);
                    bms_state_noop();
                    return;
                }
            }
        } else if (*pv > 0xbb8) {
            if ((*mode & (1u << 7)) == 0) {                  /* discharge not active */
                if ((*mode & (1u << 6)) != 0) {              /* charge allowed */
                    uint16_t dac = *(volatile uint16_t *)(0x20000254 + 2);
                    if (dac > 0xce3) {
                        vout_bypass_off();
                        *mode |= 0x80u;
                        log_print(2, s_dac_over_range);
                    } else if ((int)(dac + 5) > 0xce4) {
                        vout_set_dac(0xce4);
                    } else {
                        vout_set_dac(dac + 5);
                    }
                } else {
                    vout_enable();
                    vout_set_dac(0x5b4);
                }
            }
        } else if ((*mode & (1u << 6)) != 0 && *pv <= 0xaef) {
            uint16_t dac = *(volatile uint16_t *)(0x20000254 + 2);
            if (dac == 0) {
                vout_bypass_off();
            } else if (dac < 5) {
                vout_set_dac(0);
            } else {
                vout_set_dac(dac - 5);
            }
        }
        return;
    }

    if (*(volatile uint32_t *)0x20000202 > 0x4e1f) {  /* charger > 20000 mV */
        if (*pv < 0x4b1) {                            /* < 1201 mV */
            if ((*mode & (1u << 6)) != 0 && *pv <= 0x31f) {   /* charge allowed & <= 799 mV */
                uint16_t dac = *(volatile uint16_t *)(0x20000254 + 2);
                if (dac == 0) {
                    vout_bypass_off();
                } else if (dac < 5) {
                    vout_set_dac(0);
                } else {
                    vout_set_dac(dac - 5);
                }
            }
        } else if ((*mode & (1u << 7)) == 0) {        /* discharge not active */
            if ((*mode & (1u << 6)) != 0) {           /* charge allowed */
                uint16_t dac = *(volatile uint16_t *)(0x20000254 + 2);
                if (dac == 0x5b4) {
                    vout_set_dac(0x5dc);
                } else if (dac > 0xce3) {
                    vout_bypass_off();
                    *mode |= 0x80u;
                    log_print(2, s_dac_over_range);
                } else if ((int)(dac + 5) > 0xce4) {
                    vout_set_dac(0xce4);
                } else {
                    vout_set_dac(dac + 5);
                }
            } else {
                vout_enable();
                vout_set_dac(0x5b4);
            }
        }

        /* <30V-for-30min shutdown timer (0x707 = 1799 ticks). */
        {
            uint16_t v = *(volatile uint16_t *)0x200006bc;
            *(volatile uint16_t *)0x200006bc = (uint16_t)(v + 1);
            if ((uint16_t)(v + 1) > 0x707) {
                *(volatile uint16_t *)0x200006bc = 0;
                if (*(volatile uint32_t *)0x2000042c <= 0x4e1f) {
                    log_print(2, s_vout_under_30v_30min);
                    shipping_enter();
                    bms_state_noop();
                    return;
                }
                bms_state_noop();
                return;
            }
        }
        bms_state_noop();
        return;
    }

    /* charger <= 20000 mV: <20V-for-3s shutdown timer. */
    {
        uint16_t v = *(volatile uint16_t *)0x200006bc;
        *(volatile uint16_t *)0x200006bc = (uint16_t)(v + 1);
        if ((uint16_t)(v + 1) < 3) {
            bms_state_noop();
            return;
        }
    }
    *(volatile uint16_t *)0x200006bc = 0;
    if (*(volatile uint32_t *)0x2000042c <= 0x4e1f) {
        log_print(2, s_vout_under_20v_3s);
        shipping_enter();
        bms_state_noop();
        return;
    }
    bms_state_noop();
    return;
}

/* State 4 (bms_state_4) — the main "discharging / load-attached" operating
 * state. Runs the BMS core, then splits on the SysTick flag byte:
 *   bit0 (1 ms) set  -> the discharge MOS retry/timeout path (status_frame_emit
 *                       precharge pulse, bms_measure_update service, and the
 *                       50-sample "load gone" poll that exits to idle/charge);
 *   bit1 (slow)  set -> the load-detect / charger-attach state machine that
 *                       transitions to states 2/3/5 and toggles the PB-bank
 *                       enable/disable pins via extend_io_update().
 * Either way it tail-calls tick_uptime() (IWDG kick + 1 s counters).
 *
 * Bit positions recovered from the OEM shifts: mode word 0x200006a0 uses
 * bit11 (<<0x14), bit8 (<<0x17), bit4 (<<0x1b); flag byte 0x20000484 uses
 * bit1 (<<0x1e) and bit3 (<<0x1c); the cell-balance word 0x20000724 uses
 * bit24 (native <<7). FAULT-class byte 0x200004b4 selects the next state
 * (2 for charge-fault classes, 3 for the recoverable class). */
void bms_state_4(void)
{
    volatile uint16_t * const mode = (volatile uint16_t *)0x200006a0;
    volatile uint8_t  * const cls  = (volatile uint8_t  *)0x200004b4; /* fault class */
    volatile uint8_t  * const state = (volatile uint8_t  *)0x200005ac;
    volatile uint8_t  * const retry = (volatile uint8_t  *)0x20000710;
    volatile uint8_t  * const flags = (volatile uint8_t  *)0x20000484;
    volatile int16_t  * const tmr   = (volatile int16_t  *)0x20000486;

    bms_core_update();

    if ((*s_tick & 1) == 0) {
slow_tick:
        if (!(*s_tick & 2)) {
            goto tail;
        }
        *s_tick &= (uint8_t)~2u;

        if (!(*mode & (1u << 11))) {          /* OEM: -1 < (mode<<0x14), i.e. bit11 CLEAR */
            *mode &= 0xefffu;                 /* clear bit12 */
            uint8_t c = *cls;
            if (c == 2) {
                *state = 2;
            } else if (c == 3) {
                *state = 2;
            } else if (c == 1) {
                *state = 3;
            }
            extend_io_update();
            *state = 4;
        }

        if (gpio_bit_read(0x48000400, 0x100) == 0) {
            if (*mode & (1u << 8)) {
                *retry = 0;
            } else {
                uint8_t v = *retry;
                *retry = (uint8_t)(v + 1);
                if ((uint8_t)(v + 1) > 1) {
                    *retry = 0;
                    *mode |= 0x100u;          /* set bit8 */
                    *mode &= 0xf7ffu;         /* clear bit11 */
                    *(volatile uint8_t *)0x200006e5 = 0xff;
                    *mode &= 0xefffu;         /* clear bit12 */
                    *state = 5;
                    extend_io_update();
                    *state = 4;
                    *mode |= 0x800u;          /* set bit11 */
                }
            }
        } else if (*mode & (1u << 8)) {
            uint8_t v = *retry;
            *retry = (uint8_t)(v + 1);
            if ((uint8_t)(v + 1) > 1) {
                *retry = 0;
                *mode &= 0xf7ffu;             /* clear bit11 */
                *mode &= 0xfeffu;             /* clear bit8 */
                *(volatile uint32_t *)0x20000728 = 0;
                *(volatile uint8_t *)0x200006e5 = 0xff;
                *mode &= 0xefffu;             /* clear bit12 */
                uint8_t c = *cls;
                if (c == 2) {
                    *state = 2;
                } else if (c == 3) {
                    *state = 2;
                } else if (c == 1) {
                    *state = 3;
                }
                extend_io_update();
                *state = 4;
            }
        } else {
            *retry = 0;
            if (*mode & (1u << 11)) {
                *mode &= 0xf7ffu;             /* clear bit11 */
            }
        }

        if ((*flags & 1) == 0 ||
            *(volatile uint32_t *)0x2000072c == 0 ||
            *(volatile uint16_t *)0x20000202 > 499) {
            goto tail;
        }
        *flags &= (uint8_t)~1u;
        status_frame_emit();
        if (*(volatile uint32_t *)0x2000072c > 4) {
            goto charge_off;
        }
        gpio_bit_write(0x48000000, 0x200, 1);
        *tmr = 0x65;
        *flags |= 2u;
    } else {
        *s_tick &= (uint8_t)~1u;
        if (*mode & (1u << 4)) {
            *mode &= 0xffefu;                 /* clear bit4 */
        }
        if ((*flags & 1) != 0) {
            bms_measure_update();
        }

        /* OEM (0x0800ed00..): the timeout/MOS-retry chain. The decompiler
         * renders each guard as `(-1 < (flags<<k)) || (--tmr != 0)` — i.e.
         * "the gate bit is CLEAR, OR the predecremented timer is still
         * non-zero". Bit1 (<<0x1e) gates the outer level, bit3 (<<0x1c) the
         * inner; each level predecrements the shared 16-bit timer 0x20000486
         * exactly once. Expanded with the side effects preserved: */
        {
            int enter_outer;            /* (flags bit1 CLEAR) || (--tmr != 0) */
            if ((*flags & 2) == 0) {
                enter_outer = 1;
            } else {
                int16_t t = (int16_t)(*tmr - 1);
                *tmr = t;
                enter_outer = (t != 0);
            }
            if (enter_outer) {
                int goto_slow;          /* (flags bit3 CLEAR) || (--tmr != 0) */
                if ((*flags & 8) == 0) {
                    goto_slow = 1;
                } else {
                    int16_t t = (int16_t)(*tmr - 1);
                    *tmr = t;
                    goto_slow = (t != 0);
                }
                if (goto_slow) {
                    goto slow_tick;
                }
                *flags &= (uint8_t)~8u;
                uint8_t rc = *(volatile uint8_t *)0x20000485;
                *(volatile uint8_t *)0x20000485 = (uint8_t)(rc - 1);
                if ((uint8_t)(rc - 1) == 0) {
                    uint8_t streak = 0;
                    do {
                        while ((*s_tick & 1) == 0) {
                        }
                        *s_tick &= (uint8_t)~1u;
                        if (gpio_bit_read(0x48000400, 0x100) == 1) {
                            streak = (uint8_t)(streak + 1);
                        } else {
                            streak = 0;
                        }
                    } while (streak < 0x32);
                    *mode &= 0xf7ffu;         /* clear bit11 */
                    gpio_bit_write(0x48000000, 0x80, 0);
                    if (*(volatile uint16_t *)0x200003ce <= 0x752f) {
                        boot_enter_operating(0);
                        return;
                    }
                    if (*(volatile uint8_t *)0x200004b4 != 1) {
                        if (*(volatile uint8_t *)0x200004b4 == 3) {
                            boot_enter_operating(1);
                            return;
                        }
                        boot_enter_operating(0);
                        return;
                    }
                    if (*(volatile uint32_t *)0x20000724 & (1u << 24)) {
                        bms_state_idle();
                        return;
                    }
                    bms_enter_idle3();
                    return;
                }
                status_frame_emit();
                *(volatile uint8_t *)0x20000489 = 0;
            }
        }
charge_off:
        gpio_bit_write(0x48000000, 0x200, 0);
        timer_start_it((uint32_t *)0x20000738);
        *flags &= (uint8_t)~2u;
        *(volatile uint8_t *)0x20000488 = 1;
        *tmr = 4;
    }
    gpio_bit_write(0x48000000, 0x80, 1);
tail:
    tick_uptime();
}

/* State 6 (bms_state_6) — the charge / boot-window operating state. Runs the
 * shared BMS core, then on the slow tick (bit1) services the charge button line
 * (PB8, GPIOB 0x100): debounced (>3 ticks) it enters boot mode 1; a charger
 * voltage above 19999 (0x4e1f at 0x2000042c) enters boot mode 3. Once the
 * uptime counter (0x20000594) passes 0x18 it runs the full AFE power-down /
 * self-test shutdown loop three times, drops into the local state 0xff then back
 * to state 6, then blocks in a charge-button release loop (PB8) until the line
 * is held for >0x13 ticks (toggling mode bit8, 0x100). Finally bumps the slow
 * counter 0x200006a2 and every 20th tick (>0x13) prints the progress count, then
 * the extend-IO refresh, AFE status poll, and the tail uptime/IWDG step.
 * The inner power-down loops kick the IWDG (0x200006ac->KR=0xAAAA) every slow
 * tick to survive the long blocking waits; if PA11 (GPIOA 0x800) asserts during
 * power-down the part deliberately spins forever (hard fault latch). */
void bms_state_6(void)
{
    volatile uint16_t * const mode = (volatile uint16_t *)0x200006a0;

    bms_core_update();

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if ((*mode & 0x10) != 0) {                       /* bit4 */
            *mode &= 0xffefu;
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {                            /* slow tick (bit1) */
        *s_tick &= (uint8_t)~2u;
        alarm_scan_b5();
        if (gpio_bit_read(0x48000400, 0x100) == 0) {     /* PB8 released */
            if (!(*mode & 0x100)) {                       /* bit8 clear */
                uint8_t v = *(volatile uint8_t *)0x20000710;
                *(volatile uint8_t *)0x20000710 = (uint8_t)(v + 1);
                if ((uint8_t)(v + 1) > 3) {
                    *(volatile uint32_t *)0x2000072c = 0;
                    log_print(2, s_state6_charge_mode);
                    uart_flush();
                    *mode |= 0x100u;
                    boot_mode_enter(1);
                    return;
                }
            }
        } else {                                          /* PB8 held */
            if (*mode & 0x100) {                          /* bit8 set */
                *mode &= 0xfeffu;
            }
            *(volatile uint8_t *)0x20000710 = 0;
        }

        if (*(volatile uint32_t *)0x2000042c > 0x4e1f) {  /* charger voltage present */
            *(volatile uint32_t *)0x2000072c = 0;
            log_print(2, s_state6_charger_present);
            uart_flush();
            boot_mode_enter(3);
            return;
        }

        if (*(volatile uint32_t *)0x20000594 > 0x18) {    /* uptime past 0x18 */
            uint8_t pass = 0;
            do {
                uint16_t kick = 0;
                gpio_bit_write(0x48000400, 0x1000, 1);    /* PB12 = 1 */
                log_print(2, s_state6_powerdown_a);
                uart_flush();
                fedl5236_powerdown(1);
                log_print(2, s_state6_powerdown_b);
                uart_flush();
                int got;
                do {
                    if ((*s_tick & 1) != 0) {
                        *s_tick &= (uint8_t)~1u;
                        kick = (uint16_t)(kick + 1);
                    }
                    if ((*s_tick & 2) != 0) {
                        *s_tick &= (uint8_t)~2u;
                        **(volatile uint32_t **)0x200006ac = 0x0000AAAAu;  /* IWDG_KR */
                    }
                    got = gpio_bit_read(0x48000000, 0x800);
                } while (got == 0 && kick <= 0x3e7);
                if (gpio_bit_read(0x48000000, 0x800) == 1) {  /* PA11 latched */
                    gpio_bit_write(0x48000400, 0x1000, 0);
                    **(volatile uint32_t **)0x200006ac = 0x0000AAAAu;
                    for (;;) { }                          /* hard-fault hold */
                }
                kick = 0;
                do {
                    if ((*s_tick & 1) != 0) {
                        *s_tick &= (uint8_t)~1u;
                        kick = (uint16_t)(kick + 1);
                    }
                    if ((*s_tick & 2) != 0) {
                        *s_tick &= (uint8_t)~2u;
                        **(volatile uint32_t **)0x200006ac = 0x0000AAAAu;
                    }
                } while (kick <= 0x3e7);
                pass = (uint8_t)(pass + 1);
            } while (pass < 3);

            *(volatile uint8_t *)0x200005ac = 0xff;
            *mode &= 0xefffu;                             /* clear bit12 */
            *(volatile uint8_t *)0x200006e5 = 0;
            extend_io_update();

            uint16_t kick = 0;
            do {
                if ((*s_tick & 1) != 0) {
                    *s_tick &= (uint8_t)~1u;
                    kick = (uint16_t)(kick + 1);
                }
                if ((*s_tick & 2) != 0) {
                    *s_tick &= (uint8_t)~2u;
                    **(volatile uint32_t **)0x200006ac = 0x0000AAAAu;
                }
            } while (kick <= 0x1387);

            *(volatile uint8_t *)0x200005ac = 6;
            extend_io_update();
            *(volatile uint8_t *)0x20000710 = 0;
            *mode &= 0xfeffu;                             /* clear bit8 */

            do {
                if ((*s_tick & 1) != 0) {
                    *s_tick &= (uint8_t)~1u;
                    if (gpio_bit_read(0x48000400, 0x100) == 0) {   /* PB8 released */
                        if (*mode & 0x100) {              /* bit8 set */
                            *(volatile uint8_t *)0x20000710 = 0;
                        } else {
                            uint8_t v = *(volatile uint8_t *)0x20000710;
                            *(volatile uint8_t *)0x20000710 = (uint8_t)(v + 1);
                            if ((uint8_t)(v + 1) > 0x13) {
                                *(volatile uint8_t *)0x20000710 = 0;
                                *mode |= 0x100u;
                                system_reset_request();
                            }
                        }
                    } else if (*mode & 0x100) {           /* PB8 held, bit8 set */
                        uint8_t v = *(volatile uint8_t *)0x20000710;
                        *(volatile uint8_t *)0x20000710 = (uint8_t)(v + 1);
                        if ((uint8_t)(v + 1) > 0x13) {
                            *(volatile uint8_t *)0x20000710 = 0;
                            *mode &= 0xfeffu;             /* clear bit8 */
                        }
                    } else {
                        *(volatile uint8_t *)0x20000710 = 0;
                    }
                }
                if ((*s_tick & 2) != 0) {
                    *s_tick &= (uint8_t)~2u;
                    **(volatile uint32_t **)0x200006ac = 0x0000AAAAu;
                }
            } while (!(*mode & 0x100));                   /* until bit8 set */
        }

        volatile uint16_t * const cnt = (volatile uint16_t *)0x200006a2;
        uint16_t c = *cnt;
        *cnt = (uint16_t)(c + 1);
        if ((uint16_t)((c + 1) % 0x14) == 0 && *cnt > 0x13) {
            log_print(2, s_state6_progress, (c + 1) / 0x14);
        }
        extend_io_update();
        charger_afe_refresh();
    }

    tick_uptime();
}

/* State 7 (bms_state_7) — active charge/run state: run the BMS core, bail to
 * the AFE-latch fault path if the latched-fault byte 0x2000041d is set, then on
 * the fast tick (bit0) — only while mode bit4 is set — branch by fault state
 * (bit1 -> bms_enter_ovp2, bit0 clear -> boot_mode_enter(1), bit11 -> bms_enter_ov2_record,
 * else bms_measure_update); on the slow tick (bit1) run the periodic I/O refresh,
 * leaving to bms_enter_ov2_record on the protection-status flags (0x200006e4 bit5/6/7)
 * or to bms_enter_idle3 on a hard discharge with the run-enable bit set. Tail tick:
 * tick_uptime(). */
void bms_state_7(void)
{
    volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;
    volatile uint8_t  * const prot  = (volatile uint8_t  *)0x200006e4;

    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if ((*mode & 0x10) != 0) {
            *mode &= 0xffefu;
            if ((*fault & 2) != 0) {
                bms_enter_ovp2();
                return;
            }
            if ((*fault & 1) == 0) {
                boot_mode_enter(1);
                return;
            }
            if ((*fault & 0x800) != 0) {
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        alarm_scan_b5();
        alarm_scan_b6();
        if (*prot != 0 &&
            ((*prot & 0x20) != 0 || (*prot & 0x40) != 0 || (*prot & 0x80) != 0)) {
            bms_enter_ov2_record();
            return;
        }
        if (*(volatile uint32_t *)0x20000420 > 199 &&
            (*(volatile uint8_t *)0x20000412 & 1) == 1) {
            bms_enter_idle3();
            return;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }

    tick_uptime();
}

/* State 8 (bms_state_8) — operating state: run the BMS core, then on the 1 ms
 * tick service the protection/over-voltage dispatch (mode bit4 gated), and on
 * the slow tick run the GPIO pulse pair, the boot-button watchdog, the hard
 * discharge → AFE-shutdown branch, and the charger/IO/AFE refresh; finally the
 * IWDG-kick / uptime tail. The AFE flag at 0x2000041d forces the AFE-fault exit.
 *
 * Differs from state 7 only in the mode-bit4 fault dispatch: here OVP1 (FAULT
 * bit0) being CLEAR drives boot_mode_enter(1) while bit0 SET runs bms_enter_ovp1;
 * state 7 inverts that and uses bms_enter_ovp2. */
void bms_state_8(void)
{
    volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const boot  = (volatile uint16_t *)0x200006e4;

    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }
    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if ((*mode & 0x10) != 0) {                 /* mode bit4 */
            *mode &= 0xffefu;
            if (!(FAULT & 2)) {                    /* OVP2 (bit1) clear */
                if ((FAULT & 1) != 0) {            /* OVP1 (bit0) set */
                    bms_enter_ovp1();
                    return;
                }
                boot_mode_enter(1);
                return;
            }
            if ((FAULT & 0x800) != 0) {            /* cell imbalance (bit11) */
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }
    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        alarm_scan_b5();
        alarm_scan_b6();
        if (*boot != 0 &&
            (((*boot & 0x20) != 0) ||             /* bit5 */
             ((*boot & 0x40) != 0) ||             /* bit6 */
             ((*boot & 0x80) != 0))) {            /* bit7 */
            bms_enter_ov2_record();
            return;
        }
        if (*(volatile uint32_t *)0x20000420 > 199 &&
            (*(volatile uint8_t *)0x20000412 & 1) == 1) {
            bms_enter_idle3();
            return;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }
    tick_uptime();
}

/* State 9 (bms_state_9) — operating state: run the BMS core, then on the
 * AFE-latch fault (0x2000041d) bail to bms_enter_afe_latch. On the 1 ms tick, if the
 * mode bit4 (charger-present, 0x200006a0 & 0x10) is set, clear it and dispatch
 * on the fault register 0x20000410: UVP2 (bit3) → bms_enter_uvp2, UVP1 clear
 * (bit2) → boot_mode_enter(2), imbalance (bit11) → bms_enter_ov2_record; otherwise run
 * bms_measure_update. On the slow tick (bit1): step the two display/IO updates, then
 * on a NTC/over-temp flag (0x200006e4 bits 5/6/7) bail to bms_enter_ov2_record; else
 * count charger-absent slow ticks in 0x200006a8 and after 19 enter boot_enter_operating(0),
 * resetting the counter when the charger (0x2000042c > 19999) is present. Tail:
 * charger_shipping_check, extend_io_update, AFE status poll, then tick_uptime. */
void bms_state_9(void)
{
    volatile uint16_t * const mode = (volatile uint16_t *)0x200006a0;

    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if ((*mode & 0x10) != 0) {
            *mode &= 0xffefu;
            if ((FAULT & 0x8) != 0) {
                bms_enter_uvp2();
                return;
            }
            if (!(FAULT & 0x4)) {
                boot_mode_enter(2);
                return;
            }
            if ((FAULT & 0x800) != 0) {
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        alarm_scan_b5();
        alarm_scan_b7();
        if (*(volatile uint16_t *)0x200006e4 != 0 &&
            (((*(volatile uint16_t *)0x200006e4 & 0x20) != 0) ||
             ((*(volatile uint16_t *)0x200006e4 & 0x40) != 0) ||
             ((*(volatile uint16_t *)0x200006e4 & 0x80) != 0))) {
            bms_enter_ov2_record();
            return;
        }
        if (*(volatile uint32_t *)0x2000042c > 0x4e1f) {
            volatile uint16_t * const cnt = (volatile uint16_t *)0x200006a8;
            uint16_t v = *cnt;
            *cnt = (uint16_t)(v + 1);
            if ((uint16_t)(v + 1) > 0x13) {
                *cnt = 0;
                boot_enter_operating(0);
                return;
            }
        } else {
            *(volatile uint16_t *)0x200006a8 = 0;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }

    tick_uptime();
}

/* State 10 (bms_state_10) — operating state: run the BMS core, bail to the AFE
 * fault handler on the latched AFE-fault byte 0x2000041d, then on the 1 ms tick
 * service the protection-driven transitions and on the slow tick run the
 * extend-IO/charger refresh with the "charger absent for 20 ticks" handoff. */
void bms_state_10(void)
{
    volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;

    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if ((*mode & 0x10) != 0) {                       /* mode bit4 */
            *mode &= 0xffefu;
            if (!(*fault & 8)) {                          /* fault bit3 clear */
                if ((*fault & 4) != 0) {                  /* fault bit2 set */
                    bms_enter_uvp1();
                    return;
                }
                boot_mode_enter(2);                          /* boot_mode_enter(2) */
                return;
            }
            if ((*fault & 0x800) != 0) {                  /* fault bit11 */
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {                             /* slow tick (bit1) */
        *s_tick &= (uint8_t)~2u;
        alarm_scan_b5();
        alarm_scan_b7();
        if (*(volatile uint16_t *)0x200006e4 != 0 &&
            (((*(volatile uint16_t *)0x200006e4 & 0x20) != 0) ||   /* bit5 */
             ((*(volatile uint16_t *)0x200006e4 & 0x40) != 0) ||   /* bit6 */
             ((*(volatile uint16_t *)0x200006e4 & 0x80) != 0))) {  /* bit7 */
            bms_enter_ov2_record();
            return;
        }
        if (*(volatile uint32_t *)0x2000042c > 0x4e1f) {
            uint16_t v = *(volatile uint16_t *)0x200006a8;
            *(volatile uint16_t *)0x200006a8 = (uint16_t)(v + 1);
            if ((uint16_t)(v + 1) > 0x13) {
                *(volatile uint16_t *)0x200006a8 = 0;
                boot_enter_operating(0);
                return;
            }
        } else {
            *(volatile uint16_t *)0x200006a8 = 0;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }

    tick_uptime();
}

/* State 11 (bms_state_11) — operating state: run the BMS core, then on the AFE
 * fault byte branch to the latch handler; otherwise on the 1 ms tick walk the
 * fault register (0x20000410) and hand off to the matching protection handler
 * (gated by mode bit4 / 0x200006a0), and on the slow tick refresh I/O, run the
 * AFE poll, and re-check the discharge-OC / extend status before tail-calling
 * the uptime tick. */
void bms_state_11(void)
{
    volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;

    bms_core_update();

    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if ((*mode & 0x10) != 0) {
            *mode &= 0xffefu;
            if ((*fault & 2) != 0) {
                bms_enter_ovp2();
                return;
            }
            if ((*fault & 1) != 0) {
                bms_enter_ovp1();
                return;
            }
            if ((*fault & 8) != 0) {
                bms_enter_uvp2();
                return;
            }
            if ((*fault & 4) != 0) {
                bms_enter_uvp1();
                return;
            }
            if ((*fault & 0x10) == 0) {
                if (*(volatile uint32_t *)0x20000420 > 199) {
                    bms_enter_idle3();
                    return;
                }
                boot_mode_enter(3);
                return;
            }
            if ((*fault & 0x800) != 0) {
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        if (*(volatile uint32_t *)0x20000420 > 199) {
            bms_enter_idle3();
            return;
        }
        alarm_scan_b5();
        alarm_scan_b6();
        if (*(volatile uint8_t *)0x200006e4 != 0 &&
            (((*(volatile uint8_t *)0x200006e4 & 0x20) != 0) ||
             ((*(volatile uint8_t *)0x200006e4 & 0x40) != 0) ||
             ((*(volatile uint8_t *)0x200006e4 & 0x80) != 0))) {
            bms_enter_ov2_record();
            return;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }

    tick_uptime();
}

/* State 12 (bms_state_12) — operating state: run the BMS core, then on the AFE
 * power-fault flag (0x2000041d) bail to the AFE-fault handler; otherwise do the
 * 1 ms tick (fault dispatch off the protection register 0x20000410, gated on
 * mode bit4 of 0x200006a0) and the slow tick (charger/IO refresh, ship-mode
 * arming). Identical to state 11 except the auto-recover charge-OC probe tests
 * fault bit5 (0x20) here vs bit4 (0x10) there. */
void bms_state_12(void)
{
    volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;
    volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const m6e4  = (volatile uint16_t *)0x200006e4;

    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }
    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if ((*mode & 0x10) != 0) {
            *mode &= 0xffefu;
            if ((*fault & 0x2) != 0) {
                bms_enter_ovp2();
                return;
            }
            if ((*fault & 1) != 0) {
                bms_enter_ovp1();
                return;
            }
            if ((*fault & 0x8) != 0) {
                bms_enter_uvp2();
                return;
            }
            if ((*fault & 0x4) != 0) {
                bms_enter_uvp1();
                return;
            }
            if (!(*fault & 0x20)) {
                if (*(volatile uint32_t *)0x20000420 > 199) {
                    bms_enter_idle3();
                    return;
                }
                boot_mode_enter(3);
                return;
            }
            if ((*fault & 0x800) != 0) {
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }
    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        if (*(volatile uint32_t *)0x20000420 > 199) {
            bms_enter_idle3();
            return;
        }
        alarm_scan_b5();
        alarm_scan_b6();
        if (*m6e4 != 0 &&
            (((*m6e4 & 0x20) != 0) || ((*m6e4 & 0x40) != 0) || ((*m6e4 & 0x80) != 0))) {
            bms_enter_ov2_record();
            return;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }
    tick_uptime();
}

/* State 13 (bms_state_13) — pack-2 charge-trip hold. Runs the BMS core, then:
 * on the AFE-fault byte 0x2000041d transitions out via bms_enter_afe_latch; on the
 * 1 ms tick (and when mode bit4 is set, which it then clears) dispatches the
 * latched fault by priority (fault bit3 -> bms_enter_uvp2, bit2 -> bms_enter_uvp1,
 * bit6 CLEAR -> bms_state_idle, bit11 -> bms_enter_ov2_record), else runs the 1 ms slow
 * refresh; on the slow tick counts down a charge-present timer (chg_i >= 200 and
 * charger volts > 19999) and after 0x13 ticks transitions via boot_enter_operating(0),
 * then the slow-tick refresh, the fault-subclass check (0x200006e4 bits 5/6/7 ->
 * bms_enter_ov2_record), and the charger/IO/AFE refresh; finally the uptime tail. */
void bms_state_13(void)
{
    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }
    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
        volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;
        if (*mode & (1u << 4)) {
            *mode &= 0xffefu;
            if (*fault & (1u << 3)) {
                bms_enter_uvp2();
                return;
            }
            if (*fault & (1u << 2)) {
                bms_enter_uvp1();
                return;
            }
            if (!(*fault & (1u << 6))) {
                bms_state_idle();
                return;
            }
            if (*fault & (1u << 11)) {
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }
    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        volatile uint16_t * const cnt = (volatile uint16_t *)0x200006a8;
        if (*(volatile uint32_t *)0x200003a8 < 200 ||
            *(volatile uint32_t *)0x2000042c <= 0x4e1f) {
            *cnt = 0;
        } else {
            uint16_t v = *cnt;
            *cnt = (uint16_t)(v + 1);
            if ((uint16_t)(v + 1) > 0x13) {
                *cnt = 0;
                boot_enter_operating(0);
                return;
            }
        }
        alarm_scan_b5();
        alarm_scan_b7();
        volatile uint16_t * const sub = (volatile uint16_t *)0x200006e4;
        if (*sub != 0 &&
            ((*sub & (1u << 5)) || (*sub & (1u << 6)) || (*sub & (1u << 7)))) {
            bms_enter_ov2_record();
            return;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }
    tick_uptime();
}

/* State 14 (bms_state_14) — operating state: run the BMS core, then on the 1 ms
 * tick run the protection-fault dispatch (mode bit4 latched faults route to the
 * matching fault state, else fall to idle/imbalance), and on the slow tick run
 * the charger-present timeout (no charge current and charger absent for >19
 * ticks → enter state via boot_enter_operating), the periodic IO refresh, the
 * thermal/over-current trip check (status 0x200006e4 bits 5/6/7), and the
 * LED/extend-IO/AFE refresh. Trailing uptime tick. (Near-identical to state 13;
 * the only difference is the idle test reads fault bit7 here vs bit6 there.) */
void bms_state_14(void)
{
    volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;
    volatile uint16_t * const cnt   = (volatile uint16_t *)0x200006a8;
    volatile uint8_t  * const trip  = (volatile uint8_t  *)0x200006e4;

    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if ((*mode & (1u << 4)) != 0) {
            *mode &= 0xffefu;
            if ((*fault & (1u << 3)) != 0) {
                bms_enter_uvp2();
                return;
            }
            if ((*fault & (1u << 2)) != 0) {
                bms_enter_uvp1();
                return;
            }
            if ((*fault & (1u << 7)) == 0) {
                bms_state_idle();
                return;
            }
            if ((*fault & (1u << 11)) != 0) {
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        if (*(volatile uint32_t *)0x200003a8 < 200 ||
            *(volatile uint32_t *)0x2000042c <= 0x4e1f) {
            *cnt = 0;
        } else {
            uint16_t v = *cnt;
            *cnt = (uint16_t)(v + 1);
            if ((uint16_t)(v + 1) > 0x13) {
                *cnt = 0;
                boot_enter_operating(0);
                return;
            }
        }
        alarm_scan_b5();
        alarm_scan_b7();
        if (*trip != 0 &&
            (((*trip & (1u << 5)) != 0) ||
             ((*trip & (1u << 6)) != 0) ||
             ((*trip & (1u << 7)) != 0))) {
            bms_enter_ov2_record();
            return;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }

    tick_uptime();
}

/* State 15 (bms_state_15) — charging/operating hold: run the BMS core, then on
 * the 1 ms tick service the cfg-gated fault-transition cascade, and on the slow
 * tick count the "charging stalled" timer (transition after 19 ticks) plus the
 * extend-IO / charger / AFE refresh, with an early bail to the extra-fault state
 * when the secondary fault byte 0x200006e4 flags a charger fault. */
void bms_state_15(void)
{
    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
        volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;
        if ((*mode & 0x10) != 0) {
            *mode &= 0xffefu;
            if ((*fault & 4) != 0) {            /* UVP1 */
                bms_enter_uvp1();
                return;
            }
            if ((*fault & 8) != 0) {            /* UVP2 */
                bms_enter_uvp2();
                return;
            }
            if ((*fault & 0x100) == 0) {        /* AFE comms fault clear */
                bms_state_idle();
                return;
            }
            if ((*fault & 0x800) != 0) {        /* cell imbalance */
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        volatile uint16_t * const cnt = (volatile uint16_t *)0x200006a8;
        if (*(volatile uint32_t *)0x200003a8 < 200 ||
            *(volatile uint32_t *)0x2000042c <= 0x4e1f) {
            *cnt = 0;
        } else {
            uint16_t v = *cnt;
            *cnt = (uint16_t)(v + 1);
            if ((uint16_t)(v + 1) > 0x13) {
                *cnt = 0;
                boot_enter_operating(0);
                return;
            }
        }
        alarm_scan_b5();
        alarm_scan_b7();
        volatile uint8_t * const fault2 = (volatile uint8_t *)0x200006e4;
        if (*fault2 != 0 &&
            ((*fault2 & 0x20) != 0 || (*fault2 & 0x40) != 0 || (*fault2 & 0x80) != 0)) {
            bms_enter_ov2_record();
            return;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }

    tick_uptime();
}

/* State 16 (bms_state_16) — operating hold with a PB8-gated stall watchdog: run
 * the BMS core, then on the 1 ms tick poll PB8; if it stays low for >19 ticks
 * clear fault bit9 (0x200) and re-init via bms_state_idle. Otherwise run the
 * cfg-gated (mode bit4) fault-transition cascade. On the slow tick count the
 * "charging stalled" timer (transition after 19 ticks), refresh the IO/charger
 * helpers, and bail to the extra-fault state when the secondary fault byte
 * 0x200006e4 flags a charger fault. */
void bms_state_16(void)
{
    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;
        if (gpio_bit_read(0x48000400, 0x100) == 0) {       /* PB8 low */
            uint8_t v = *(volatile uint8_t *)0x20000710;
            *(volatile uint8_t *)0x20000710 = (uint8_t)(v + 1);
            if ((uint8_t)(v + 1) > 0x13) {
                *fault &= 0xfdffu;                          /* clear bit9 */
                bms_state_idle();
                return;
            }
        } else {
            *(volatile uint8_t *)0x20000710 = 0;
        }
        volatile uint16_t * const mode = (volatile uint16_t *)0x200006a0;
        if ((*mode & 0x10) != 0) {
            *mode &= 0xffefu;
            if ((*fault & 4) != 0) {            /* UVP1 */
                bms_enter_uvp1();
                return;
            }
            if ((*fault & 8) != 0) {            /* UVP2 */
                bms_enter_uvp2();
                return;
            }
            if ((*fault & 0x800) != 0) {        /* cell imbalance */
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        volatile uint16_t * const cnt = (volatile uint16_t *)0x200006a8;
        if (*(volatile uint32_t *)0x200003a8 < 200 ||
            *(volatile uint32_t *)0x2000042c <= 0x4e1f) {
            *cnt = 0;
        } else {
            uint16_t v = *cnt;
            *cnt = (uint16_t)(v + 1);
            if ((uint16_t)(v + 1) > 0x13) {
                *cnt = 0;
                boot_enter_operating(0);
                return;
            }
        }
        alarm_scan_b5();
        alarm_scan_b7();
        volatile uint8_t * const fault2 = (volatile uint8_t *)0x200006e4;
        if (*fault2 != 0 &&
            ((*fault2 & 0x20) != 0 || (*fault2 & 0x40) != 0 || (*fault2 & 0x80) != 0)) {
            bms_enter_ov2_record();
            return;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }

    tick_uptime();
}

/* State 17 (bms_state_17) — operating/charging hold: run the BMS core, then on
 * the 1 ms tick watch the PB8 input (20-tick debounce → idle) and, when mode
 * bit4 latches, dispatch the UVP / cell-imbalance / boot-mode fault cascade; on
 * the slow tick run the "hard-charge stalled" countdown (transition after 19
 * ticks), the GPIO pulse pair, the secondary charger-fault bail, and the
 * charger / extend-IO / AFE refresh; finally the IWDG-kick / uptime tail.
 *
 * Unlike state 16 this handler has no top-level AFE-fault (0x2000041d) exit and
 * does not pre-clear FAULT bit9 before the idle transition. */
void bms_state_17(void)
{
    volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;

    bms_core_update();

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if (gpio_bit_read(0x48000400, 0x100) == 0) {       /* PB8 */
            uint8_t v = *(volatile uint8_t *)0x20000710;
            *(volatile uint8_t *)0x20000710 = (uint8_t)(v + 1);
            if ((uint8_t)(v + 1) > 0x13) {
                bms_state_idle();
                return;
            }
        } else {
            *(volatile uint8_t *)0x20000710 = 0;
        }
        if ((*mode & 0x10) != 0) {                          /* mode bit4 */
            *mode &= 0xffefu;
            if ((*fault & 8) != 0) {                        /* UVP2 (bit3) */
                bms_enter_uvp2();
                return;
            }
            if ((*fault & 4) != 0) {                        /* UVP1 (bit2) */
                bms_enter_uvp1();
                return;
            }
            if ((*fault & 0x400) == 0) {                    /* TS/temp (bit10) clear */
                if ((*mode & 0x100) != 0) {                 /* mode bit8 */
                    boot_mode_enter(1);
                    return;
                }
                boot_mode_enter(3);
                return;
            }
            if ((*fault & 0x800) != 0) {                    /* cell imbalance (bit11) */
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        volatile uint16_t * const cnt = (volatile uint16_t *)0x200006a8;
        if (*(volatile uint32_t *)0x200003a8 < 200 ||
            *(volatile uint32_t *)0x2000042c <= 0x4e1f) {
            *cnt = 0;
        } else {
            uint16_t v = *cnt;
            *cnt = (uint16_t)(v + 1);
            if ((uint16_t)(v + 1) > 0x13) {
                *cnt = 0;
                boot_enter_operating(0);
                return;
            }
        }
        alarm_scan_b5();
        alarm_scan_b7();
        volatile uint8_t * const fault2 = (volatile uint8_t *)0x200006e4;
        if (*fault2 != 0 &&
            ((*fault2 & 0x20) != 0 ||                        /* bit5 */
             (*fault2 & 0x40) != 0 ||                        /* bit6 */
             (*fault2 & 0x80) != 0)) {                       /* bit7 */
            bms_enter_ov2_record();
            return;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }

    tick_uptime();
}

/* State 18 (bms_state_18) — operating state with thermal shutdown: run the BMS
 * core, then on the AFE fault byte (0x2000041d) branch to the latch handler;
 * otherwise on the 1 ms tick walk the fault register (0x20000410) under mode
 * bit4 (0x200006a0) and hand off to the matching protection handler; on the
 * slow tick run the charge/discharge-OC and the warm-temperature shutdown
 * (both AFE TS readings below 0x53 for >29 ticks enters boot mode 3), refresh
 * the service I/O and the AFE poll, re-check the extend status, then tail-call
 * the uptime tick. */
void bms_state_18(void)
{
    volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;
    volatile uint8_t  * const ts    = (volatile uint8_t  *)0x20000218; /* +1 charge TS, +2 discharge TS */

    bms_core_update();

    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if ((*mode & 0x10) != 0) {
            *mode &= 0xffefu;
            if ((*fault & 2) != 0) {
                bms_enter_ovp2();
                return;
            }
            if ((*fault & 1) != 0) {
                bms_enter_ovp1();
                return;
            }
            if ((*fault & 8) != 0) {
                bms_enter_uvp2();
                return;
            }
            if ((*fault & 4) != 0) {
                bms_enter_uvp1();
                return;
            }
            if ((*fault & 0x800) != 0) {
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        if (*(volatile uint32_t *)0x20000420 > 199) {
            bms_enter_idle3();
            return;
        }
        if (ts[1] < 0x53 && ts[2] < 0x53) {
            volatile uint16_t * const cnt = (volatile uint16_t *)0x20000550;
            uint16_t v = *cnt;
            *cnt = (uint16_t)(v + 1);
            if ((uint16_t)(v + 1) > 0x1d) {
                boot_mode_enter(3);
                return;
            }
        } else {
            *(volatile uint16_t *)0x20000550 = 0;
        }
        alarm_scan_b5();
        alarm_scan_b6();
        if (*(volatile uint8_t *)0x200006e4 != 0 &&
            (((*(volatile uint8_t *)0x200006e4 & 0x20) != 0) ||
             ((*(volatile uint8_t *)0x200006e4 & 0x40) != 0) ||
             ((*(volatile uint8_t *)0x200006e4 & 0x80) != 0))) {
            bms_enter_ov2_record();
            return;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }

    tick_uptime();
}

/* bms_state_19 — OEM bms_state_19 @ 0x08009414 (354 B). Operating-state routine
 * for state 19: run the shared BMS core update, then bail to the AFE-fault
 * transition if the latched AFE-fault byte (0x2000041d) is set. Otherwise do the
 * per-tick work keyed off the SysTick flag byte:
 *   bit0 (1 ms): if mode bit4 (0x200006a0) is set, clear it and dispatch the
 *     highest-priority latched protection in the fault word 0x20000410
 *     (bit1 -> bms_enter_ovp2, bit0 -> bms_enter_ovp1, bit3 -> bms_enter_uvp2,
 *      bit2 -> bms_enter_uvp1, bit11 -> bms_enter_ov2_record), else run bms_measure_update.
 *   bit1 (slow): hard-discharge (>199) bails to bms_enter_idle3; else if both TS
 *     readings (0x20000218+1/+2) are warm (>=0x2b) count the ship timer
 *     0x20000580 and enter boot mode 3 after 29 ticks; then refresh IO and, if
 *     any of bits 5/6/7 of 0x200006e4 are set, bail to bms_enter_ov2_record, else run
 *     the charger/extend/AFE refresh.
 *   then tick_uptime() (IWDG kick + uptime).
 * Each FUN()/return below is a tail-branch transition; every early-return kept.
 */
void bms_state_19(void)
{
    volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;
    volatile uint8_t  * const ts    = (volatile uint8_t  *)0x20000218; /* +1 charge TS, +2 discharge TS */
    volatile uint16_t * const ship  = (volatile uint16_t *)0x20000580;
    volatile uint16_t * const flags = (volatile uint16_t *)0x200006e4;

    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if ((*mode & 0x10) != 0) {                 /* mode bit4 */
            *mode &= 0xffefu;
            if ((*fault & 2) != 0) {               /* fault bit1 */
                bms_enter_ovp2();
                return;
            }
            if ((*fault & 1) != 0) {               /* fault bit0 */
                bms_enter_ovp1();
                return;
            }
            if ((*fault & 8) != 0) {               /* fault bit3 */
                bms_enter_uvp2();
                return;
            }
            if ((*fault & 4) != 0) {               /* fault bit2 */
                bms_enter_uvp1();
                return;
            }
            if ((*fault & 0x800) != 0) {           /* fault bit11 */
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {                      /* slow tick (bit1) */
        *s_tick &= (uint8_t)~2u;
        if (*(volatile uint32_t *)0x20000420 > 199) {
            bms_enter_idle3();
            return;
        }
        if (ts[1] < 0x2b || ts[2] < 0x2b) {
            *ship = 0;
        } else {
            uint16_t v = *ship;
            *ship = (uint16_t)(v + 1);
            if ((uint16_t)(v + 1) > 0x1d) {
                boot_mode_enter(3);
                return;
            }
        }
        alarm_scan_b5();
        alarm_scan_b6();
        if (*flags != 0 &&
            ((*flags & 0x20) != 0 ||               /* bit5 */
             (*flags & 0x40) != 0 ||               /* bit6 */
             (*flags & 0x80) != 0)) {              /* bit7 */
            bms_enter_ov2_record();
            return;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }

    tick_uptime();
}

/* State 20 (bms_state_20) — operating state with a thermal-recovery watch: run
 * the BMS core, then on the AFE-fault byte 0x2000041d transition out via
 * bms_enter_afe_latch. On the 1 ms tick, when mode bit4 is latched (then cleared) the
 * protection-fault cascade routes by priority (fault bit3 -> bms_enter_uvp2,
 * bit2 -> bms_enter_uvp1, bit11 -> bms_enter_ov2_record), else runs the 1 ms refresh
 * bms_measure_update. On the slow tick: while both AFE temperature samples
 * (0x20000218+1 charge TS, +2 discharge TS) are below 101 it counts ticks in
 * 0x20000584 and after 0x1d returns to idle via bms_state_idle, else the counter
 * resets; then the periodic IO refresh trio, the secondary fault-byte check
 * (0x200006e4 bits 5/6/7 -> bms_enter_ov2_record), and the charger/extend-IO/AFE
 * refresh. Trailing uptime tick. */
void bms_state_20(void)
{
    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
        volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;
        if ((*mode & (1u << 4)) != 0) {
            *mode &= 0xffefu;
            if ((*fault & (1u << 3)) != 0) {
                bms_enter_uvp2();
                return;
            }
            if ((*fault & (1u << 2)) != 0) {
                bms_enter_uvp1();
                return;
            }
            if ((*fault & (1u << 11)) != 0) {
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        volatile uint8_t  * const ts  = (volatile uint8_t  *)0x20000218; /* +1 charge TS, +2 discharge TS */
        volatile uint16_t * const cnt = (volatile uint16_t *)0x20000584;
        if (ts[1] < 0x65 && ts[2] < 0x65) {
            uint16_t v = *cnt;
            *cnt = (uint16_t)(v + 1);
            if ((uint16_t)(v + 1) > 0x1d) {
                bms_state_idle();
                return;
            }
        } else {
            *cnt = 0;
        }
        alarm_scan_b5();
        alarm_scan_b6();
        alarm_scan_b7();
        volatile uint8_t * const fault2 = (volatile uint8_t *)0x200006e4;
        if (*fault2 != 0 &&
            ((*fault2 & (1u << 5)) != 0 ||
             (*fault2 & (1u << 6)) != 0 ||
             (*fault2 & (1u << 7)) != 0)) {
            bms_enter_ov2_record();
            return;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }

    tick_uptime();
}

/* State 21 (bms_state_21) — temperature-recovery hold: run the BMS core, then on
 * the 1 ms tick run the cfg-gated latched-fault dispatch (mode bit4: fault bit3 ->
 * bms_enter_uvp2, bit2 -> bms_enter_uvp1, bit11 -> bms_enter_ov2_record), else the 1 ms slow
 * refresh; on the slow tick run a "temperature back within range" recovery timer —
 * both AFE TS channels (charge TS 0x20000219, discharge TS 0x2000021a) above 0x16
 * for >0x1d ticks exits to idle via bms_state_idle (bms_state_idle) — then the IO
 * refresh, the secondary trip check (0x200006e4 bits 5/6/7 -> bms_enter_ov2_record), and
 * the charger/extend-IO/AFE refresh. Trailing uptime tick.
 * Note vs states 20/13/14: the recovery timer has NO else-branch reset (the OEM
 * never zeroes the 0x200004ca counter), the slow block runs an extra refresh call
 * alarm_scan_b6, and the 1 ms fault cascade skips the idle/bit6 test. */
void bms_state_21(void)
{
    volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;
    volatile uint8_t  * const ts    = (volatile uint8_t  *)0x20000218; /* +1 charge TS, +2 discharge TS */
    volatile uint16_t * const cnt   = (volatile uint16_t *)0x200004ca;
    volatile uint8_t  * const trip  = (volatile uint8_t  *)0x200006e4;

    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if ((*mode & (1u << 4)) != 0) {
            *mode &= 0xffefu;
            if ((*fault & (1u << 3)) != 0) {
                bms_enter_uvp2();
                return;
            }
            if ((*fault & (1u << 2)) != 0) {
                bms_enter_uvp1();
                return;
            }
            if ((*fault & (1u << 11)) != 0) {
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        if (ts[1] > 0x16 && ts[2] > 0x16) {
            uint16_t v = *cnt;
            *cnt = (uint16_t)(v + 1);
            if ((uint16_t)(v + 1) > 0x1d) {
                bms_state_idle();
                return;
            }
        }
        alarm_scan_b5();
        alarm_scan_b6();
        alarm_scan_b7();
        if (*trip != 0 &&
            (((*trip & (1u << 5)) != 0) ||
             ((*trip & (1u << 6)) != 0) ||
             ((*trip & (1u << 7)) != 0))) {
            bms_enter_ov2_record();
            return;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }

    tick_uptime();
}

/* State 22 (bms_state_22) — operating/hold state with a temperature-gated
 * timeout. Runs the BMS core, then: on the AFE-fault byte 0x2000041d transitions
 * out via bms_enter_afe_latch; on the 1 ms tick, when mode bit4 is latched (and then
 * cleared) dispatches the fault by priority (fault bit1 -> bms_enter_ovp2,
 * bit0 -> bms_enter_ovp1, bit3 -> bms_enter_uvp2, bit2 -> bms_enter_uvp1,
 * bit11 -> bms_enter_ov2_record), else the 1 ms slow refresh bms_measure_update; on the slow
 * tick (s_tick bit1) it counts up a timeout while the charge-TS temp byte
 * 0x20000218 stays below 0x74 (>=0x74 resets it) and after 0x1d ticks returns
 * to idle via bms_state_idle, then runs the periodic refresh (alarm_scan_b5 /
 * alarm_scan_b6 / alarm_scan_b7), the sub-fault check (status 0x200006e4 bits
 * 5/6/7 -> bms_enter_ov2_record), and the charger/IO/AFE refresh. Trailing uptime tick. */
void bms_state_22(void)
{
    volatile uint16_t * const mode  = (volatile uint16_t *)0x200006a0;
    volatile uint16_t * const fault = (volatile uint16_t *)0x20000410;
    volatile uint8_t  * const temp  = (volatile uint8_t  *)0x20000218;
    volatile uint16_t * const cnt   = (volatile uint16_t *)0x20000588;
    volatile uint16_t * const trip  = (volatile uint16_t *)0x200006e4;

    bms_core_update();
    if (*(volatile uint8_t *)0x2000041d != 0) {
        bms_enter_afe_latch();
        return;
    }

    if ((*s_tick & 1) != 0) {
        *s_tick &= (uint8_t)~1u;
        if ((*mode & (1u << 4)) != 0) {
            *mode &= 0xffefu;
            if ((*fault & (1u << 1)) != 0) {
                bms_enter_ovp2();
                return;
            }
            if ((*fault & 1) != 0) {
                bms_enter_ovp1();
                return;
            }
            if ((*fault & (1u << 3)) != 0) {
                bms_enter_uvp2();
                return;
            }
            if ((*fault & (1u << 2)) != 0) {
                bms_enter_uvp1();
                return;
            }
            if ((*fault & (1u << 11)) != 0) {
                bms_enter_ov2_record();
                return;
            }
        }
        bms_measure_update();
    }

    if ((*s_tick & 2) != 0) {
        *s_tick &= (uint8_t)~2u;
        if (*temp < 0x74) {
            uint16_t v = *cnt;
            *cnt = (uint16_t)(v + 1);
            if ((uint16_t)(v + 1) > 0x1d) {
                bms_state_idle();
                return;
            }
        } else {
            *cnt = 0;
        }
        alarm_scan_b5();
        alarm_scan_b6();
        alarm_scan_b7();
        if (*trip != 0 &&
            (((*trip & (1u << 5)) != 0) ||
             ((*trip & (1u << 6)) != 0) ||
             ((*trip & (1u << 7)) != 0))) {
            bms_enter_ov2_record();
            return;
        }
        charger_shipping_check();
        extend_io_update();
        charger_afe_refresh();
    }

    tick_uptime();
}
