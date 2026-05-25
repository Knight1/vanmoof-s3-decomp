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
        }
    } while ((tick_get() - start) < 0x2A);

    return 3;  /* timeout */
}

/*
 * Timeout poll — wait for status condition with deadline.
 *
 * Spins checking (*ctx + 8) & mask == expected. If deadline is -1,
 * continues checking indefinitely (inner do-while). Otherwise
 * computes a deadline window: deadline + (max_time - tick_now).
 * Breaks when time exceeds the window or when the step counter
 * (derived from tick_counter[0x200000C8] bit-shifted) reaches zero.
 *
 * On exit (timeout):
 *   - Clears bits 0-4 in (*ctx + 4) (SR register cleanup)
 *   - If ctx[1] == 0x104 and ctx[2] in {0x8000, 0x400}: clears bit 6 in *ctx
 *   - If ctx[10] == 0x2000: sets *ctx = (*ctx & 0xFFFFDFFF) | 0x2000
 *   - Sets status byte to 1, clears tx_active flag
 *   - Returns 3
 *
 * Returns 0 when the expected status condition is met.
 */
uint32_t timeout_poll(int *ctx, uint32_t mask, uint8_t expected, int deadline, uint32_t max_time)
{
    volatile uint32_t *tick_ptr = (volatile uint32_t *)0x200000C8;
    uint32_t tick_now = tick_get();
    uint32_t window = (uint32_t)(deadline + ((int)max_time - (int)tick_now));
    tick_now = tick_get();
    int step = (int)(window * (((*tick_ptr & 0x7FFFFFF) >> 15)));

    while (1) {
        do {
            if ((mask == (*(volatile uint32_t *)(*ctx + 8) & mask)) == (expected != 0)) {
                return 0;
            }
        } while (deadline == -1);

        uint32_t now = tick_get();
        if ((window <= (now - tick_now)) || (window == 0)) break;
        if (step == 0) {
            window = 0;
        }
        step--;
    }

    /* timeout: cleanup */
    volatile uint32_t *reg = (volatile uint32_t *)*ctx;
    reg[1] &= 0xFFFFFF1F;

    if ((ctx[1] == 0x104) && ((ctx[2] == 0x8000) || (ctx[2] == 0x400))) {
        reg[0] &= ~0x40U;
    }
    if (ctx[10] == 0x2000) {
        reg[0] = (reg[0] & 0xFFFFDFFF) | 0x2000;
    }

    *(volatile uint8_t *)((uintptr_t)ctx + 0x51) = 1;
    *(volatile uint8_t *)(ctx + 0x14) = 0;
    return 3;
}

/*
 * DMA transfer done — post-transfer completion handler.
 *
 * Computes a timeout from tick_counter / 24000 * 100, clears bit 5 (0x20)
 * in the SR, then countdown-polls until bit 1 (0x02) is set in the status
 * register. If timeout expires, sets ctx[0x15] |= 0x20.
 * Then calls dma_timeout_copy. After that:
 *   - If SR bit 4 (error): status byte = 1, ctx[0x15] |= 2, system_reset_with_arg
 *   - If ctx[0x15] == 0:
 *       status 4: restart thunk
 *       otherwise: veneer_11ec8(ctx) (next-step callback)
 *   - Otherwise: system_reset_with_arg
 */
void dma_transfer_done(int *ctx)
{
    volatile uint32_t *tick_ptr = (volatile uint32_t *)0x200000C8;
    uint32_t divisor = 0x5DC0;  /* 24000 */
    uint32_t timeout = (*tick_ptr / divisor) * 100;

    uint32_t tick_now = tick_get();
    volatile uint32_t *reg = (volatile uint32_t *)*ctx;
    reg[1] &= 0xFFFFFFDF;

    do {
        if (timeout == 0) {
            ctx[0x15] |= 0x20;
            break;
        }
        timeout--;
    } while ((reg[2] & 2) == 0);

    extern uint32_t dma_timeout_copy(int *, uint32_t, uint32_t);
    if (dma_timeout_copy(ctx, 100, tick_now) != 0) {
        ctx[0x15] |= 0x20;
    }

    if ((reg[2] & 0x10) == 0x10) {
        *(volatile uint8_t *)((uintptr_t)ctx + 0x51) = 1;
        ctx[0x15] |= 2;
        reg[2] = 0xFFEF;
        system_reset_with_arg((uint32_t)ctx);
    } else if (ctx[0x15] == 0) {
        if (*(volatile uint8_t *)((uintptr_t)ctx + 0x51) == 4) {
            *(volatile uint8_t *)((uintptr_t)ctx + 0x51) = 1;
            extern void modem_restart_thunk(void);
            modem_restart_thunk();
        } else {
            *(volatile uint8_t *)((uintptr_t)ctx + 0x51) = 1;
            extern void veneer_11ec8(void *);
            /* FUN_08011ec8 — next-step transition callback */
            (void)ctx;
            veneer_11ec8(ctx);
        }
    } else {
        *(volatile uint8_t *)((uintptr_t)ctx + 0x51) = 1;
        system_reset_with_arg((uint32_t)ctx);
    }
}

