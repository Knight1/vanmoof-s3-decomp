#include "powerbankware.h"

/*
 * Coulomb counter and cell-balance governor. Translated from this image; memory
 * access widths were cross-checked against the disassembly (the same SRAM word
 * is read at different widths by neighbouring functions, so Ghidra's per-
 * function type inference can't be trusted on its own here).
 *
 *   coulomb_integrate     = OEM FUN_08009D80
 *   cell_balance_update   = OEM FUN_08009BFC
 *   coulomb_discharge_sub = OEM FUN_08009D4C
 *
 * BMS record (0x200004D0) fields used by the integrator:
 *   +0x18 remaining mAh (u32)   +0x1c full capacity (u32)
 *   +0x20 SOC scratch (u32)     +0x24 cycle accumulator (u32)
 *   +0x50 cycle count (u16)     +0x5a SOC% (u8)  +0x5b SOC2% (u8)  +0x7a (history)
 */

#define REC ((volatile uint8_t  *)0x200004D0)

/* Record (0x200004D0) capacity fields, kept at the OEM widths. */
#define REC_REMAINING (*(volatile uint32_t *)(0x200004D0u + 0x18))  /* +0x18 remaining mAh */
#define REC_FULLCAP   (*(volatile uint32_t *)(0x200004D0u + 0x1c))  /* +0x1c full capacity */
#define REC_SOCSCRATCH (*(volatile uint32_t *)(0x200004D0u + 0x20)) /* +0x20 SOC scratch   */
#define REC_CYCLEACC  (*(volatile uint32_t *)(0x200004D0u + 0x24))  /* +0x24 cycle accum   */
#define REC_CYCLECNT  (*(volatile uint16_t *)(0x200004D0u + 0x50))  /* +0x50 cycle count   */

/* Live coulomb-counter SRAM globals (see docs/hardware.md). */
static volatile uint32_t * const s_remaining_cap = (volatile uint32_t *)0x20000238; /* remaining-cap accumulator (RSOC) */
static volatile uint32_t * const s_learn_base    = (volatile uint32_t *)0x20000230; /* capacity-learning start sample   */
static volatile uint32_t * const s_learn_accum   = (volatile uint32_t *)0x20000234; /* capacity-learning accumulator    */
static volatile uint16_t * const s_learn_count   = (volatile uint16_t *)0x20000240; /* capacity-learning sample count   */
static volatile uint8_t  * const s_temp_cal      = (volatile uint8_t  *)0x20000218; /* SOC-index/temp block; [1]/[2] TS  */
static volatile uint16_t * const s_min_cell      = (volatile uint16_t *)0x200003D2; /* min cell mV                      */
static volatile uint16_t * const s_run_counter   = (volatile uint16_t *)0x200003CE; /* run-time counter vs learn thresh */
static volatile uint32_t * const s_full_cap_limit= (volatile uint32_t *)0x200005A4;
static volatile uint32_t * const s_cycle_thresh  = (volatile uint32_t *)0x200005A8;

/* Cell-balance governor SRAM globals. */
static volatile uint16_t * const s_mode          = (volatile uint16_t *)0x200006A0; /* mode/cfg word */
static volatile uint8_t  * const s_bal_debounce  = (volatile uint8_t  *)0x2000041C; /* balance debounce tick */
static volatile uint8_t  * const s_bal_sel       = (volatile uint8_t  *)0x20000224; /* selected balance cell */
static volatile uint16_t * const s_max_cell      = (volatile uint16_t *)0x200003A2; /* max cell mV */
static volatile uint8_t  * const s_max_cell_idx  = (volatile uint8_t  *)0x20000430; /* max cell index */

/*
 * Discharge coulomb sub-step: drain `mag` from the remaining-capacity
 * accumulator (record +0x18), clamping at zero. Called once per protection
 * tier from the integrator's discharge branch.
 */
void coulomb_discharge_sub(uint32_t mag)
{
    if (REC_REMAINING < mag) {
        REC_REMAINING = 0;
    } else {
        REC_REMAINING -= mag;
    }
}

/*
 * Integrate one signed current sample into the capacity counters. Negative =
 * discharge: drop the remaining-capacity accumulator and run the protection-
 * tiered discharge sub-steps. Positive (>199) = charge: accumulate, roll the
 * cycle counter, and — once the run-time counter passes 0x9E33 — run the
 * capacity-learning / SOC2 estimate. The tail clamps remaining capacity and
 * recomputes SOC% from the scaled capacity ratio.
 */
