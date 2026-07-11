#ifndef MAINWARE_DMA_H
#define MAINWARE_DMA_H

/* Enable the DMA1 + DMA2 controller clocks (RCC_AHB1ENR bits 21/22) and the two
 * DMA stream IRQs the firmware uses — DMA1_Stream1 (IRQ 12) and DMA2_Stream0
 * (IRQ 56). OEM dma_controller_init at 0x0803C218. */
void dma_controller_init(void);

#endif