/*
 * DMA channel configuration — apply configuration bits from ctx.
 *
 * Walks ctx[9] bitmask (bits 0-7) and applies each enabled configuration
 * field (ctx[10]-ctx[18]) to the peripheral registers at *ctx + 4 and
 * *ctx + 8, using the corresponding clear masks from the literal pool.
 * Special case: if bit 6 is set and ctx[10] == 0x100000, ORs ctx[11] too.
 */
void dma_channel_config(int *ctx)
{
    volatile uint32_t *reg = (volatile uint32_t *)*ctx;

    if ((ctx[9] & 1) != 0) {
        reg[1] = ctx[10] | (reg[1] & 0xFFFDFFFF);
    }
    if ((ctx[9] & 2) != 0) {
        reg[1] = ctx[11] | (reg[1] & 0xFFFEFFFF);
    }
    if ((ctx[9] & 4) != 0) {
        reg[1] = ctx[12] | (reg[1] & 0xFFFBFFFF);
    }
    if ((ctx[9] & 8) != 0) {
        reg[1] = ctx[13] | (reg[1] & 0xFFFF7FFF);
    }
    if ((ctx[9] & 0x10) != 0) {
        reg[2] = ctx[14] | (reg[2] & 0xFFFFEFFF);
    }
    if ((ctx[9] & 0x20) != 0) {
        reg[2] = ctx[15] | (reg[2] & 0xFFFFDFFF);
    }
    if ((ctx[9] & 0x40) != 0) {
        reg[1] = ctx[16] | (reg[1] & 0xFFEFFFFF);
        if (ctx[16] == 0x100000) {
            reg[1] = ctx[17] | (reg[1] & 0xFF9FFFFF);
        }
    }
    if ((ctx[9] & 0x80) != 0) {
        reg[1] = ctx[18] | (reg[1] & 0xFFF7FFFF);
    }
}

/*
 * DMA/USART peripheral initialization.
 *
 * NULL check → returns 1.
 * If ctx[9] == 0: clears ctx[7] (unless ctx[1]==0x104)
 * Otherwise: zeroes ctx[4] and ctx[5]
 * If status byte (ctx+0x51) is 0: clears tx_active, calls ISR ack thunk.
 * Sets status = 2, clears bit 6 in base, then merges all config fields
 * (ctx[1]-ctx[10]) into the base register and SR register.
 * If ctx[10]==0x2000: copies ctx[11] bits 0-15 to *ctx+0x10.
 * Masks *ctx+0x1C with 0xFFFFF7FF, zeroes ctx[0x15], sets status=1.
 * Returns 0 on success.
 */
uint32_t dma_usart_init(int *ctx)
{
    if (ctx == NULL) {
        return 1;
    }

    if (ctx[9] == 0) {
        if (ctx[1] != 0x104) {
            ctx[7] = 0;
        }
    } else {
        ctx[4] = 0;
        ctx[5] = 0;
    }

    if (*(volatile uint8_t *)((uintptr_t)ctx + 0x51) == 0) {
        *(volatile uint8_t *)(ctx + 0x14) = 0;
        extern void modem_isr_ack_dup(void);
        modem_isr_ack_dup();
    }

    *(volatile uint8_t *)((uintptr_t)ctx + 0x51) = 2;

    volatile uint32_t *reg = (volatile uint32_t *)*ctx;
    reg[0] &= ~0x40U;

    reg[0] = (ctx[10] & 0x2000) |
             (ctx[1]  & 0x104)  |
             (ctx[2]  & 0x8400) |
             (ctx[3]  & 0x800)  |
             (ctx[4]  & 0x2)    |
             (ctx[5]  & 0x1)    |
             (ctx[6]  & 0x200)  |
             (ctx[7]  & 0x38)   |
             (ctx[8]  & 0x80);

    reg[1] = (ctx[9] & 0x10) | ((uint32_t)(ctx[6] >> 16) & 4);

    if (ctx[10] == 0x2000) {
        reg[4] = ctx[11] & 0xFFFF;
    }

    reg[7] &= 0xFFFFF7FF;
    ctx[0x15] = 0;
    *(volatile uint8_t *)((uintptr_t)ctx + 0x51) = 1;

    return 0;
}

