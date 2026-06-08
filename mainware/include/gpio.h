#ifndef MAINWARE_GPIO_H
#define MAINWARE_GPIO_H

/* Board GPIO bring-up for the STM32F413 main controller (OEM gpio_init,
 * 0x080314E8): enables the GPIO port clocks, drives the initial output levels
 * (power/enable rails, status LEDs, amp lines), and configures every pin via
 * the CubeF4 HAL_GPIO_Init. This is the mainware pin map — see hardware.md. */
void gpio_init(void);

/* PC0 / PC1 sense lines: 1 if the pin reads LOW, else 0 (OEM 0x08040350 /
 * 0x08040368). The two button-state bytes of the BLE 0x5568 read. */
int gpio_pc0_is_low(void);
int gpio_pc1_is_low(void);

#endif
