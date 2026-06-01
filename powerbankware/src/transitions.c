#include "powerbankware.h"

/*
 * BMS state-entry / protection-transition sequences. Each routine drives the
 * gate-driver GPIOs into the safe pattern for the state it enters, programs the
 * FEDL5236 reg-9 FET-control byte (shadowed at 0x20000412), and hands off to the
 * state-machine dispatcher bms_state_enter(state). They share one shape so the
 * binary diff stays meaningful; each is a separate OEM function and is kept
 * inline (not factored) to preserve the emitted call structure.
 *
 * GPIO map (verified): AFE CS-adjacent PA7 (GPIOA 0x80); FET/LED lines on GPIOB
 * PB0 (1), PB9 (0x200), PB11 (0x800). The bypass FET (PA12) is driven via
 * bypass_fet_on/off (src/vout.c).
 *
 * Fault-bit -> entry mapping (from the state-1 protection dispatch):
 *   OVP1 -> bms_enter_ovp1 (7)   OVP2 -> bms_enter_ovp2 (8)
 *   UVP1 -> bms_enter_uvp1 (9)   UVP2 -> bms_enter_uvp2 (10)
 *   imbalance/OV2/MOS -> bms_enter_ov2_record (0x17/0x18/0x19)
 * Temperature protections (calibration-command entries too, from main):
 *   COTP (0x12) CUTP (0x13) DOTP (0x14) DUTP (0x15)
 *
 *   bms_enter_state_b  = FUN_08008FF0   bms_enter_state_c  = FUN_080091C0
 *   bms_enter_cotp     = FUN_080093AC   bms_enter_cutp     = FUN_08009598
 *   bms_enter_state_d  = FUN_0800A714   bms_enter_state_e  = FUN_0800A8E4
 *   bms_enter_dotp     = FUN_0800AA98   bms_enter_dutp     = FUN_0800AC44
 *   bms_enter_state_16 = FUN_0800F4A4   bms_enter_ovp1     = FUN_080104FC
 *   bms_enter_ovp2     = FUN_080106B4   bms_enter_afe_latch= FUN_08010BE4
 *   bms_enter_uvp1     = FUN_08015C68   bms_enter_uvp2     = FUN_08015E1C
 *   bms_enter_idle3    = FUN_0800B49C   bms_enter_ov2_record = FUN_0800F258
 *   boot_enter_operating = FUN_08009AA4 bms_state_noop     = FUN_0800B476
 *   bms_enter_afe_fault  = FUN_08008E2C
 */

#define GPIOA 0x48000000u
#define GPIOB 0x48000400u
#define FETB  (*(volatile uint8_t *)0x20000412)

/* boot-path bypass log strings (src/strings.c). */
extern const char s_bypass_on[];           /* "\nByPass On\r"                 0x0801DB94 */
extern const char s_bypass_off_batt_low[]; /* "\nByPass Off --> Battery Low\r" 0x0801DBA0 */
extern const char s_bypass_off[];          /* "\nByPass Off\r"                0x0801DBC0 */

/* The common front porch shared by every entry: CS PA7 low, PB0 high, PB11 low. */
static inline void entry_porch(void)
{
    gpio_bit_write(GPIOA, 0x80, 0);
    gpio_bit_write(GPIOB, 1, 1);
    gpio_bit_write(GPIOB, 0x800, 0);
}

/* ---- bypass-ON protections (charge-side) ------------------------------- */

void bms_enter_state_b(void)
{
    entry_porch();
    bypass_fet_on();
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 1;
    fedl5236_command_write(9, FETB);
    bms_state_enter(0xb);
}

void bms_enter_state_c(void)
{
    entry_porch();
    bypass_fet_on();
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 1;
    fedl5236_command_write(9, FETB);
    bms_state_enter(0xc);
}

void bms_enter_cotp(void)
{
    entry_porch();
    bypass_fet_on();
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 1;
    fedl5236_command_write(9, FETB);
    bms_state_enter(0x12);
}

void bms_enter_cutp(void)
{
    entry_porch();
    bypass_fet_on();
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 1;
    fedl5236_command_write(9, FETB);
    bms_state_enter(0x13);
}

/* ---- bypass-OFF protections (discharge-side / temperature) -------------- */

void bms_enter_state_d(void)
{
    entry_porch();
    bypass_fet_off();
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 2;
    fedl5236_command_write(9, FETB);
    bms_state_enter(0xd);
}

void bms_enter_state_e(void)
{
    entry_porch();
    bypass_fet_off();
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 2;
    fedl5236_command_write(9, FETB);
    bms_state_enter(0xe);
}

void bms_enter_dotp(void)
{
    entry_porch();
    bypass_fet_off();
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 0;
    fedl5236_command_write(9, FETB);
    bms_state_enter(0x14);
}

void bms_enter_dutp(void)
{
    entry_porch();
    bypass_fet_off();
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 0;
    fedl5236_command_write(9, FETB);
    bms_state_enter(0x15);
}

void bms_enter_state_16(void)
{
    entry_porch();
    bypass_fet_off();
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 0;
    fedl5236_command_write(9, FETB);
    bms_state_enter(0x16);
}

void bms_enter_afe_latch(void)
{
    entry_porch();
    bypass_fet_off();
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 2;
    fedl5236_command_write(9, FETB);
    bms_state_enter(0x11);
}

void bms_enter_uvp1(void)
{
    entry_porch();
    bypass_fet_off();
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 2;
    fedl5236_command_write(9, FETB);
    bms_state_enter(9);
}

