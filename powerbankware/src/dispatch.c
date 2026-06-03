#include "powerbankware.h"

/*
 * bms_state_enter — OEM FUN_0800F7B4.
 *
 * The state-machine transition routine: every protection/operating entry in
 * src/transitions.c (and the boot path in src/bms.c) hands off here with the
 * state index it wants to commit. It saves the previous state, latches the new
 * one, clears the per-state soft counters and alarm debounces, parks the power
 * path (vout_bypass_off, drive PA9 high, clear mode bits 7/12, and — if the
 * cell-balance flag bit 5 is set — disable balancing via FEDL5236 regs 10/11),
 * then, when the event log is enabled (0x2000072C != 0), bumps the per-state
 * event counter and writes one circular trace record to the EEPROM error log.
 * It ends by re-running the periodic hook, saving the AFE record, and logging
 * the new/previous state.
 *
 * Reconstructed from the disassembly: the decompiler could not recover the two
 * jump tables (0x0801E75C main, 0x0801E7CC event-code), so the original renders
 * as a mangled indirect call; both are plain switch(state) blocks here.
 *
 * Callee kept extern (deeper leaf, own pass):
 *   rtc_timestamp_read  read the BCD-decoded RTC time/date into an 8-byte buffer
 *
 * The OEM circular-trace index math used the compiler's signed divmod helper
 * FUN_08008410 (= __aeabi_idivmod); expressed here as a native `%` so the
 * toolchain emits the identical routine.
 */

#define MODE    (*(volatile uint16_t *)0x200006A0)
#define STATEB  (*(volatile uint8_t  *)0x200005AC)   /* current state */
#define PREVB   (*(volatile uint8_t  *)0x200004B0)   /* previous state */
#define LOGGING (*(volatile uint32_t *)0x2000072C != 0)

#define REC ((volatile uint8_t *)0x200004D0)         /* BMS record */
#define LOG ((volatile uint8_t *)0x200005B0)         /* circular trace entry */

/* ── GPIO / RTC handle ───────────────────────────────────────────────── */
#define GPIOA_BASE  0x48000000u
#define RTC_HANDLE  ((void *)0x200006F0)

/* ── Identity / wake word (+ complement mirror) ──────────────────────── */
#define IDBLK     (*(volatile uint32_t *)0x20000724)   /* identity/wake word; [1]=state */
#define IDBLK_INV (*(volatile uint32_t *)0x20000698)   /* ~idblk mirror */

/* ── AFE / measurement cells (owned by fedl5236.c, charger.c, alarms.c) ─ */
#define FET_STATUS  (*(volatile uint8_t  *)0x200003A5)  /* cached FET/balance status */
#define AFE_BUF     ((volatile uint8_t  *)0x20000614)   /* AFE RX frame buffer       */
#define CELL_MV     ((volatile uint16_t *)0x20000380)   /* 10 cell voltages          */
#define CELL_SUM    (*(volatile uint16_t *)0x200003CE)  /* pack-voltage sum          */
#define CUR_AVG     (*(volatile int32_t  *)0x20000424)  /* signed avg current        */
#define LIMIT       ((volatile uint8_t  *)0x20000218)   /* cal/limit block; [0..2]   */
#define BAL_SEL     (*(volatile uint8_t  *)0x20000224)  /* selected balance cell     */
#define BAL_DEBNC   (*(volatile uint8_t  *)0x2000041C)  /* balance debounce tick     */

/* ── Per-state soft counters / debounces cleared on every entry ──────── */
#define S_UPTIME_1S  (*(volatile uint32_t *)0x20000594)  /* uptime seconds        */
#define S_REQ        (*(volatile uint8_t  *)0x200006E4)  /* request/REQ low byte  */
#define S_TCNT_550   (*(volatile uint16_t *)0x20000550)  /* temp/ship debounce    */
#define S_TCNT_584   (*(volatile uint16_t *)0x20000584)  /* temp-recovery debounce*/
#define S_TCNT_580   (*(volatile uint16_t *)0x20000580)  /* temp/ship debounce    */
#define S_TCNT_4CA   (*(volatile uint16_t *)0x200004CA)  /* temp-recovery debounce*/
#define S_TCNT_588   (*(volatile uint16_t *)0x20000588)  /* temp-recovery debounce*/
#define S_TCNT_4BC   (*(volatile uint16_t *)0x200004BC)  /* soft counter          */
#define S_TCNT_5AE   (*(volatile uint16_t *)0x200005AE)  /* soft counter          */
#define S_TCNT_4B8   (*(volatile uint16_t *)0x200004B8)  /* soft counter          */
#define S_SHIP_CNT   (*(volatile uint16_t *)0x200006A2)  /* ship-mode tick counter*/
#define S_CNT_41B    (*(volatile uint8_t  *)0x2000041B)  /* soft counter          */
#define S_CHG_CNT    (*(volatile uint16_t *)0x200006A8)  /* charger present/absent*/
#define S_NOLOAD2    (*(volatile uint16_t *)0x2000069E)  /* no-load (bit12) counter*/
#define S_DDON_CNT   (*(volatile uint8_t  *)0x20000720)  /* DD-on tick counter    */
#define S_BTN_DEB    (*(volatile uint8_t  *)0x20000710)  /* PB8 button debounce   */
#define S_BLINK      (*(volatile uint16_t *)0x20000728)  /* blink counter         */
#define S_TEST_MODE  (*(volatile uint8_t  *)0x2000069C)  /* 0 = normal, else test */
#define S_CNT_248    (*(volatile uint32_t *)0x20000248)  /* soft counter          */
#define S_SOFTCNT_BC (*(volatile uint16_t *)0x200006BC)  /* per-state soft counter*/

