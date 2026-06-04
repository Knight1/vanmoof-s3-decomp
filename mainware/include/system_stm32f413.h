#ifndef MAINWARE_SYSTEM_STM32F413_H
#define MAINWARE_SYSTEM_STM32F413_H

/* Low-level system init, called first by Reset_Handler before the C runtime.
 * This is the stock CubeF4 SystemInit (OEM 0x08043AA4): enable the FPU, put
 * the RCC back to its reset state, and set VTOR to the flash base. mainware's
 * own vector table at 0x08020200 is installed later, by main(). */
void SystemInit(void);

#endif
