#include "batteryware.h"

/* FLASH controller base */
static volatile uint32_t * const FLASH = (volatile uint32_t *)0x40022000;

/*
 * Enable flash prefetch and set 0-wait-state latency.
 * Writes PRFTEN (bit 1) and LATENCY=0 (bit 0) to FLASH_ACR.
 */
uint32_t flash_enable_prefetch(void)
{
    FLASH[0x04 / 4] |= 2;   /* FLASH_ACR |= PRFTEN */
    FLASH[0x04 / 4] |= 1;   /* FLASH_ACR |= LATENCY (0) */
    return 0;
}

/*
 * Unlock FLASH option bytes.
 * Writes OPTKEY1 + OPTKEY2 to FLASH_OPTKEYR, then sets OPTLOCK (bit 0x10) in FLASH_CR.
 */
uint32_t flash_unlock_opt(void)
{
    FLASH[0x08 / 4] = 0x04152637;   /* OPTKEY1 */
    FLASH[0x08 / 4] = 0xFAFBFCFD;   /* OPTKEY2 */
    FLASH[0x00 / 4] |= 0x10;        /* FLASH_CR |= OPTER */
    return 0;
}

/*
 * Lock FLASH option bytes.
 * Writes OPTKEY1 + OPTKEY2, then clears the option byte lock in FLASH_CR.
 */
uint32_t flash_lock_opt(void)
{
    FLASH[0x08 / 4] = 0x04152637;   /* OPTKEY1 */
    FLASH[0x08 / 4] = 0xFAFBFCFD;   /* OPTKEY2 */
    FLASH[0x00 / 4] &= ~0x10U;      /* FLASH_CR &= ~OPTER */
    return 0;
}

/* Systick timer base */
static volatile uint32_t * const SYSTICK = (volatile uint32_t *)0xE000E010;

/*
 * Start flash page erase via Systick timeout.
 *
 * Configures Systick with (timeout_ticks - 1) in LOAD, clears VAL,
 * sets CTRL to 7 (ENABLE | TICKINT | CLKSOURCE), and sets the
 * flash operation struct at 0xE000E010[4] = timeout, struct[0] = 7.
 * Returns false if timeout_ticks is out of range (< 0x1000000).
 */
bool flash_page_erase(uint32_t timeout_ticks)
{
    if (timeout_ticks - 1 >= 0x1000000) {
        return true;  /* out of range */
    }

    SYSTICK[1] = timeout_ticks - 1;   /* SYSTICK_LOAD */
    interrupt_set_priority(0xFF, 3);   /* set SysTick priority */
    SYSTICK[2] = 0;                    /* SYSTICK_VAL = 0 */
    SYSTICK[0] = 7;                    /* SYSTICK_CTRL: ENABLE | TICKINT | CLKSOURCE */

    return false;  /* success */
}

/*
 * NVIC / SCB interrupt priority setter.
 *
 * If irqn < 0x80: sets NVIC priority (IPR register at 0xE000E400).
 * If irqn >= 0x80: sets SCB system handler priority (SHPR register at 0xE000ED18).
 *
 * Only the upper 2 bits of priority are used (Cortex-M0+ implements 2-bit priority).
 */
void interrupt_set_priority(uint8_t irqn, uint32_t priority)
{
    volatile uint32_t *reg;

    if (irqn < 0x80) {
        /* NVIC IPR: base 0xE000E400, byte lanes [irqn/4] */
        reg = (volatile uint32_t *)(0xE000E400 + ((irqn >> 2) * 4));
    } else {
        /* SCB SHPR2/SHPR3: base 0xE000ED00, IRQ 8-15 */
        reg = (volatile uint32_t *)(0xE000ED00 + ((((irqn & 0xF) - 8) >> 2) + 6) * 4 + 4);
    }

    uint32_t shift = (irqn & 3) << 3;
    uint32_t mask  = 0xFFUL << shift;
    *reg = (*reg & ~mask) | (((priority & 3) << 6) << shift);
}

/*
 * Flash option byte operation wrapper.
 *
 * Delegates to interrupt_set_priority with signed-char conversion.
 * The OEM uses this as a unified "priority setter" for both NVIC
 * and SCB registers — the name "flash_opt_byte_op" is inherited
 * from the call site context (flash operation setup).
 */
