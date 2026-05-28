#include "batteryware.h"

/*
 * Clock-tree configuration — HAL_RCC_ClockConfig equivalent.
 *
 * Previously decompiled as `usart_bus_config` (FUN_08010554) and
 * stubbed to `return 0`. The OEM body is 640 B of HAL_RCC_ClockConfig
 * + FLASH latency staging — nothing to do with USART. The wrapper in
 * main.c::peripheral_init zeroes a 20-byte cfg buffer, sets
 *   cfg[0] = 0x0F   (ClockType = SYSCLK|HCLK|PCLK1|PCLK2)
 *   cfg[4] = 1      (SYSCLKSource = HSI16)
 *   cfg[8] = 0x80   (AHBCLKDivider = SYSCLK/2 via HPRE bit 7)
 * and calls this with FLatency = 0 (zero wait states).
 *
 * Register addresses match STM32L072 (single-bit LATENCY field in
 * FLASH->ACR — see RM0376 §3.3.3).
 */

/* FLASH interface: 0x40022000, FLASH->ACR @ +0 */
#define FLASH_ACR_REG       (*(volatile uint32_t *)0x40022000U)
#define FLASH_ACR_LATENCY   1u

/* RCC base: 0x40021000 */
#define RCC_BASE            (*(volatile uint32_t (*)[16])0x40021000U)
#define RCC_CR              RCC_BASE[0]            /* +0x00 */
#define RCC_CFGR            RCC_BASE[3]            /* +0x0C */

#define RCC_CR_HSI16RDY     (1u << 2)              /* bit 2  */
#define RCC_CR_MSIRDY       (1u << 9)              /* bit 9  */
#define RCC_CR_HSERDY       (1u << 17)             /* bit 17 */
#define RCC_CR_PLLRDY       (1u << 25)             /* bit 25 */

#define RCC_CFGR_SW_Msk     0x3u                   /* [1:0]  */
#define RCC_CFGR_SWS_Msk    0xCu                   /* [3:2]  */
#define RCC_CFGR_HPRE_Msk   0xF0u                  /* [7:4]  */
#define RCC_CFGR_PPRE1_Msk  0x700u                 /* [10:8] */
#define RCC_CFGR_PPRE2_Msk  0x3800u                /* [13:11] */

#define RCC_CLOCKTYPE_SYSCLK   0x1u
#define RCC_CLOCKTYPE_HCLK     0x2u
#define RCC_CLOCKTYPE_PCLK1    0x4u
#define RCC_CLOCKTYPE_PCLK2    0x8u

/* SYSCLKSource encodings — match the CFGR.SW field directly. */
#define RCC_SYSCLKSOURCE_MSI   0u
#define RCC_SYSCLKSOURCE_HSI   1u
#define RCC_SYSCLKSOURCE_HSE   2u
#define RCC_SYSCLKSOURCE_PLL   3u

#define CLOCKSWITCH_TIMEOUT_TICKS  5000u

typedef struct {
    uint32_t ClockType;          /* +0  */
    uint32_t SYSCLKSource;       /* +4  */
    uint32_t AHBCLKDivider;      /* +8  */
    uint32_t APB1CLKDivider;     /* +12 */
    uint32_t APB2CLKDivider;     /* +16 */
} rcc_clk_init_t;

/* SRAM globals — used throughout the firmware as raw addresses. */
#define G_SYSTEM_CORE_CLOCK  (*(volatile uint32_t *)0x200000C8U)
#define G_TICK_PRIORITY      (*(volatile uint32_t *)0x200000C0U)

/*
 * AHB prescaler shift table (HPRE encoding → bit-shift count).
 * Lives in flash at the OEM address 0x080181E8 in the original image
 * (past the .bin we have, but the values are the canonical STM32 HAL
 * table). Kept here so the C source resolves it locally.
 */
static const uint8_t AHBPrescTable[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6, 7, 8, 9
};

/* uwTickFreq lives at SRAM 0x200000C4. Encodes the HAL tick frequency
 * as period-in-ms: 1 = 1 kHz, 10 = 100 Hz, 100 = 10 Hz. */
#define G_TICK_FREQ          (*(volatile uint8_t  *)0x200000C4U)

/* PLL multiplier table — RCC_CFGR.PLLMUL (bits [21:18]) is an index.
 * The OEM keeps this in flash at 0x08010820 (past the .bin we have);
 * values are the canonical STM32L0 PLLMulTable. */
static const uint8_t PLLMulTable[9] = { 3, 4, 6, 8, 12, 16, 24, 32, 48 };

