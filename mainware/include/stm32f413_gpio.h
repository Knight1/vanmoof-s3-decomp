#ifndef MAINWARE_STM32F413_GPIO_H
#define MAINWARE_STM32F413_GPIO_H

/* Just the GPIO bank base addresses we actually reference. Per
 * STM32F413 reference manual (RM0430 Rev 8 §2.3 "Memory map"). */
#define GPIOA_BASE 0x40020000u
#define GPIOB_BASE 0x40020400u
#define GPIOC_BASE 0x40020800u
#define GPIOD_BASE 0x40020C00u
#define GPIOE_BASE 0x40021000u

#endif
