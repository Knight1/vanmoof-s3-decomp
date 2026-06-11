#ifndef MAINWARE_VECTORS_H
#define MAINWARE_VECTORS_H

/* NVIC peripheral-interrupt vector trampolines (vectors.c). Each occupies one
 * external-IRQ slot of the table at flash 0x08020200 (slot index == IRQ number)
 * and forwards to the real handler. Reconstructed but not yet wired into
 * startup.S — see vectors.c for the staging rationale. CMSIS vector names; the
 * OEM addresses run 0x0803CA20..0x0803CB64. */

void RTC_WKUP_IRQHandler(void);        /* IRQ3 */
void EXTI0_IRQHandler(void);           /* IRQ6 */
void EXTI1_IRQHandler(void);           /* IRQ7 */
void EXTI2_IRQHandler(void);           /* IRQ8 */
void EXTI3_IRQHandler(void);           /* IRQ9 */
void EXTI4_IRQHandler(void);           /* IRQ10 */
void DMA1_Stream1_IRQHandler(void);    /* IRQ12 */
void EXTI9_5_IRQHandler(void);         /* IRQ23 */
void TIM1_UP_TIM10_IRQHandler(void);   /* IRQ25 */
void I2C1_EV_IRQHandler(void);         /* IRQ31 */
void I2C1_ER_IRQHandler(void);         /* IRQ32 */
void USART1_IRQHandler(void);          /* IRQ37 */
void USART2_IRQHandler(void);          /* IRQ38 */
void USART3_IRQHandler(void);          /* IRQ39 */
void EXTI15_10_IRQHandler(void);       /* IRQ40 */
void UART4_IRQHandler(void);           /* IRQ52  BMS Modbus bus */
void UART5_IRQHandler(void);           /* IRQ53  BLE data link */
void TIM6_DAC_IRQHandler(void);        /* IRQ54 */
void TIM7_IRQHandler(void);            /* IRQ55 */
void DMA2_Stream0_IRQHandler(void);    /* IRQ56 */
void USART6_IRQHandler(void);          /* IRQ71 */
void I2C3_EV_IRQHandler(void);         /* IRQ72 */
void I2C3_ER_IRQHandler(void);         /* IRQ73 */
void UART7_IRQHandler(void);           /* IRQ82  ES3 console primary */
void UART8_IRQHandler(void);           /* IRQ83  BLE-debug link */

#endif
