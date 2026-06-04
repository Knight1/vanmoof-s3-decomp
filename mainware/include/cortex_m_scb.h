#ifndef MAINWARE_CORTEX_M_SCB_H
#define MAINWARE_CORTEX_M_SCB_H

#include <stdint.h>

/* Cortex-M4 System Control Block fault-status / fault-address registers, read
 * by the HardFault frame dumper. Addresses per the ARMv7-M Architecture
 * Reference Manual (B3.2.2 "System control and ID registers"). These are core
 * registers, identical on every ARMv7-M part — not STM32F4-specific. */

#define SCB_CFSR   (*(volatile uint32_t *)0xE000ED28u)  /* Configurable Fault Status */
#define SCB_HFSR   (*(volatile uint32_t *)0xE000ED2Cu)  /* HardFault Status           */
#define SCB_DFSR   (*(volatile uint32_t *)0xE000ED30u)  /* Debug Fault Status         */
#define SCB_MMFAR  (*(volatile uint32_t *)0xE000ED34u)  /* MemManage Fault Address    */
#define SCB_BFAR   (*(volatile uint32_t *)0xE000ED38u)  /* BusFault Address           */
#define SCB_AFSR   (*(volatile uint32_t *)0xE000ED3Cu)  /* Auxiliary Fault Status     */

#endif