/* ── Per-state diagnostic event counters ─────────────────────────────── */
#define EVT_582   (*(volatile uint8_t *)0x20000582)
#define EVT_554   (*(volatile uint8_t *)0x20000554)

/* transition-trace log strings (src/strings.c). */
extern const char s_new_state[];   /* "\nNew State = %d\r"      0x0801E0F4 */
extern const char s_prev_state[];  /* "\nPrevious State = %d\r" 0x0801E108 */

static inline volatile uint16_t *rec16(uint32_t off)
{
    return (volatile uint16_t *)(0x200004D0u + off);
}

/*
 * state_persist_to_backup — OEM FUN_08014280. Pack {0, state, prev-state} into
 * the low three bytes of the identity/state word 0x20000724 (byte 3 keeps the
 * mode/balance flags), store its 32-bit complement at 0x20000698, and persist
 * both words into RTC backup registers 0/1 (via rtc_backup_write, RTC handle at
 * 0x200006F0) so the state machine survives a reset/brownout. Only the
 * dispatcher calls it.
 */
static void state_persist_to_backup(uint8_t state)
{
    volatile uint8_t *packed = (volatile uint8_t *)&IDBLK;

    packed[0] = 0;
    packed[1] = state;
    packed[2] = PREVB;                                  /* previous state */
    IDBLK_INV = ~IDBLK;
    rtc_backup_write(RTC_HANDLE, 0, IDBLK);
    rtc_backup_write(RTC_HANDLE, 1, IDBLK_INV);
}

/*
 * afe_fet_status_refresh — OEM FUN_0800E1AC. Post-transition hook: re-read the
 * FEDL5236 FET-control read-back register (reg 0x0D) and, on a good read, cache
 * the returned FET/balance status byte (AFE buffer +2) to 0x200003A5. Only the
 * dispatcher calls it.
 */
static void afe_fet_status_refresh(void)
{
    if (fedl5236_read_data(0x0d, 1) != 0) {
        FET_STATUS = AFE_BUF[2];
    }
}

