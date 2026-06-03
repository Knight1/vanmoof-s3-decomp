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

/*
 * HardFault_Handler — OEM FUN_0800fe2c (14 bytes).
 *
 * The OEM HardFault vector does no fault-status capture: it is a bare tail
 * call into spi_error_reset() (FUN_0800fe3a), which requests a system reset
 * via SCB->AIRCR and spins. A fault in a BMS is unrecoverable, so the safe
 * action is an immediate reset rather than continuing on a corrupt context.
 * Names HardFault_Handler so it strong-overrides the weak Default_Handler
 * alias in startup_stm32f091.S.
 */
void HardFault_Handler(void)
{
    spi_error_reset();
}
