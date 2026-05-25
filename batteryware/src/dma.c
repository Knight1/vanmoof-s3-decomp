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

/*
 * DMA transfer dispatcher.
 *
 * Dispatches based on dma_ctx->mode (field[8]):
 *   mode 3: word copy — copies 'count' words from src to dst
 *   mode 1: byte copy — delegates to byte-copy (FUN_0800eff8)
 *   mode 2: halfword copy — delegates to memcpy_halfword
 * Sets the status flag (field[0x1D]) to 2 at start and 1 at end.
 */
uint32_t dma_transfer(volatile uint32_t *ctx, void *src, uint32_t count)
{
    volatile uint8_t *status = (volatile uint8_t *)((uintptr_t)ctx + 0x1D);
    *status = 2;

    uint32_t result = 0;
    uint32_t mode = ctx[8];

    if (mode == 3) {
        volatile uint32_t *dst = (volatile uint32_t *)*ctx;
        for (uint32_t i = 0; i < count; i++) {
            dst[i] = ((volatile uint32_t *)src)[i];
        }
        result = *dst;
    } else if (mode == 1) {
        extern uint32_t dma_byte_copy(void *ctx, void *src, uint32_t count);
        result = dma_byte_copy(ctx, src, count);
    } else if (mode == 2) {
        extern uint32_t memcpy_hw(void *ctx, void *src, uint32_t count);
        result = memcpy_hw(ctx, src, count);
    }

    *status = 1;
    return result;
}

/*
 * DMA transfer with interrupt enable.
 * Same as dma_transfer but also sets bit 1 in the base register.
 */
uint32_t dma_transfer_irq(volatile uint32_t *ctx, void *src, uint32_t count)
{
    volatile uint8_t *status = (volatile uint8_t *)((uintptr_t)ctx + 0x1D);
    *status = 2;

    volatile uint32_t *base = (volatile uint32_t *)*ctx;
    base[8 / 4] |= 1;  /* enable interrupt */

    uint32_t result = 0;
    uint32_t mode = ctx[8];

    if (mode == 3) {
        volatile uint32_t *dst = (volatile uint32_t *)*ctx;
        for (uint32_t i = 0; i < count; i++) {
            dst[i] = ((volatile uint32_t *)src)[i];
        }
        result = *dst;
    } else if (mode == 1) {
        extern uint32_t dma_byte_copy(void *ctx, void *src, uint32_t count);
        result = dma_byte_copy(ctx, src, count);
    } else if (mode == 2) {
        extern uint32_t memcpy_hw(void *ctx, void *src, uint32_t count);
        result = memcpy_hw(ctx, src, count);
    }

    *status = 1;
    return result;
}

/*
 * Atomic 64-byte (16 word) copy with interrupts disabled.
 * Copies from src to dst within a critical section.
 */
uint8_t atomic_copy_16words(volatile uint32_t *dst, volatile uint32_t *src)
{
    volatile uint32_t * const s_periph = (volatile uint32_t *)0x20002000;
    extern uint8_t dma_lock(void *ctx);  /* FUN_08015360 */

    uint8_t ret = dma_lock((void *)s_periph);
    if (ret != 0) {
        return ret;
    }

    s_periph[4 / 4] |= 0x400;
    s_periph[4 / 4] |= 8;

    __disable_irq();

    for (uint32_t i = 0; i < 16; i++) {
        dst[i] = src[i];
    }

    __enable_irq();

    ret = dma_lock((void *)s_periph);
    s_periph[4 / 4] &= ~8U;
    s_periph[4 / 4] &= 0xFFFFFFBF;

    return ret;
}
