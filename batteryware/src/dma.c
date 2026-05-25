#include "batteryware.h"

/* DMA enable flag in SRAM */
static volatile uint8_t * const s_dma_enabled  = (volatile uint8_t *)0x20002554;
/* DMA transfer counter */
static volatile uint8_t * const s_dma_counter  = (volatile uint8_t *)0x20002582;
/* DMA context struct */
static volatile void * const s_dma_ctx         = (volatile void *)0x200024F4;

/*
 * Stop DMA operation.
 *
 * Clears the DMA enable flag (bit 0), resets the transfer counter,
 * and calls the DMA de-initialization routine.
 */
void dma_stop(void)
{
    *s_dma_enabled &= ~1U;
    *s_dma_counter = 0;
    extern void dma_deinit(void *ctx);  /* FUN_0800e794 */
    dma_deinit((void *)s_dma_ctx);
}