/*
 * DMA byte transfer handler — reads from peripheral to memory.
 *
 * Called during a DMA byte-by-byte transfer: reads one byte from
 * peripheral register (*ctx + 0x0C) into the destination buffer
 * at ctx[0x0E], increments destination, decrements remaining count
 * at ctx+0x3E. When the transfer completes:
 *   - If ctx[10] == 0x2000: sets the next-stage callback ptr
 *   - Otherwise: clears bit 6 in SR, and if secondary counter
 *     ctx[0x36] == 0, calls the transfer-done callback.
 */
void dma_byte_handler(int *ctx)
{
    volatile uint8_t *dst = (volatile uint8_t *)ctx[0x0E];
    *dst = *(volatile uint8_t *)(*ctx + 0x0C);
    ctx[0x0E] = (int)(dst + 1);
    *(volatile int16_t *)((uintptr_t)ctx + 0x3E) -= 1;

    if (*(volatile int16_t *)((uintptr_t)ctx + 0x3E) == 0) {
        if (ctx[10] == 0x2000) {
            ctx[0x10] = (int)0x0801601D;
        } else {
            volatile uint32_t *reg = (volatile uint32_t *)*ctx;
            reg[1] &= 0xFFFFFF9F;
            if (*(volatile int16_t *)((uintptr_t)ctx + 0x36) == 0) {
                dma_transfer_done(ctx);
            }
        }
    }
}

/*
 * DMA byte transfer handler v2 — writes from memory to peripheral.
 *
 * Like dma_byte_handler but reversed: reads from source buffer at
 * ctx[0x0C] into peripheral register (*ctx + 0x0C). Uses ctx+0x36
 * as the primary count. On completion:
 *   - If ctx[10] == 0x2000: sets bit 0x1000 in base, clears bit 7 in SR
 *   - Otherwise: clears bit 7, checks secondary counter ctx[0x3E]
 */
void dma_byte_handler_v2(int *ctx)
{
    volatile uint8_t *src = (volatile uint8_t *)ctx[0x0C];
    *(volatile uint8_t *)(*ctx + 0x0C) = *src;
    ctx[0x0C] = (int)(src + 1);
    *(volatile int16_t *)((uintptr_t)ctx + 0x36) -= 1;

    if (*(volatile int16_t *)((uintptr_t)ctx + 0x36) == 0) {
        if (ctx[10] == 0x2000) {
            volatile uint32_t *reg = (volatile uint32_t *)*ctx;
            reg[0] |= 0x1000;
            reg[1] &= 0xFFFFFF7F;
        } else {
            volatile uint32_t *reg = (volatile uint32_t *)*ctx;
            reg[1] &= 0xFFFFFF7F;
            if (*(volatile int16_t *)((uintptr_t)ctx + 0x3E) == 0) {
                dma_transfer_done(ctx);
            }
        }
    }
}

/*
 * DMA halfword transfer handler — writes from memory to peripheral.
 *
 * Same pattern as dma_byte_handler_v2 but operates on 16-bit
 * halfwords instead of bytes. Reads from ctx[0x0C], writes to
 * (*ctx + 0x0C). Increments source by 2. Uses ctx+0x36 as primary.
 */
void dma_halfword_handler(int *ctx)
{
    volatile uint16_t *src = (volatile uint16_t *)ctx[0x0C];
    *(volatile uint32_t *)(*ctx + 0x0C) = (uint32_t)*src;
    ctx[0x0C] = (int)(src + 1);
    *(volatile int16_t *)((uintptr_t)ctx + 0x36) -= 1;

    if (*(volatile int16_t *)((uintptr_t)ctx + 0x36) == 0) {
        if (ctx[10] == 0x2000) {
            volatile uint32_t *reg = (volatile uint32_t *)*ctx;
            reg[0] |= 0x1000;
            reg[1] &= 0xFFFFFF7F;
        } else {
            volatile uint32_t *reg = (volatile uint32_t *)*ctx;
            reg[1] &= 0xFFFFFF7F;
            if (*(volatile int16_t *)((uintptr_t)ctx + 0x3E) == 0) {
                dma_transfer_done(ctx);
            }
        }
    }
}