void bms_enter_uvp2(void)
{
    entry_porch();
    bypass_fet_off();
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 2;
    fedl5236_command_write(9, FETB);
    bms_state_enter(10);
}

/* OVP1/OVP2 additionally re-open the front-porch lines after the transition. */
void bms_enter_ovp1(void)
{
    entry_porch();
    bypass_fet_off();
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 1;
    fedl5236_command_write(9, FETB);
    bms_state_enter(7);
    gpio_bit_write(GPIOB, 1, 0);
    gpio_bit_write(GPIOB, 0x800, 1);
    bypass_fet_off();
}

void bms_enter_ovp2(void)
{
    entry_porch();
    bypass_fet_off();
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 1;
    fedl5236_command_write(9, FETB);
    bms_state_enter(8);
    gpio_bit_write(GPIOB, 1, 0);
    gpio_bit_write(GPIOB, 0x800, 1);
    bypass_fet_off();
}

/* Idle/state-3 entry: bypass on after the FET write, zero two soft counters. */
void bms_enter_idle3(void)
{
    gpio_bit_write(GPIOA, 0x80, 0);
    gpio_bit_write(GPIOB, 1, 1);
    gpio_bit_write(GPIOB, 0x800, 0);
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 1;
    fedl5236_command_write(9, FETB);
    bypass_fet_on();
    *(volatile uint16_t *)0x200003CC = 0;
    *(volatile uint16_t *)0x2000042C = 0;
    bms_state_enter(3);
}

/*
 * Imbalance / OV-2nd / MOS-failure record entry. If a charger-present request
 * bit (alarm word 0x200006E4 bit6/bit7) is set while mode bit13 is clear, raise
 * PB7 first. Then the standard porch, and a 3-way state select: fault bit11 set
 * -> 0x19; else charger-present -> 0x17; else -> 0x18.
 */
void bms_enter_ov2_record(void)
{
    volatile uint16_t *req   = (volatile uint16_t *)0x200006E4;
    volatile uint16_t *mode  = (volatile uint16_t *)0x200006A0;
    volatile uint16_t *fault = (volatile uint16_t *)0x20000410;

    if (((*req & 0x40) || (*req & 0x80)) && !(*mode & 0x2000)) {  /* req bit6/7, mode bit13 clear */
        gpio_bit_write(GPIOB, 0x80, 1);    /* PB7 high */
    }
    gpio_bit_write(GPIOA, 0x80, 0);
    gpio_bit_write(GPIOB, 1, 1);
    gpio_bit_write(GPIOB, 0x800, 0);
    bypass_fet_off();
    gpio_bit_write(GPIOB, 0x200, 0);
    FETB = 0;
    fedl5236_command_write(9, FETB);

    if (*fault & 0x800) {                   /* fault bit11 */
        bms_state_enter(0x19);
    } else if ((*req & 0x40) || (*req & 0x80)) {
        bms_state_enter(0x17);
    } else {
        bms_state_enter(0x18);
    }
}

/*
 * Normal boot into the operating state (state 2). `from_recovery` non-zero takes
 * the bypass-on path (mode bit12 set, no battery-low warning); zero picks
 * "ByPass Off" or "ByPass Off --> Battery Low" by the boot counter vs 0x752F.
 */
void boot_enter_operating(int from_recovery)
{
    *(volatile uint16_t *)0x200003C8 = 4000;
    *(volatile uint16_t *)0x200003BC = 0;
    gpio_bit_write(GPIOB, 1, 1);
    gpio_bit_write(GPIOB, 0x800, 0);
    bypass_fet_off();

    if (from_recovery == 0) {
        if (*(volatile uint16_t *)0x200003CE > 0x752F) {   /* boot counter high */
            gpio_bit_write(GPIOB, 0x200, 1);
            FETB = 2;
            fedl5236_command_write(9, FETB);
            bms_state_enter(2);
            log_print(2, s_bypass_off);
            *(volatile uint16_t *)0x20000220 = 0x3C;
        } else {
            gpio_bit_write(GPIOB, 0x200, 0);
            FETB = 2;
            fedl5236_command_write(9, FETB);
            bms_state_enter(2);
            log_print(2, s_bypass_off_batt_low);
            *(volatile uint16_t *)0x20000220 = 0x3C;
        }
    } else {
        gpio_bit_write(GPIOB, 0x200, 0);
        FETB = 0;
        fedl5236_command_write(9, FETB);
        bms_state_enter(2);
        log_print(2, s_bypass_on);
        *(volatile uint16_t *)0x200006A0 |= 0x1000;   /* mode bit12 */
        *(volatile uint16_t *)0x20000220 = 0;
    }

    gpio_bit_write(GPIOA, 0x80, 1);
    gpio_bit_write(GPIOA, 0x200, 0);
}

/*
 * AFE-fault latch entry (state 0x1B): drop PB7, run the standard porch, open the
 * bypass FET off, drop PB9, and enter 0x1B. Reached from the charger refresh
 * path when the FEDL5236 FET-control read-back keeps failing.
 */
void bms_enter_afe_fault(void)
{
    gpio_bit_write(GPIOB, 0x80, 0);    /* PB7 low */
    entry_porch();
    bypass_fet_off();
    gpio_bit_write(GPIOB, 0x200, 0);   /* PB9 low */
    bms_state_enter(0x1b);
}

/* Empty OEM hook (optimised to a bare return in this image); the state handlers
 * call it as a branch-balanced no-op. */
void bms_state_noop(void)
{
}
