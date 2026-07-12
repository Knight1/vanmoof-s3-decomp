#ifndef MAINWARE_SYSTEM_STM32F413_H
#define MAINWARE_SYSTEM_STM32F413_H

#include <stdint.h>

/* Low-level system init, called first by Reset_Handler before the C runtime.
 * This is the stock CubeF4 SystemInit (OEM 0x08043AA4): enable the FPU, put
 * the RCC back to its reset state, and set VTOR to the flash base. mainware's
 * own vector table at 0x08020200 is installed later, by main(). */
void SystemInit(void);

/* Core STOP-mode sleep primitive (OEM 0x08022DC4): program PWR_CR mode bits +
 * SCB->SCR.SLEEPDEEP, then WFI (use_wfi == 1) or double-WFE, clearing SLEEPDEEP on
 * wake. Called by enter_stop_mode. */
void enter_low_power_wait(uint32_t pwr_cr_mode, int use_wfi);

/* CubeF4 HAL_Init (OEM 0x080232AC): flash prefetch+caches, NVIC priority grouping,
 * SysTick, HAL_MspInit. Returns 0. First call of the boot sequence. */
int hal_mcu_init(void);

#endif
