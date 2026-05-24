/* timekeeper.c — bleware monotonic-clock / wall-clock subsystem.
 *
 * Functions decoded:
 *
 *   `timekeeper_apply_request` @ 0x00020B18   — write the shared state
 *   `timekeeper_submit_epoch`  @ 0x00026CC0   — BLE-side epoch setter
 *   `timekeeper_read_be`       @ 0x00027448   — read current epoch
 *
 * The subsystem keeps a 20-byte shared state struct on the heap (the
 * pointer lives at OEM `DAT_00020B60`):
 *
 *      offset 0x00   u32   epoch_lo         (the BLE-supplied epoch)
 *      offset 0x04   u32   epoch_hi         (caller-supplied counter)
 *      offset 0x08   u32   sysclk_snapshot  (high half of FreeRTOS tick)
 *      offset 0x0C   u32   delta_ticks      (epoch tick - sysclk_low)
 *      offset 0x10   u32   delta_wrap_flag  (0 or 0xFFFFFFFF)
 *
 * Reader-side accessor `timekeeper_read_be` returns the current
 * adjusted time as a 64-bit value built by adding the latest sysclk
 * delta to the stored `epoch_*` pair (see `FUN_0001EAF8`).
 *
 * Both the writer and the reader run inside a BasePriority-raised
 * critical section (PRIMASK is left alone; only low-priority IRQs are
 * masked) because the state is also touched from a timer ISR. The
 * sysclock-snapshot helper is `FUN_000236E8` — it queries the active
 * task scheduler's clock vtable and fans the result out into a
 * 12-byte `{0, high32, low_scaled}` triple on the caller's stack. */

#include <stdint.h>

#include "bleware.h"

/* Sample the live system clock into a 12-byte snapshot on the caller's
 * stack. Layout is `{ pad0:u32, high:u32, low_scaled:u32 }`; the third
 * field is the low half of the tick count multiplied through the
 * board's tick-to-microseconds scale (DAT_0002371C).
 *
 * The OEM calls through a ROM function-pointer table (DAT_00023718 @
 * flash 0x1000018C → slot +0x30 = `Clock_getTicks` or equivalent TI-RTOS
 * ROM service) to get a 64-bit tick count, then scales the lower 16 bits
 * by the 1 000 000 000 µs/s modulus. OEM at 0x000236E8 (48 B). */
void sysclock_snapshot(uint32_t out_clock[3])
{
    /* ROM indirect: (*(*DAT_00023718 + 0x30))() → returns 64-bit in r0:r1.
     * DAT_00023718 = 0x1000018C — the TI-RTOS ROM clock function table pointer.
     * Slot +0x30 maps to ClockP_getTicks or a similar tick-source hook. */
    extern uint32_t *g_rom_clock_table;
    typedef uint64_t (*clock_get_ticks_t)(void);
    uint64_t raw_ticks;
    clock_get_ticks_t get_ticks;

    get_ticks  = (clock_get_ticks_t)*(uint32_t *)(g_rom_clock_table + 0x30u / 4u);
    raw_ticks  = get_ticks();

    out_clock[1] = (uint32_t)(raw_ticks >> 32);   /* high 32 bits */
    out_clock[0] = 0u;                              /* pad */
    {
        extern uint32_t g_timekeeper_tick_modulus;  /* DAT_0002371C = 1 000 000 000 */
        uint32_t raw_low_16  = (uint32_t)(raw_ticks >> 16) & 0xFFFFu;
        uint64_t scaled       = (uint64_t)g_timekeeper_tick_modulus * raw_low_16;
        /* pack: bits [47:32] → upper 16, bits [31:16] → lower 16 */
        out_clock[2] = ((uint32_t)(scaled >> 32) << 16)
                     | ((uint32_t)scaled >> 16);
    }
}

/* Combine a stored `{epoch_lo, epoch_hi, epoch_ticks}` request with a
 * fresh sysclock snapshot into a 64-bit "current time" reading. This
 * is the read counterpart to `timekeeper_apply_request`: it adds the
 * delta between the stored snapshot tick and the live tick to the
 * stored epoch and writes the resulting `{lo, hi, ticks}` triple back
 * into the caller-supplied scratch. */
extern void timekeeper_read_request(uint32_t scratch[3]);          /* FUN_0001EAF8 */

#define TIMEKEEPER_BASEPRI_MASK   0x20u

/* Cortex-M BASEPRI helpers — masked-IRQ critical section. */
static inline uint32_t basepri_raise(uint32_t mask)
{
    uint32_t prev;
    __asm__ volatile (
        "mrs %0, basepri\n\t"
        "msr basepri, %1"
        : "=&r" (prev)
        : "r"   (mask)
        : "memory");
    return prev;
}

static inline void basepri_restore(uint32_t prev)
{
    __asm__ volatile ("msr basepri, %0" :: "r" (prev) : "memory");
}

/* OEM `DAT_00020B60` — pointer to the 20-byte shared state (heap-
 * allocated at module init). Declared weak so the link succeeds while
 * the init path is still stubbed. */
extern uint32_t *g_timekeeper_state;                              /* DAT_00020B60 */

/* Apply a 12-byte timekeeper request to the shared state. The request
 * carries the BLE-supplied `{epoch_lo, epoch_hi, epoch_ticks}`; this
 * routine snapshots the live system clock, then folds the request and
 * snapshot together into the 20-byte shared state. OEM at 0x00020B18.
 *
 * The OEM stores the request fields in a deliberately swapped order
 * (epoch_lo at +0x04, epoch_hi at +0x00) so the reader can pull a
 * 64-bit value with a single little-endian load. */
int timekeeper_apply_request(const uint32_t request[3])
{
    uint32_t snapshot[3];
    uint32_t prev_basepri = basepri_raise(TIMEKEEPER_BASEPRI_MASK);

    sysclock_snapshot(snapshot);

    uint32_t *state = g_timekeeper_state;
    state[1] = request[0];     /* epoch_lo */
    state[0] = request[1];     /* epoch_hi */
    state[2] = snapshot[1];    /* sysclk high half */

    uint32_t delta;
    uint32_t wrap_flag;
    if (request[2] >= snapshot[2]) {
        delta     = request[2] - snapshot[2];
        wrap_flag = 0u;
    } else {
        extern uint32_t g_timekeeper_tick_modulus;            /* DAT_00020B64 */
        delta     = (request[2] - snapshot[2]) + g_timekeeper_tick_modulus;
        wrap_flag = 0xFFFFFFFFu;
    }
    state[3] = delta;
    state[4] = wrap_flag;

    basepri_restore(prev_basepri);
    return 0;
}

void timekeeper_submit_epoch(uint32_t epoch)
{
    uint32_t request[3];
    request[0] = 0u;
    request[1] = epoch;
    request[2] = 0u;
    timekeeper_apply_request(request);
}

/* Read the current adjusted time as a packed 64-bit value (epoch_hi
 * in the high 32 bits, epoch_lo in the low 32). OEM at 0x00027448. */
uint64_t timekeeper_read_be(void)
{
    uint32_t scratch[3];
    scratch[0] = 0u;
    scratch[1] = 0u;
    scratch[2] = 0u;
    timekeeper_read_request(scratch);
    return ((uint64_t)scratch[0] << 32) | scratch[1];
}
