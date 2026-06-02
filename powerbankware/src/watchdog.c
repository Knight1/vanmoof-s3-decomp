#include "powerbankware.h"

/*
 * iwdg_init — OEM FUN_08013820.
 *
 * Independent watchdog bring-up, called from hal_bringup. Fills the IWDG HAL
 * handle (@0x200006ac), runs HAL_IWDG_Init, then writes the reload key so the
 * counter starts from its full value. The handle's Instance member is later
 * dereferenced by the OTA inner loop to kick the dog (ota.c).
 *
 * Field offsets and constants below are disasm-confirmed against the OEM image.
 */

/*
 * hal_iwdg_init — OEM FUN_0801b488 (HAL_IWDG_Init).
 *
 * Handle layout: [0]=Instance(IWDG), [1]=Prescaler, [2]=Reload, [3]=Window.
 * IWDG registers off Instance: KR +0x00, PR +0x04, RLR +0x08, SR +0x0c,
 * WINR +0x10. Writes the start/enable keys, loads PR/RLR, waits (≤40 ms) for
 * pending register updates to clear, then sets the window (or reloads if it is
 * already correct). Keys/offsets/widths disasm-confirmed against the OEM image.
 */
int hal_iwdg_init(void *hiwdg)
{
    if (hiwdg == NULL) {
        return 1;                                  /* HAL_ERROR */
    }
    uint32_t *h = (uint32_t *)hiwdg;
    volatile uint32_t *iwdg = (volatile uint32_t *)h[0];   /* Instance */

    iwdg[0] = 0x0000ccccu;                          /* KR:  start watchdog       */
    iwdg[0] = 0x00005555u;                          /* KR:  enable write access  */
    iwdg[1] = h[1];                                 /* PR  (+0x04) = Prescaler   */
    iwdg[2] = h[2];                                 /* RLR (+0x08) = Reload      */

    uint32_t start = tick_get();
    while (iwdg[3] != 0) {                           /* SR  (+0x0c): updates busy */
        if (tick_get() - start > 0x27u) {
            return 3;                                /* HAL_TIMEOUT */
        }
    }

    if (iwdg[4] == h[3]) {                           /* WINR (+0x10) already set  */
        iwdg[0] = 0x0000aaaau;                       /* KR: reload (refresh)      */
    } else {
        iwdg[4] = h[3];                              /* WINR = Window             */
    }
    return 0;                                        /* HAL_OK */
}

void iwdg_init(void)
{
    /* IWDG handle: Instance = IWDG (0x40003000), Prescaler = 4 (÷64),
     * Reload = 0x4e2, Window = 0xfff (disabled). */
    volatile uint32_t *h = (volatile uint32_t *)0x200006ac;
    h[0] = 0x40003000u;
    h[1] = 4;
    h[2] = 0x000004e2u;
    h[3] = 0x00000fffu;

    *(volatile uint32_t *)0x20002614 = 0;            /* reset ms tick */

    if (hal_iwdg_init((void *)h) != 0) { spi_error_reset(); }

    *(volatile uint32_t *)h[0] = 0x0000aaaau;        /* IWDG_KR: reload (refresh) key */
}
