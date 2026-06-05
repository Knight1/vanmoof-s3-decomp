#ifndef MAINWARE_GPIO_H
#define MAINWARE_GPIO_H

/* Board GPIO bring-up for the STM32F413 main controller (OEM gpio_init,
 * 0x080314E8): enables the GPIO port clocks, drives the initial output levels
 * (power/enable rails, status LEDs, amp lines), and configures every pin via
 * the CubeF4 HAL_GPIO_Init. This is the mainware pin map — see hardware.md. */
void gpio_init(void);

#endif
