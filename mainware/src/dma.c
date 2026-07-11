#include <stdint.h>

#include "dma.h"

extern void nvic_set_priority();   /* 0x08027078 */
extern void nvic_enable_irq();     /* 0x080270E0 */

/* dma_controller_init (OEM 0x0803C218) — the CubeF4 MX_DMA_Init equivalent: turn
 * on the DMA1 and DMA2 controller clocks (RCC_AHB1ENR bits 21/22) and enable the
 * two DMA stream interrupt lines the firmware services (DMA1_Stream1 = IRQ 12 for
 * the I2C/UART path, DMA2_Stream0 = IRQ 56). The stream-to-peripheral wiring is
 * done later by each peripheral's own MSP init. */
void dma_controller_init(void)
{
    volatile uint32_t *rcc_ahb1enr = (volatile uint32_t *)(0x40023800u + 0x30u);

    *rcc_ahb1enr |= 0x200000u;      /* RCC_AHB1ENR.DMA1EN (bit 21) */
    *rcc_ahb1enr |= 0x400000u;      /* RCC_AHB1ENR.DMA2EN (bit 22) */
    nvic_set_priority(12, 0, 0);    /* DMA1_Stream1_IRQn */
    nvic_enable_irq(12);
    nvic_set_priority(56, 0, 0);    /* DMA2_Stream0_IRQn */
    nvic_enable_irq(56);
}