/*
 * HAL_RCC_GetSysClockFreq — FUN_080107e4 (216 B).
 *
 * Inspects RCC->CFGR.SWS (bits [3:2]) to pick the active SYSCLK
 * source, then returns its frequency in Hz:
 *
 *   SWS=0  (MSI)   →  32768 << (MSIRANGE + 1)   from RCC->ICSCR[15:13]
 *   SWS=4  (HSI16) →  16 MHz, or 4 MHz if RCC->CR.HSI16DIVEN (bit 4) is set
 *   SWS=8  (HSE)   →  fixed 8 MHz (HSE_VALUE)
 *   SWS=12 (PLL)   →  pll_input * PLLMulTable[PLLMUL] / (PLLDIV + 1)
 *                     where pll_input = HSE_VALUE if PLLSRC, else HSI16-or-/4
 *
 * The PLL math is done in 64-bit (the OEM calls __aeabi_lmul +
 * __aeabi_ldivmod) — the multiplier × clock product can exceed 32 bits.
 */
uint32_t rcc_get_sysclock_freq(void)
{
    uint32_t cfgr = RCC_CFGR;
    uint32_t sws  = cfgr & 0xCu;
    uint32_t freq;

    if (sws == 12) {
        /* PLL */
        uint32_t pll_mul = PLLMulTable[(cfgr >> 18) & 0xFu];
        uint32_t pll_div = ((cfgr >> 22) & 0x3u) + 1;
        uint32_t pll_input;

        if ((cfgr & 0x10000u) != 0) {
            /* PLLSRC = 1: HSE */
            pll_input = 8000000u;
        } else if ((RCC_CR & (1u << 4)) != 0) {
            /* PLLSRC = 0, HSI16 with /4 divider */
            pll_input = 4000000u;
        } else {
            /* PLLSRC = 0, raw HSI16 */
            pll_input = 16000000u;
        }
        freq = (uint32_t)(((uint64_t)pll_input * pll_mul) / pll_div);
    } else if (sws == 4) {
        /* HSI16 — half on if HSI16DIVEN */
        freq = ((RCC_CR & (1u << 4)) != 0) ? 4000000u : 16000000u;
    } else if (sws == 8) {
        /* HSE */
        freq = 8000000u;
    } else {
        /* MSI */
        uint32_t msi_range = (RCC_BASE[1] >> 13) & 0x7u;  /* RCC->ICSCR */
        freq = 32768u << (msi_range + 1);
    }
    return freq;
}

/*
 * HAL_InitTick — FUN_0800e29c (90 B).
 *
 * Reconfigures SysTick to fire at uwTickFreq, then assigns the given
 * priority to the SysTick exception. Returns 0 on success, 1 if the
 * priority is out of range (>3, Cortex-M0+ has 2 priority bits) or if
 * SysTick_Config fails.
 *
 * SysTick reload = SystemCoreClock / (1000 / uwTickFreq).
 *
 * The 0xedd6 / 0xed6c helpers are HAL_SYSTICK_Config /
 * HAL_NVIC_SetPriority — currently named `flash_erase_page_wrapper`
 * and `flash_opt_byte_op` in this tree. Calls go through those
 * existing symbols to keep the byte sequence intact; renaming them
 * is a separate task.
 */
uint8_t hal_init_tick(uint32_t priority)
{
    extern bool flash_erase_page_wrapper(uint32_t ticks);  /* HAL_SYSTICK_Config */
    extern void flash_opt_byte_op(uint8_t irqn, uint32_t prio); /* HAL_NVIC_SetPriority */

    uint32_t reload = G_SYSTEM_CORE_CLOCK / (1000u / G_TICK_FREQ);
    if (flash_erase_page_wrapper(reload)) {
        return 1;
    }
    if (priority > 3) {
        return 1;
    }
    flash_opt_byte_op((uint8_t)-1, priority);   /* SysTick_IRQn = -1 → 0xFF */
    G_TICK_PRIORITY = priority;
    return 0;
}

/*
 * Configure the clock tree.
 *
 * Stage 1: if FLatency > current LATENCY, raise latency first.
 * Stage 2: if HCLK requested, program CFGR.HPRE.
 * Stage 3: if SYSCLK requested, verify source is ready in CR, write
 *          CFGR.SW, then poll CFGR.SWS until it matches.
 * Stage 4: if FLatency < current LATENCY (only possible after a
 *          SYSCLK drop), lower latency.
 * Stage 5: if PCLK1 requested, program CFGR.PPRE1.
 * Stage 6: if PCLK2 requested, program CFGR.PPRE2 (value is
 *          left-shifted by 3 before OR — the OEM expects a pre-
 *          normalised divider code here).
 * Stage 7: recompute SystemCoreClock and reinstall SysTick.
 *
 * Returns 0 on success, 1 on validation error (source not ready), or
 * 3 on a 5-second timeout polling LATENCY ack or SWS.
 */
