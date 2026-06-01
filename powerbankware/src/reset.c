#include "powerbankware.h"

/*
 * system_reset_hang — OEM FUN_0800bab4.
 *
 * Requests a system reset via SCB->AIRCR (VECTKEY 0x05FA | SYSRESETREQ) and
 * spins until it takes effect. One of several compiler-duplicated copies of
 * this idiom in the image (cf. the static hal_error_reset in spi.c at
 * FUN_0800f50c); kept separate to preserve the OEM call structure.
 */
void system_reset_hang(void)
{
    __DSB();
    *(volatile uint32_t *)(0xE000ED00u + 0x0C) = 0x05FA0004u;   /* SCB_AIRCR */
    __DSB();
    for (;;) {
    }
}

/*
 * system_reset_request — OEM FUN_08010F30.
 *
 * The same SYSRESETREQ idiom, reached from a state-handler fault path; a third
 * compiler-duplicated copy in the image, kept separate to preserve the OEM
 * call structure.
 */
void system_reset_request(void)
{
    __DSB();
    *(volatile uint32_t *)(0xE000ED00u + 0x0C) = 0x05FA0004u;   /* SCB_AIRCR */
    __DSB();
    for (;;) {
    }
}
