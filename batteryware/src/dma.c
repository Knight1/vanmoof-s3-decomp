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

/* DMA init config struct */
static volatile uint32_t * const s_dma_init_cfg = (volatile uint32_t *)0x20002000;
static volatile uint32_t * const s_dma_timeout  = (volatile uint32_t *)0x20002000;

/*
 * DMA initialization.
 *
 * Configures the DMA struct: channel=4, address/control fields from flash,
 * timeout=0, then calls dma_flash_start to begin the operation.
 * Triggers system_reset on failure.
 */
void dma_init(void)
{
    s_dma_init_cfg[0] = 0x20002000;   /* base address */
    s_dma_init_cfg[1] = 4;            /* channel count */
    s_dma_init_cfg[2] = 0x20002000;   /* src pointer */
    s_dma_init_cfg[3] = 0x20002000;   /* dst/control */
    *s_dma_timeout = 0;

    if (dma_flash_start((void *)s_dma_init_cfg) != 0) {
        system_reset();
    }

    *(volatile uint32_t *)*s_dma_init_cfg = 0x20002000;
}

/* DMA peripheral base and status */
static volatile uint32_t * const s_dma_status   = (volatile uint32_t *)0x20002000;
static volatile uint32_t * const s_dma_periph  = (volatile uint32_t *)0x20002000;

/*
 * Reset a single DMA channel.
 *
 * Clears the status register, sets bits 0x200 (reset) and 0x8 (enable)
 * in the peripheral control, then zeroes the channel base address.
 */
void dma_channel_reset(uint32_t dma_base)
{
    s_dma_status[0x14 / 4] = 0;
    s_dma_periph[4 / 4] |= 0x200;
    s_dma_periph[4 / 4] |= 8;
    *(volatile uint32_t *)(dma_base & 0xFFFFFF80) = 0;
}

/*
 * Start a DMA-backed flash operation.
 *
 * If ctx is NULL, returns 1. Otherwise writes flash key sequence to
 * unlock, copies channel config fields, then polls with 0x2A (42) tick
 * timeout for the DMA to finish. Returns 0 on success, 3 on timeout.
 */
uint32_t dma_flash_start(void *ctx)
{
    volatile uint32_t *c = (volatile uint32_t *)ctx;
    extern uint32_t flash_key1, flash_key2;  /* flash key constants */
    extern uint32_t tick_get(void);  /* FUN_0800e304 */

    if (ctx == NULL) {
        return 1;
    }

    /* Write flash unlock keys */
    *(volatile uint32_t *)*c = 0x89ABCDEF;  /* KEY1 */
    *(volatile uint32_t *)*c = 0x02030405;  /* KEY2 */

    c[0x04 / 4] = c[1];   /* copy channel config */
    c[0x08 / 4] = c[2];

    uint32_t start = tick_get();
    do {
        if (c[0x0C / 4] == 0) {
            if (c[0x10 / 4] == c[3]) {
                *(volatile uint32_t *)*c = 0xFAFBFCFD;  /* lock key */
            } else {
                c[0x10 / 4] = c[3];
            }
            return 0;
        }
    } while ((tick_get() - start) < 0x2A);

    return 3;  /* timeout */
}