void bms_state_enter(int state_in)
{
    uint8_t state = (uint8_t)state_in;

    MODE &= 0xEFFFu;                 /* clear mode bit 12 */
    PREVB = STATEB;
    STATEB = state;

    /* clear the per-state soft counters and alarm debounces */
    S_UPTIME_1S  = 0;
    S_REQ        = 0;
    S_TCNT_550   = 0;
    S_TCNT_584   = 0;
    S_TCNT_580   = 0;
    S_TCNT_4CA   = 0;
    S_TCNT_588   = 0;
    S_TCNT_4BC   = 0;
    S_TCNT_5AE   = 0;
    S_TCNT_4B8   = 0;
    S_SHIP_CNT   = 0;
    S_CNT_41B    = 0;
    S_CHG_CNT    = 0;
    S_NOLOAD2    = 0;
    S_DDON_CNT   = 0;
    S_BTN_DEB    = 0;
    S_BLINK      = 0;
    S_TEST_MODE  = 0;
    S_CNT_248    = 0;
    S_SOFTCNT_BC = 0;

    vout_bypass_off();
    gpio_bit_write(GPIOA_BASE, 0x200, 1);   /* PA9 high */
    MODE &= 0xFF7Fu;                          /* clear mode bit 7 */

    if (MODE & 0x20u) {                       /* cell-balance active (bit 5) */
        MODE &= 0xFFDFu;
        fedl5236_command_write(10, 0);
        fedl5236_command_write(11, 0);
        BAL_SEL   = 0;
        BAL_DEBNC = 0;
    }

    /* per-state diagnostic event counters (event log enabled only) */
    switch (state) {
    case 2:
        EVT_582 = 0;
        IDBLK &= 0xFEFFFFFFu;
        break;
    case 3:
        EVT_554 = 0;
        break;
    case 7:  if (LOGGING) (*rec16(0x40))++; break;
    case 8:  if (LOGGING) (*rec16(0x42))++; break;
    case 9:
        if (LOGGING) {
            uint32_t ts[2];
            (*rec16(0x44))++;
            rtc_timestamp_read(ts);
            *(volatile uint32_t *)(REC + 0x10) = ts[0];
            *(volatile uint32_t *)(REC + 0x14) = ts[1];
        }
        break;
    case 0xa:
        if (LOGGING) {
            uint32_t ts[2];
            (*rec16(0x46))++;
            rtc_timestamp_read(ts);
            *(volatile uint32_t *)(REC + 0x10) = ts[0];
            *(volatile uint32_t *)(REC + 0x14) = ts[1];
        }
        break;
    case 0xb:
        if (LOGGING) { EVT_554++; (*rec16(0x3c))++; }
        break;
    case 0xc:
        if (LOGGING) { EVT_554++; (*rec16(0x3e))++; }
        break;
    case 0xd:  if (LOGGING) (*rec16(0x38))++; break;
    case 0xe:  if (LOGGING) (*rec16(0x3a))++; break;
    case 0xf:
        if (LOGGING) { EVT_582++; (*rec16(0x48))++; }
        break;
    case 0x10: if (LOGGING) (*rec16(0x4a))++; break;
    case 0x11: if (LOGGING) (*rec16(0x4e))++; break;
    case 0x12: if (LOGGING) (*rec16(0x34))++; break;
    case 0x13: if (LOGGING) (*rec16(0x36))++; break;
    case 0x14: if (LOGGING) (*rec16(0x30))++; break;
    case 0x15: if (LOGGING) (*rec16(0x32))++; break;
    case 0x16: if (LOGGING) (*rec16(0x4c))++; break;
    default:   break;
    }

    /* circular trace record (states 7..0x1b, event log enabled) */
    if (state >= 7 && state <= 0x1b && LOGGING) {
        uint16_t evt;
        switch (state) {
        case 7:    evt = 0x80;   break;
        case 8:    evt = 0x40;   break;
        case 9:    evt = 0x20;   break;
        case 0xa:  evt = 0x10;   break;
        case 0xb:  evt = 0x200;  break;
        case 0xc:  evt = 0x100;  break;
        case 0xd:  evt = 0x800;  break;
        case 0xe:  evt = 0x400;  break;
        case 0x11: evt = 1;      break;
        case 0x12: evt = 0x2000; break;
        case 0x13: evt = 0x1000; break;
        case 0x14: evt = 0x8000; break;
        case 0x15: evt = 0x4000; break;
        case 0x16: evt = 2;      break;
        case 0x17: evt = 0xffff; break;
        case 0x18: evt = 0xc0;   break;
        case 0x1a: evt = 8;      break;
        case 0x1b: evt = 4;      break;
        default:   evt = 0;      break;   /* 0xf, 0x10, 0x19 */
        }

        uint16_t idx = *rec16(0x2a) + 1;
        *rec16(0x2a) = idx;
        if (idx > 0xfde7) {
            *rec16(0x2a) = 0;
        }

        *(volatile uint16_t *)(LOG + 0x0c) = *rec16(0x2a);
        *(volatile uint16_t *)(LOG + 0x0e) = evt;
        LOG[0x2a] = LIMIT[1];
        LOG[0x2b] = LIMIT[2];
        LOG[0x2c] = LIMIT[0];
        *(volatile uint16_t *)(LOG + 0x26) = CELL_SUM;
        *(volatile uint32_t *)(LOG + 0x00) = (uint32_t)CUR_AVG;
        *(volatile uint32_t *)(LOG + 0x04) = *(volatile uint32_t *)(REC + 0x1c);
        *(volatile uint32_t *)(LOG + 0x08) = *(volatile uint32_t *)(REC + 0x20);
        LOG[0x28] = REC[0x5a];
        LOG[0x29] = REC[0x5b];
        *(volatile uint16_t *)(LOG + 0x10) = *(volatile uint16_t *)(REC + 0x50);
        for (uint8_t i = 0; i < 10; i++) {
            *(volatile uint16_t *)(LOG + 0x12 + i * 2) = CELL_MV[i];
        }

        idx = *rec16(0x2a);
        errlog_erase((int16_t)(((int32_t)idx - 1) % 1000));
    }

    afe_fet_status_refresh();
    fedl5236_record_save();
    state_persist_to_backup(STATEB);
    log_print(2, s_new_state, STATEB);
    log_print(2, s_prev_state, PREVB);
    uart_flush();
}
