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
    volatile uint8_t *packed = (volatile uint8_t *)0x20000724;

    packed[0] = 0;
    packed[1] = state;
    packed[2] = *(volatile uint8_t *)0x200004B0;        /* previous state */
    *(volatile uint32_t *)0x20000698 = ~*(volatile uint32_t *)0x20000724;
    rtc_backup_write((void *)0x200006F0, 0, *(volatile uint32_t *)0x20000724);
    rtc_backup_write((void *)0x200006F0, 1, *(volatile uint32_t *)0x20000698);
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
        *(volatile uint8_t *)0x200003A5 = *(volatile uint8_t *)(0x20000614 + 2);
    }
}

void bms_state_enter(int state_in)
{
    uint8_t state = (uint8_t)state_in;

    MODE &= 0xEFFFu;                 /* clear mode bit 12 */
    PREVB = STATEB;
    STATEB = state;

    /* clear the per-state soft counters and alarm debounces */
    *(volatile uint32_t *)0x20000594 = 0;
    *(volatile uint8_t  *)0x200006E4 = 0;
    *(volatile uint16_t *)0x20000550 = 0;
    *(volatile uint16_t *)0x20000584 = 0;
    *(volatile uint16_t *)0x20000580 = 0;
    *(volatile uint16_t *)0x200004CA = 0;
    *(volatile uint16_t *)0x20000588 = 0;
    *(volatile uint16_t *)0x200004BC = 0;
    *(volatile uint16_t *)0x200005AE = 0;
    *(volatile uint16_t *)0x200004B8 = 0;
    *(volatile uint16_t *)0x200006A2 = 0;
    *(volatile uint8_t  *)0x2000041B = 0;
    *(volatile uint16_t *)0x200006A8 = 0;
    *(volatile uint16_t *)0x2000069E = 0;
    *(volatile uint8_t  *)0x20000720 = 0;
    *(volatile uint8_t  *)0x20000710 = 0;
    *(volatile uint16_t *)0x20000728 = 0;
    *(volatile uint8_t  *)0x2000069C = 0;
    *(volatile uint32_t *)0x20000248 = 0;
    *(volatile uint16_t *)0x200006BC = 0;

    vout_bypass_off();
    gpio_bit_write(0x48000000u, 0x200, 1);   /* PA9 high */
    MODE &= 0xFF7Fu;                          /* clear mode bit 7 */

    if (MODE & 0x20u) {                       /* cell-balance active (bit 5) */
        MODE &= 0xFFDFu;
        fedl5236_command_write(10, 0);
        fedl5236_command_write(11, 0);
        *(volatile uint8_t *)0x20000224 = 0;
        *(volatile uint8_t *)0x2000041C = 0;
    }

    /* per-state diagnostic event counters (event log enabled only) */
    switch (state) {
    case 2:
        *(volatile uint8_t  *)0x20000582 = 0;
        *(volatile uint32_t *)0x20000724 &= 0xFEFFFFFFu;
        break;
    case 3:
        *(volatile uint8_t *)0x20000554 = 0;
        break;
    case 7:  if (LOGGING) (*rec16(0x40))++; break;
    case 8:  if (LOGGING) (*rec16(0x42))++; break;
    case 9:
        if (LOGGING) {
            uint32_t ts[2];
            (*rec16(0x44))++;
            rtc_timestamp_read(ts);
            *(volatile uint32_t *)(0x200004D0 + 0x10) = ts[0];
            *(volatile uint32_t *)(0x200004D0 + 0x14) = ts[1];
        }
        break;
    case 0xa:
        if (LOGGING) {
            uint32_t ts[2];
            (*rec16(0x46))++;
            rtc_timestamp_read(ts);
            *(volatile uint32_t *)(0x200004D0 + 0x10) = ts[0];
            *(volatile uint32_t *)(0x200004D0 + 0x14) = ts[1];
        }
        break;
    case 0xb:
        if (LOGGING) { (*(volatile uint8_t *)0x20000554)++; (*rec16(0x3c))++; }
        break;
    case 0xc:
        if (LOGGING) { (*(volatile uint8_t *)0x20000554)++; (*rec16(0x3e))++; }
        break;
    case 0xd:  if (LOGGING) (*rec16(0x38))++; break;
    case 0xe:  if (LOGGING) (*rec16(0x3a))++; break;
    case 0xf:
        if (LOGGING) { (*(volatile uint8_t *)0x20000582)++; (*rec16(0x48))++; }
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
        LOG[0x2a] = *(volatile uint8_t *)(0x20000218 + 1);
        LOG[0x2b] = *(volatile uint8_t *)(0x20000218 + 2);
        LOG[0x2c] = *(volatile uint8_t *)0x20000218;
        *(volatile uint16_t *)(LOG + 0x26) = *(volatile uint16_t *)0x200003CE;
        *(volatile uint32_t *)(LOG + 0x00) = *(volatile uint32_t *)0x20000424;
        *(volatile uint32_t *)(LOG + 0x04) = *(volatile uint32_t *)(0x200004D0 + 0x1c);
        *(volatile uint32_t *)(LOG + 0x08) = *(volatile uint32_t *)(0x200004D0 + 0x20);
        LOG[0x28] = REC[0x5a];
        LOG[0x29] = REC[0x5b];
        *(volatile uint16_t *)(LOG + 0x10) = *(volatile uint16_t *)(0x200004D0 + 0x50);
        for (uint8_t i = 0; i < 10; i++) {
            *(volatile uint16_t *)(LOG + 0x12 + i * 2) =
                *(volatile uint16_t *)(0x20000380 + i * 2);
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
