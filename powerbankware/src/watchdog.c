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

extern int  FUN_0801b488(void *hiwdg);   /* HAL_IWDG_Init */
extern void FUN_0800fe3a(void);          /* OEM error handler */

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

    if (FUN_0801b488((void *)h) != 0) { FUN_0800fe3a(); }

    *(volatile uint32_t *)h[0] = 0x0000aaaau;        /* IWDG_KR: reload (refresh) key */
}