/*
 * DMA timeout copy — waits for DMA peripheral to be ready.
 *
 * If ctx[1] == 0x104 (special mode): calls the timeout poll helper
 * with mask 0x80. On failure sets bit 5 in ctx[0x15] and returns 3.
 * Otherwise: countdown loop using a calibrated timeout from
 * (tick_counter / 0x016E3600 * 1000). Returns 0 when bit 0x80
 * clears in (*ctx + 8) or when timeout expires.
 */
uint32_t dma_timeout_copy(int *ctx, uint32_t param2, uint32_t param3)
{
    volatile uint32_t *tick_ptr = (volatile uint32_t *)0x200000C8;
    uint32_t divisor = 0x016E3600;
    uint32_t timeout = (*tick_ptr / divisor) * 1000;

    if (ctx[1] == 0x104) {
        int result = timeout_poll(ctx, 0x80, 0, param2, param3);
        if (result != 0) {
            ctx[0x15] |= 0x20;
            return 3;
        }
    } else {
        do {
            if (timeout == 0) {
                return 0;
            }
            timeout--;
        } while (((*(volatile uint32_t *)(*ctx + 8)) & 0x80) == 0x80);
    }
    return 0;
}

/*
 * memcpy_halfword — copy halfword-aligned data into a destination pointer.
 *
 * Copies param_3 halfwords from param_2 to *param_1. Pairs two halfwords
 * into one 32-bit store. If an odd halfword remains, writes it as a 16-bit
 * store. Returns the last word written (the value at *param_1).
 */
uint32_t memcpy_halfword(volatile uint32_t *dst_ptr, uint32_t src_base, uint32_t count)
{
    uint32_t i;
    const uint16_t *src = (const uint16_t *)src_base;

    for (i = 0; i < (count >> 1); i++) {
        *dst_ptr = ((uint32_t)src[i * 2 + 1] << 16) | src[i * 2];
    }

    if ((count & 1) != 0) {
        *(volatile uint16_t *)dst_ptr = src[i * 2];
    }

    return *dst_ptr;
}

/*
 * DMA completion handler — finalize DMA transfer.
 *
 * Called after a DMA page program completes. Clears the completion
 * counter, gets the current tick, then checks two completion flags:
 *   - bit 3 (0x8): calls timeout poll with 0x200000 mask
 *   - bit 2 (0x4): calls timeout poll with 0x400000 mask
 * If either fails (returns non-zero), returns 3 (timeout error).
 * Otherwise sets page counters (0x1E/0x1F to 0x20, 0x18 to 0,
 * status byte to 0) and returns 0 (success).
 */
uint32_t dma_completion_handler(uint32_t *ctx)
{
    extern uint32_t tick_get(void);
    uint32_t tick = tick_get();
    int result;

    ctx[0x20] = 0;

    if (((*ctx & 8) == 8) &&
        ((result = timeout_poll(ctx, 0x200000, 0, tick, 0x01FFFFFF)) != 0)) {
        return 3;
    }
    if (((*ctx & 4) == 4) &&
        ((result = timeout_poll(ctx, 0x400000, 0, tick, 0x01FFFFFF)) != 0)) {
        return 3;
    }

    ctx[0x1E] = 0x20;
    ctx[0x1F] = 0x20;
    ctx[0x18] = 0;
    *(volatile uint8_t *)(ctx + 0x1D) = 0;
    return 0;
}

/*
 * DMA wait-done — poll DMA channel until transfer completes.
 *
 * Counts down from the given timeout value while bit 0 (TCIF)
 * remains set in the DMA status register at *ctx + 0x18.
 * If count reaches zero (timeout): returns 3.
 * Otherwise: clears any TCIF (bit 1), then checks for error flags
 * (bits 0x100-0x20000) and triggers fault handler on any error.
 * Returns 0 on clean completion.
 */
uint32_t dma_wait_done(int timeout)
{
    volatile uint32_t * const s_dma_ch_status = (volatile uint32_t *)0x40020020;
    int count = timeout;

    while (((s_dma_ch_status[6] & 1) == 1) && (count != 0)) {
        count--;
    }

    if (count == 0) {
        return 3;
    }

    if ((s_dma_ch_status[6] & 2) == 2) {
        s_dma_ch_status[6] = 2;  /* clear TCIF */
    }

    uint32_t status = s_dma_ch_status[6];
    if (((status & 0x100) == 0x100) || ((status & 0x200) == 0x200) ||
        ((status & 0x400) == 0x400) || ((status & 0x800) == 0x800) ||
        ((status & 0x2000) == 0x2000) || ((status & 0x20000) == 0x20000) ||
        ((status & 0x10000) == 0x10000)) {
        extern void dma_fault_handler(void);
        dma_fault_handler();
        return 1;
    }

    return 0;
}
