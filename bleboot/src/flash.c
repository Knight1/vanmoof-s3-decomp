#include <stdint.h>

#include "bim.h"

/* Flash-session begin (`FUN_00056A88` in the OEM). Called as the
 * gate at every BIM entry point that touches flash:
 * `bim_full_scan_and_launch`, `bim_verify_and_launch_image`,
 * `bim_crc32_image` (when its `use_flash` arg is non-zero). Returns
 * 1 on success (caller may proceed), 0 on failure (caller bails).
 *
 * The complementary "release" routine is `FUN_000570AC`, called by
 * every flash-using path after the operation completes (and by
 * this function on the failure branch). Together they implement a
 * begin/end bracket around flash MMIO access.
 *
 * Five-stage setup:
 *
 *   1. Configure something via `FUN_000563C8` with the literal
 *      `0x003D0900` (= 4,000,000 decimal — likely a 4 MHz clock
 *      reference) and a count/divisor of 9.
 *
 *   2. Two ROM-API calls into the flash sub-table at ROM
 *      `0x100001B4` (4 bytes earlier than `bim_panic_prep`'s
 *      `0x100001B8`, so an adjacent sub-table in the standard
 *      `ROM_API_TABLE` array). Index 15 (byte offset 60) called
 *      with args `4` then `3` — looks like a two-step state
 *      machine (e.g., wake from low-power, then arm sense
 *      amplifier). Exact API identity not yet pinned.
 *
 *   3. Light DIO3 via `GPIO_DOUTSET31_0` — the flash-session
 *      busy LED. Distinct from DIO2 (`bim_panic_indicate`) and
 *      DIO4 (`FUN_00057138` / `FUN_00057188` pair).
 *
 *   4. Call `FUN_00057138` (8 B + 4 B literal) — known to write
 *      `16 = 1<<4` to `GPIO_DOUTSET31_0`, i.e. set DIO4 as well.
 *      So both DIO3 and DIO4 light during a flash session.
 *
 *   5. Two-stage readiness probe: `FUN_00056D6A()` must return
 *      non-zero, then `FUN_0005698C()` provides the actual
 *      success flag. If the first stage fails, we release the
 *      partially-acquired session via `FUN_000570AC` and report
 *      failure. */

#define GPIO_DOUTSET31_0      (*(volatile uint32_t *)0x40022090u)
#define DIO_FLASH_BUSY_LED    (1u << 3)

#define BIM_FLASH_ROM_TABLE   (*(const uintptr_t *const *)0x100001B4u)

extern int  FUN_000563C8(uint32_t a, uint32_t b);
extern void FUN_00057138(void);
extern int  FUN_00056D6A(void);
extern int  FUN_0005698C(void);
extern void FUN_000570AC(void);

int bim_flash_prepare(void)
{
    FUN_000563C8(0x003D0900u, 9u);

    ((void (*)(uint32_t))BIM_FLASH_ROM_TABLE[15])(4u);
    ((void (*)(uint32_t))BIM_FLASH_ROM_TABLE[15])(3u);

    GPIO_DOUTSET31_0 = DIO_FLASH_BUSY_LED;

    FUN_00057138();

    if (FUN_00056D6A() == 0) {
        FUN_000570AC();
        return 0;
    }

    return FUN_0005698C();
}