void flash_opt_byte_op(uint8_t op, uint32_t val)
{
    interrupt_set_priority(op, val);
}

/*
 * Wait for flash operation to complete.
 *
 * Takes a flash context pointer. If the status register bit 2 (BUSY)
 * is not set, returns 0 (already done). Otherwise:
 *   - Sets bit 4 (error flag) if bit 1 (EOP) not set and bit 2 set
 *   - Polls with up to 0xB (11) ticks timeout via FUN_0800e304 (tick counter)
 *   - On timeout: sets bit 0x10 in field[0x15] and bit 1 in field[0x16]
 * Returns 0 on success, 1 on timeout.
 */
uint32_t flash_wait_ready(void *ctx)
{
    volatile uint32_t *c = (volatile uint32_t *)ctx;
    volatile uint32_t *reg = (volatile uint32_t *)*c;  /* base register pointer */

    if ((reg[8 / 4] & 4) == 0) {
        return 0;  /* not busy */
    }

    /* Error flag: if bit 1 clear and bit 2 set */
    if ((reg[8 / 4] & 4) == 4 && (reg[8 / 4] & 2) == 0) {
        reg[8 / 4] |= 0x10;
    }

    extern uint32_t tick_get(void);  /* FUN_0800e304 */
    uint32_t start = tick_get();

    do {
        if ((reg[8 / 4] & 4) == 0) {
            return 0;  /* done */
        }
    } while ((tick_get() - start) < 0xB);

    /* Timeout */
    c[0x15] |= 0x10;
    c[0x16] |= 1;
    return 1;
}

/*
 * Flash timeout check.
 *
 * Reads a counter and divider from SRAM, computes (counter / (1000 / divider)),
 * calls flash_page_erase with the result. If erase succeeds and param < 4,
 * configures the option byte. Returns 0 on success, 1 on failure.
 */
uint32_t flash_timeout_check(uint32_t param)
{
    volatile uint32_t * const s_counter  = (volatile uint32_t *)0x200000C8;
    volatile uint32_t * const s_divider  = (volatile uint32_t *)0x200000C4;
    volatile uint32_t * const s_output   = (volatile uint32_t *)0x200000C0;

    uint32_t num = *s_counter;
    uint32_t div = 1000 / *s_divider;
    uint32_t count = num / div;

    if (flash_page_erase(count) != 0) {
        return 1;
    }

    if (param < 4) {
        flash_opt_byte_op(0xFF, param);
        *s_output = param;
        return 0;
    }

    return 1;
}

/*
 * Peripheral reset.
 *
 * Sets bit 1 in FLASH_ACR, calls flash_timeout_check(3), and if
 * successful calls epilogue thunk. Returns true if reset succeeded.
 */
bool peripheral_reset(void)
{
    extern void epilogue(void);  /* FUN_0800e290 */
    FLASH[0x04 / 4] |= 2;       /* FLASH_ACR |= PRFTEN */

    if (flash_timeout_check(3) == 0) {
        epilogue();
    }

    return flash_timeout_check(3) != 0;
}

/*
 * Start flash program operation.
 *
 * If status indicates not-ready (bit 1+0 both set in control?), returns 0.
 * Otherwise sets bit 1 in the control register, delays 1µs, and polls
 * with 0xB tick timeout. Returns 0 on completion, 1 on timeout.
 */
uint32_t flash_program_start(void *ctx)
{
    volatile uint32_t *c = (volatile uint32_t *)ctx;
    volatile uint32_t *reg = (volatile uint32_t *)*c;
    extern uint32_t tick_get(void);  /* FUN_0800e304 */

    /* Check if ready: bit 1 set in control and bit 0 set in base? */
    if (((reg[8 / 4] & 3) == 1) && ((reg[0] & 1) == 1)) {
        return 0;  /* already done */
    }

    if ((reg[8 / 4] & 0xFFFFFFFF) == 0) {
        reg[8 / 4] |= 1;  /* start program */
        delay_us(1);

        uint32_t start = tick_get();
        do {
            if ((reg[0] & 1) == 1) {
                return 0;
            }
        } while ((tick_get() - start) < 0xB);

        /* Timeout */
        c[0x15] |= 0x10;
        c[0x16] |= 1;
        return 1;
    }

    /* Error state */
    c[0x15] |= 0x10;
    c[0x16] |= 1;
    return 1;
}