void coulomb_integrate(uint32_t current)
{
    volatile uint32_t *rem  = s_remaining_cap;
    volatile uint32_t *lrn0 = s_learn_base;
    volatile uint32_t *lrnA = s_learn_accum;
    volatile uint16_t *lrnC = s_learn_count;
    volatile uint8_t  *temp = s_temp_cal;                        /* [1]/[2] temp sensors */

    if ((int32_t)current < 0) {
        uint32_t mag = ~current + 1;
        if (199 < mag) {
            if (*rem < mag) *rem = 0; else *rem -= mag;
            coulomb_discharge_sub(mag);
            if (0xC4E < *s_min_cell) {
                if (REC[0x5a] < 7) {
                    coulomb_discharge_sub(mag);
                } else if (*s_min_cell < 0xD01 && 7 < REC[0x5a]) {
                    coulomb_discharge_sub(mag);
                }
            } else {
                coulomb_discharge_sub(mag);
                coulomb_discharge_sub(mag);
            }
            if (*rem < REC_REMAINING) {
                coulomb_discharge_sub(mag);
            }
            if (*lrn0 != 0) { *lrn0 = 0; *lrnA = 0; *lrnC = 0; }
        }
    } else if (199 < current) {
        if (*lrn0 == 0 && REC[0x5a] < 100) {
            *lrn0 = *rem;
        }
        *rem += current;
        REC_REMAINING += current;
        REC_CYCLEACC += current;
        if (*s_cycle_thresh <= REC_CYCLEACC) {
            REC_CYCLEACC = 0;
            REC_CYCLECNT += 1;
        }

        if (0x9E33 < *s_run_counter) {
            if (current < 0x14B) {
                if (REC[0x5a] < 100) {
                    *lrnA += current;
                    uint16_t c = *lrnC;
                    *lrnC = c + 1;
                    if (0xEF < (uint16_t)(c + 1)) {
                        *lrnC = 0;
                        uint32_t sum = *lrnA + *lrn0;
                        *lrnA = 0;
                        *lrn0 = 0;
                        if (*s_full_cap_limit < sum) {
                            REC_FULLCAP = 0x25E4;
                            REC_REMAINING = *s_full_cap_limit;
                        } else {
                            REC_FULLCAP = ((sum) / (0x3840));
                            REC_REMAINING = sum;
                        }
                        REC[0x5a] = 100;
                        int full = (int)REC_FULLCAP;
                        int scaled = 0x25E4;
                        uint8_t lo = (temp[1] < temp[2]) ? temp[1] : temp[2];
                        if (lo < 0x3A) {
                            char d = (char)((0x3Au - lo) / (3));
                            scaled = (int)(((uint32_t)(uint8_t)(100 - d) * 0x25E4) / (100));
                        }
                        REC[0x5b] = (uint8_t)(((uint32_t)(full * 100)) / ((uint32_t)scaled));
                        if (100 < REC[0x5b]) REC[0x5b] = 100;
                    }
                } else {
                    *lrnA = 0;
                    *lrn0 = 0;
                    if (*s_full_cap_limit < REC_REMAINING) {
                        REC_FULLCAP = 0x25E4;
                        REC_REMAINING = *s_full_cap_limit;
                    } else {
                        REC_FULLCAP = ((REC_REMAINING) / (0x3840));
                    }
                    int full = (int)REC_FULLCAP;
                    int scaled = 0x25E4;
                    uint8_t lo = (temp[1] < temp[2]) ? temp[1] : temp[2];
                    if (lo < 0x3A) {
                        char d = (char)((0x3Au - lo) / (3));
                        scaled = (int)(((uint32_t)(uint8_t)(100 - d) * 0x25E4) / (100));
                    }
                    REC[0x5b] = (uint8_t)(((uint32_t)(full * 100)) / ((uint32_t)scaled));
                    if (100 < REC[0x5b]) REC[0x5b] = 100;
                    *lrnC = 0;
                }
            } else {
                *lrnA += current;
                *lrnC = 0;
            }
        } else {
            *lrnA += current;
            *lrnC = 0;
        }
    }

    /* tail: clamp remaining capacity, recompute SOC% */
    if (*s_min_cell <= *rem) {
        *rem = *s_min_cell;
    }
    if (*s_min_cell <= REC_REMAINING) {
        REC_REMAINING = *s_min_cell;
        REC_FULLCAP = 0x25E4;
    }
    REC_SOCSCRATCH = REC_REMAINING;
    if (0x383F < REC_SOCSCRATCH) {
        REC_SOCSCRATCH = ((REC_SOCSCRATCH) / (0x3840));
    } else {
        REC_SOCSCRATCH = 0;
    }
    if (REC_FULLCAP < REC_SOCSCRATCH) {
        REC_SOCSCRATCH = REC_FULLCAP;
    }
    REC[0x5a] = (uint8_t)(((uint32_t)((int)REC_SOCSCRATCH * 100)) / (REC_FULLCAP));
    if (100 < REC[0x5a]) REC[0x5a] = 100;
}

/*
 * Cell-balance governor. When the minimum cell exceeds 0xED7 (≈3.8 V) and the
 * cell spread sits in the 30..300 (mV) window, debounce three ticks then enable
 * the FEDL5236 balance bit-mask (regs 10/11) for the selected cell; otherwise
 * (or outside the window) clear the balance registers. Cell readings at
 * 0x200003D2 (this/min) and 0x200003A2 (max) are 16-bit; the debounce and cell
 * selectors are bytes.
 */
void cell_balance_update(void)
{
    volatile uint16_t *mode = s_mode;
    volatile uint8_t  *deb  = s_bal_debounce;
    volatile uint8_t  *sel  = s_bal_sel;
    uint16_t this_cell = *s_min_cell;
    uint16_t max_cell  = *s_max_cell;

    if (0xED7 < this_cell) {
        int spread = (int)((uint32_t)max_cell - (uint32_t)this_cell);
        if (spread < 0x1E || 300 < spread) {
            if (*mode & 0x20) {                          /* mode bit5 */
                *mode &= 0xFFDFu;
                fedl5236_command_write(10, 0);
                fedl5236_command_write(0x0b, 0);
                *sel = 0;
                *deb = 0;
            }
        } else {
            *mode |= 0x20;
            if ((uint8_t)(++*deb) < 3) {
                fedl5236_command_write(10, 0);
                fedl5236_command_write(0x0b, 0);
            } else {
                *sel = *s_max_cell_idx;
                *deb = 0;
                uint16_t mask = 0x10;
                if (*sel > 1) {
                    mask = (uint16_t)(0x10u << ((*sel - 1) & 0xff));
                }
                fedl5236_command_write(10, (uint8_t)mask);
                fedl5236_command_write(0x0b, (uint8_t)(mask >> 8));
            }
        }
    } else {
        if (*mode & 0x20) {
            *mode &= 0xFFDFu;
            fedl5236_command_write(10, 0);
            fedl5236_command_write(0x0b, 0);
        }
        *sel = 0;
        *deb = 0;
    }
}