uint32_t rcc_configure(void *cfg_v, uint32_t flatency)
{
    rcc_clk_init_t *cfg = (rcc_clk_init_t *)cfg_v;
    uint32_t start;

    if (cfg == NULL) {
        return 1;
    }

    /* Stage 1: raise FLASH latency if needed. */
    if (flatency > (FLASH_ACR_REG & FLASH_ACR_LATENCY)) {
        FLASH_ACR_REG = (FLASH_ACR_REG & ~FLASH_ACR_LATENCY) | flatency;
        start = tick_get();
        while ((FLASH_ACR_REG & FLASH_ACR_LATENCY) != flatency) {
            if ((tick_get() - start) > CLOCKSWITCH_TIMEOUT_TICKS) {
                return 3;
            }
        }
    }

    /* Stage 2: HCLK (AHB prescaler). */
    if ((cfg->ClockType & RCC_CLOCKTYPE_HCLK) != 0) {
        RCC_CFGR = (RCC_CFGR & ~RCC_CFGR_HPRE_Msk) | cfg->AHBCLKDivider;
    }

    /* Stage 3: SYSCLK switch with ready-check and SWS poll. */
    if ((cfg->ClockType & RCC_CLOCKTYPE_SYSCLK) != 0) {
        switch (cfg->SYSCLKSource) {
        case RCC_SYSCLKSOURCE_HSE:
            if ((RCC_CR & RCC_CR_HSERDY) == 0) return 1;
            break;
        case RCC_SYSCLKSOURCE_PLL:
            if ((RCC_CR & RCC_CR_PLLRDY) == 0) return 1;
            break;
        case RCC_SYSCLKSOURCE_HSI:
            if ((RCC_CR & RCC_CR_HSI16RDY) == 0) return 1;
            break;
        default:  /* MSI */
            if ((RCC_CR & RCC_CR_MSIRDY) == 0) return 1;
            break;
        }

        RCC_CFGR = (RCC_CFGR & ~RCC_CFGR_SW_Msk) | cfg->SYSCLKSource;

        start = tick_get();
        uint32_t expected_sws;
        switch (cfg->SYSCLKSource) {
        case RCC_SYSCLKSOURCE_HSE: expected_sws = 0x8u; break;
        case RCC_SYSCLKSOURCE_PLL: expected_sws = 0xCu; break;
        case RCC_SYSCLKSOURCE_HSI: expected_sws = 0x4u; break;
        default:                   expected_sws = 0x0u; break;
        }
        while ((RCC_CFGR & RCC_CFGR_SWS_Msk) != expected_sws) {
            if ((tick_get() - start) > CLOCKSWITCH_TIMEOUT_TICKS) {
                return 3;
            }
        }
    }

    /* Stage 4: lower FLASH latency if the new SYSCLK allows it. */
    if (flatency < (FLASH_ACR_REG & FLASH_ACR_LATENCY)) {
        FLASH_ACR_REG = (FLASH_ACR_REG & ~FLASH_ACR_LATENCY) | flatency;
        start = tick_get();
        while ((FLASH_ACR_REG & FLASH_ACR_LATENCY) != flatency) {
            if ((tick_get() - start) > CLOCKSWITCH_TIMEOUT_TICKS) {
                return 3;
            }
        }
    }

    /* Stage 5: PCLK1 (APB1 prescaler). */
    if ((cfg->ClockType & RCC_CLOCKTYPE_PCLK1) != 0) {
        RCC_CFGR = (RCC_CFGR & ~RCC_CFGR_PPRE1_Msk) | cfg->APB1CLKDivider;
    }

    /* Stage 6: PCLK2 (APB2 prescaler) — pre-shifted by 3 to land in
     * the PPRE2 field. */
    if ((cfg->ClockType & RCC_CLOCKTYPE_PCLK2) != 0) {
        RCC_CFGR = (RCC_CFGR & ~RCC_CFGR_PPRE2_Msk) | (cfg->APB2CLKDivider << 3);
    }

    /* Stage 7: recompute SystemCoreClock = SYSCLK >> AHB_shift. */
    uint32_t sysclk = rcc_get_sysclock_freq();
    uint32_t hpre = (RCC_CFGR >> 4) & 0xFu;
    G_SYSTEM_CORE_CLOCK = sysclk >> AHBPrescTable[hpre];

    return hal_init_tick(G_TICK_PRIORITY);
}