/*
 * Start flash erase operation.
 *
 * If not ready, sets bit 2 in control, writes CR=3, polls with
 * 0xB timeout. Additional error check: if bit 0+2 set but bit 0
 * of base is clear, still treats as error. Returns 0 on success, 1 on failure.
 */
uint32_t flash_erase_start(void *ctx)
{
    volatile uint32_t *c = (volatile uint32_t *)ctx;
    volatile uint32_t *reg = (volatile uint32_t *)*c;
    extern uint32_t tick_get(void);  /* FUN_0800e304 */

    if (((reg[8 / 4] & 3) == 1) && ((reg[0] & 1) == 1)) {
        /* Check additional error: bit 0+2 must both be set */
        if ((reg[8 / 4] & 5) == 1) {
            reg[8 / 4] |= 2;
            reg[0] = 3;  /* write CR */

            uint32_t start = tick_get();
            do {
                if ((reg[8 / 4] & 1) != 1) {
                    return 0;
                }
            } while ((tick_get() - start) < 0xB);

            /* Timeout */
            c[0x15] |= 0x10;
            c[0x16] |= 1;
            return 1;
        }

        /* Error: bits set wrong */
        c[0x15] |= 0x10;
        c[0x16] |= 1;
        return 1;
    }

    return 0;
}

/*
 * Write a single 32-bit word to flash memory.
 *
 * Mutex-guarded: if the flash mutex at 0x200047E0 is locked, returns 2.
 * Otherwise locks the mutex, checks the bus via dma_lock, writes the word,
 * unlocks the mutex, and returns the status.
 */
uint8_t flash_word_write(uint32_t type, volatile uint32_t *dst, uint32_t val)
{
    volatile uint8_t  * const s_flash_mutex  = (volatile uint8_t *)0x200047E0;
    volatile uint32_t * const s_flash_addr   = (volatile uint32_t *)0x200047E4;
    volatile uint32_t * const s_flash_busy   = (volatile uint32_t *)0x20002000;
    extern uint8_t dma_lock(void *ctx);  /* FUN_0800f3ac */

    if (s_flash_mutex[0x10] == 1) {
        return 2;  /* already locked */
    }

    s_flash_mutex[0x10] = 1;
    uint8_t ret = dma_lock((void *)s_flash_busy);

    if (ret == 0) {
        s_flash_addr[0x14 / 4] = 0;
        *dst = val;
        ret = dma_lock((void *)s_flash_busy);
    }

    s_flash_mutex[0x10] = 0;
    return ret;
}

/*
 * Unlock both flash program and option byte access.
 *
 * If FLASH_SR bit 0 is set: saves PRIMASK, disables IRQs, writes KEY1+KEY2
 * to FLASH_KEYR, restores PRIMASK, checks success.
 * If FLASH_SR bit 1 is set: same for OPTKEYR.
 * Returns 1 if unlock fails, 0 on success.
 */
uint32_t flash_unlock_both(void)
{
    /* Check if already unlocked */
    if ((FLASH[4 / 4] & 1) == 1) {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();

        FLASH[0x0C / 4] = 0x89ABCDEF;  /* KEY1 */
        FLASH[0x0C / 4] = 0x02030405;  /* KEY2 */

        if (__get_PRIMASK() == 0) {
            __set_PRIMASK(primask);
        }

        if ((FLASH[4 / 4] & 1) == 1) {
            return 1;  /* unlock failed */
        }
    }

    if ((FLASH[4 / 4] & 2) == 2) {
        uint32_t primask = __get_PRIMASK();
        __disable_irq();

        FLASH[0x10 / 4] = 0x89ABCDEF;  /* OPTKEY1 */
        FLASH[0x10 / 4] = 0x02030405;  /* OPTKEY2 */

        if (__get_PRIMASK() == 0) {
            __set_PRIMASK(primask);
        }

        if ((FLASH[4 / 4] & 2) == 2) {
            return 1;
        }
    }

    return 0;
}
