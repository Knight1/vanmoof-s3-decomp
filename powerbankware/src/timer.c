#include "powerbankware.h"

/*
 * timer_start_it — OEM FUN_0801CF70.
 *
 * Start a timer behind a HAL handle: the handle's first member is the
 * peripheral base, +0x0C is DIER (set UIE) and +0x00 is CR1 (set CEN). Used by
 * the state super-loop to arm the periodic interrupt. Returns 0 (OEM keeps the
 * HAL_OK convention).
 */
uint32_t timer_start_it(uint32_t *handle)
{
    volatile uint32_t *base = *(volatile uint32_t **)handle;
    base[3] |= 1u;   /* DIER (+0x0C): UIE  */
    base[0] |= 1u;   /* CR1  (+0x00): CEN  */
    return 0;
}
