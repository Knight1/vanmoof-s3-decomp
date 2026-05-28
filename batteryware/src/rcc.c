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

/* Additional RCC register indices (relative to RCC_BASE). */
#define RCC_ICSCR           RCC_BASE[1]            /* +0x04 */
#define RCC_CRRCR           RCC_BASE[6]            /* +0x18 */
#define RCC_APB1ENR         RCC_BASE[14]           /* +0x38 */
#define RCC_CSR             RCC_BASE[20]           /* +0x50 */

/* PWR base — needed to unlock backup domain before LSE/RTC writes. */
#define PWR_CR              (*(volatile uint32_t *)0x40007000U)
#define PWR_CR_DBP          (1u << 8)

/* CR bits */
#define RCC_CR_HSI16ON      (1u << 0)
#define RCC_CR_HSI16RDY     (1u << 2)
#define RCC_CR_HSI16DIVEN   (1u << 4)
#define RCC_CR_MSION        (1u << 8)
#define RCC_CR_MSIRDY       (1u << 9)
#define RCC_CR_HSEON        (1u << 16)
#define RCC_CR_HSERDY_BIT   (1u << 17)
#define RCC_CR_HSEBYP       (1u << 18)
#define RCC_CR_PLLON        (1u << 24)
#define RCC_CR_PLLRDY_BIT   (1u << 25)

/* CSR bits (LSE/LSI live in RCC->CSR on STM32L0) */
#define RCC_CSR_LSION       (1u << 0)
#define RCC_CSR_LSIRDY      (1u << 1)
#define RCC_CSR_LSEON       (1u << 8)
#define RCC_CSR_LSERDY      (1u << 9)
#define RCC_CSR_LSEBYP      (1u << 10)

/* CRRCR.HSI48ON / RDY */
#define RCC_CRRCR_HSI48ON   (1u << 0)
#define RCC_CRRCR_HSI48RDY  (1u << 1)

/* OscillatorType bitmask */
#define RCC_OSC_HSE         0x01u
#define RCC_OSC_HSI         0x02u
#define RCC_OSC_LSE         0x04u
#define RCC_OSC_LSI         0x08u
#define RCC_OSC_MSI         0x10u
#define RCC_OSC_HSI48       0x20u

/* State encodings — these are bit patterns the HAL ORs into the
 * relevant register rather than enum codes. */
#define RCC_HSE_ON          0x00010000u
#define RCC_HSE_BYPASS      0x00050000u
#define RCC_LSE_ON          0x00000100u
#define RCC_LSE_BYPASS      0x00000500u

/* PLL state */
#define RCC_PLL_NONE        0u
#define RCC_PLL_OFF         1u
#define RCC_PLL_ON          2u

/* Timeout values (ms) — match standard STM32L0 HAL. */
#define RCC_HSE_TIMEOUT     100u
#define RCC_LSE_TIMEOUT     5000u
#define RCC_DBP_TIMEOUT     2u
#define RCC_GENERIC_TIMEOUT 2u

typedef struct {
    uint32_t OscillatorType;     /* +0  */
    uint32_t HSEState;           /* +4  */
    uint32_t LSEState;           /* +8  */
    uint32_t HSIState;           /* +12 */
    uint32_t HSICalibrationValue;/* +16 */
    uint32_t LSIState;           /* +20 */
    uint32_t HSI48State;         /* +24 */
    uint32_t MSIState;           /* +28 */
    uint32_t MSICalibrationValue;/* +32 */
    uint32_t MSIClockRange;      /* +36 */
    uint32_t PLLState;           /* +40 */
    uint32_t PLLSource;          /* +44 */
    uint32_t PLLMUL;             /* +48 */
    uint32_t PLLDIV;             /* +52 */
} rcc_osc_init_t;

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

/*
 * Oscillator configuration — HAL_RCC_OscConfig equivalent
 * (FUN_0800FDAC, 1864 B).
 *
 * Previously stubbed as `bus_fault_reset` returning 0. The OEM body is
 * the textbook HAL multi-phase oscillator bring-up: HSE, HSI, MSI, LSI,
 * LSE, HSI48, then PLL. Each phase follows the same shape:
 *
 *   1. Skip if `cfg->OscillatorType` doesn't include this oscillator.
 *   2. Validate: if the requested change would disable a clock that is
 *      currently driving SYSCLK (or the PLL that drives SYSCLK), refuse
 *      with HAL_ERROR (1).
 *   3. Apply the new ON/OFF/BYPASS state by writing the appropriate
 *      bits in RCC->CR / RCC->CSR / RCC->CRRCR.
 *   4. Poll the matching RDY bit with the phase-specific timeout
 *      (100 ms for HSE, 5000 ms for LSE, 2 ms for everything else).
 *      A timeout returns HAL_TIMEOUT (3).
 *
 * The MSI phase additionally recomputes SystemCoreClock and reinstalls
 * SysTick (since changing MSI range may move HCLK). The PLL phase
 * disables PLL → reprograms PLLSRC/PLLMUL/PLLDIV via CFGR → enables PLL,
 * each with a 2 ms RDY poll.
 *
 * LSE programming requires the backup domain to be unlocked
 * (PWR->CR.DBP), so the wrapper does that as needed and re-locks on exit.
 *
 * Returns: 0 = HAL_OK, 1 = HAL_ERROR (null arg or oscillator-in-use),
 *          3 = HAL_TIMEOUT (RDY bit never asserted within the window).
 */
uint32_t rcc_osc_config(void *cfg_v)
{
    rcc_osc_init_t *cfg = (rcc_osc_init_t *)cfg_v;
    uint32_t start;

    if (cfg == NULL) {
        return 1;
    }

    /* Snapshot current SYSCLK source (SWS) and PLLSRC for in-use
     * checks across phases. */
    const uint32_t sws_save    = RCC_CFGR & 0xCu;
    const uint32_t pllsrc_save = RCC_CFGR & 0x10000u;

    /* ---------- HSE phase --------------------------------------------- */
    if ((cfg->OscillatorType & RCC_OSC_HSE) != 0) {
        /* In-use check: HSE drives SYSCLK directly, or PLL+HSE source. */
        if (sws_save == 0x8u ||
            (sws_save == 0xCu && pllsrc_save == 0x10000u)) {
            if ((RCC_CR & RCC_CR_HSERDY_BIT) != 0 && cfg->HSEState == 0) {
                return 1;
            }
        } else {
            if (cfg->HSEState == RCC_HSE_ON) {
                RCC_CR |= RCC_CR_HSEON;
            } else if (cfg->HSEState == RCC_HSE_BYPASS) {
                RCC_CR |= RCC_CR_HSEBYP;
                RCC_CR |= RCC_CR_HSEON;
            } else {
                RCC_CR &= ~RCC_CR_HSEON;
                start = tick_get();
                while ((RCC_CR & RCC_CR_HSERDY_BIT) != 0) {
                    if ((tick_get() - start) > RCC_HSE_TIMEOUT) {
                        return 3;
                    }
                }
                RCC_CR &= ~RCC_CR_HSEBYP;
            }
            if (cfg->HSEState != 0) {
                start = tick_get();
                while ((RCC_CR & RCC_CR_HSERDY_BIT) == 0) {
                    if ((tick_get() - start) > RCC_HSE_TIMEOUT) {
                        return 3;
                    }
                }
            }
        }
    }

    /* ---------- HSI phase --------------------------------------------- */
    if ((cfg->OscillatorType & RCC_OSC_HSI) != 0) {
        /* In-use check: HSI drives SYSCLK or PLL+HSI. */
        if (sws_save == 0x4u ||
            (sws_save == 0xCu && pllsrc_save == 0)) {
            if ((RCC_CR & RCC_CR_HSI16RDY) != 0 && cfg->HSIState == 0) {
                return 1;
            }
            /* Allow only the trim update — write HSITRIM bits in CR. */
            RCC_CR = (RCC_CR & ~(0x1Fu << 8)) |
                     ((cfg->HSICalibrationValue & 0x1Fu) << 8);
        } else {
            if (cfg->HSIState != 0) {
                RCC_CR |= RCC_CR_HSI16ON;
                start = tick_get();
                while ((RCC_CR & RCC_CR_HSI16RDY) == 0) {
                    if ((tick_get() - start) > RCC_GENERIC_TIMEOUT) {
                        return 3;
                    }
                }
                RCC_CR = (RCC_CR & ~(0x1Fu << 8)) |
                         ((cfg->HSICalibrationValue & 0x1Fu) << 8);
            } else {
                RCC_CR &= ~RCC_CR_HSI16ON;
                start = tick_get();
                while ((RCC_CR & RCC_CR_HSI16RDY) != 0) {
                    if ((tick_get() - start) > RCC_GENERIC_TIMEOUT) {
                        return 3;
                    }
                }
            }
        }
    }

    /* ---------- MSI phase --------------------------------------------- */
    if ((cfg->OscillatorType & RCC_OSC_MSI) != 0) {
        /* In-use check: MSI drives SYSCLK directly. */
        if (sws_save == 0) {
            if ((RCC_CR & RCC_CR_MSIRDY) != 0 && cfg->MSIState == 0) {
                return 1;
            }
            /* Same-source: only update range / calibration. */
            RCC_ICSCR = (RCC_ICSCR & 0xFFFF1FFFu) | cfg->MSIClockRange;
            RCC_ICSCR = (RCC_ICSCR & 0x00FFFFFFu) |
                        (cfg->MSICalibrationValue << 24);
            /* Recompute SystemCoreClock from the new MSI range, since
             * SYSCLK moved. */
            uint32_t msi_range = cfg->MSIClockRange >> 13;
            uint32_t msi_hz    = 32768u << (msi_range + 1);
            uint32_t hpre      = (RCC_CFGR >> 4) & 0xFu;
            G_SYSTEM_CORE_CLOCK = msi_hz >> AHBPrescTable[hpre];
            uint8_t  st = hal_init_tick(G_TICK_PRIORITY);
            if (st != 0) return st;
        } else if (cfg->MSIState != 0) {
            RCC_CR |= RCC_CR_MSION;
            start = tick_get();
            while ((RCC_CR & RCC_CR_MSIRDY) == 0) {
                if ((tick_get() - start) > RCC_GENERIC_TIMEOUT) {
                    return 3;
                }
            }
            RCC_ICSCR = (RCC_ICSCR & 0xFFFF1FFFu) | cfg->MSIClockRange;
            RCC_ICSCR = (RCC_ICSCR & 0x00FFFFFFu) |
                        (cfg->MSICalibrationValue << 24);
        } else {
            RCC_CR &= ~RCC_CR_MSION;
            start = tick_get();
            while ((RCC_CR & RCC_CR_MSIRDY) != 0) {
                if ((tick_get() - start) > RCC_GENERIC_TIMEOUT) {
                    return 3;
                }
            }
        }
    }

    /* ---------- LSI phase --------------------------------------------- */
    if ((cfg->OscillatorType & RCC_OSC_LSI) != 0) {
        if (cfg->LSIState != 0) {
            RCC_CSR |= RCC_CSR_LSION;
            start = tick_get();
            while ((RCC_CSR & RCC_CSR_LSIRDY) == 0) {
                if ((tick_get() - start) > RCC_GENERIC_TIMEOUT) {
                    return 3;
                }
            }
        } else {
            RCC_CSR &= ~RCC_CSR_LSION;
            start = tick_get();
            while ((RCC_CSR & RCC_CSR_LSIRDY) != 0) {
                if ((tick_get() - start) > RCC_GENERIC_TIMEOUT) {
                    return 3;
                }
            }
        }
    }

    /* ---------- LSE phase --------------------------------------------- */
    if ((cfg->OscillatorType & RCC_OSC_LSE) != 0) {
        /* Unlock backup domain (DBP). */
        bool dbp_locked = false;
        if ((RCC_APB1ENR & (1u << 28)) == 0) {
            RCC_APB1ENR |= (1u << 28);
            dbp_locked = true;
        }
        if ((PWR_CR & PWR_CR_DBP) == 0) {
            PWR_CR |= PWR_CR_DBP;
            start = tick_get();
            while ((PWR_CR & PWR_CR_DBP) == 0) {
                if ((tick_get() - start) > RCC_DBP_TIMEOUT * 50) {
                    return 3;
                }
            }
        }

        if (cfg->LSEState == RCC_LSE_ON) {
            RCC_CSR |= RCC_CSR_LSEON;
        } else if (cfg->LSEState == RCC_LSE_BYPASS) {
            RCC_CSR |= RCC_CSR_LSEBYP;
            RCC_CSR |= RCC_CSR_LSEON;
        } else {
            RCC_CSR &= ~RCC_CSR_LSEON;
            RCC_CSR &= ~RCC_CSR_LSEBYP;
        }

        if (cfg->LSEState != 0) {
            start = tick_get();
            while ((RCC_CSR & RCC_CSR_LSERDY) == 0) {
                if ((tick_get() - start) > RCC_LSE_TIMEOUT) {
                    return 3;
                }
            }
        } else {
            start = tick_get();
            while ((RCC_CSR & RCC_CSR_LSERDY) != 0) {
                if ((tick_get() - start) > RCC_LSE_TIMEOUT) {
                    return 3;
                }
            }
        }

        if (dbp_locked) {
            RCC_APB1ENR &= ~(1u << 28);
        }
    }

    /* ---------- HSI48 phase ------------------------------------------- */
    if ((cfg->OscillatorType & RCC_OSC_HSI48) != 0) {
        if (cfg->HSI48State != 0) {
            RCC_CRRCR |= RCC_CRRCR_HSI48ON;
            start = tick_get();
            while ((RCC_CRRCR & RCC_CRRCR_HSI48RDY) == 0) {
                if ((tick_get() - start) > RCC_GENERIC_TIMEOUT) {
                    return 3;
                }
            }
        } else {
            RCC_CRRCR &= ~RCC_CRRCR_HSI48ON;
            start = tick_get();
            while ((RCC_CRRCR & RCC_CRRCR_HSI48RDY) != 0) {
                if ((tick_get() - start) > RCC_GENERIC_TIMEOUT) {
                    return 3;
                }
            }
        }
    }

    /* ---------- PLL phase --------------------------------------------- */
    if (cfg->PLLState != RCC_PLL_NONE) {
        /* Cannot reconfigure PLL while it drives SYSCLK. */
        if (sws_save == 0xCu) {
            return 1;
        }
        if (cfg->PLLState == RCC_PLL_ON) {
            /* Disable, wait for not-ready, reprogram CFGR, re-enable. */
            RCC_CR &= ~RCC_CR_PLLON;
            start = tick_get();
            while ((RCC_CR & RCC_CR_PLLRDY_BIT) != 0) {
                if ((tick_get() - start) > RCC_GENERIC_TIMEOUT) {
                    return 3;
                }
            }
            RCC_CFGR =
                (RCC_CFGR & ~(0x10000u | 0x3C0000u | 0xC00000u)) |
                cfg->PLLSource | cfg->PLLMUL | cfg->PLLDIV;
            RCC_CR |= RCC_CR_PLLON;
            start = tick_get();
            while ((RCC_CR & RCC_CR_PLLRDY_BIT) == 0) {
                if ((tick_get() - start) > RCC_GENERIC_TIMEOUT) {
                    return 3;
                }
            }
        } else {
            RCC_CR &= ~RCC_CR_PLLON;
            start = tick_get();
            while ((RCC_CR & RCC_CR_PLLRDY_BIT) != 0) {
                if ((tick_get() - start) > RCC_GENERIC_TIMEOUT) {
                    return 3;
                }
            }
        }
    }

    return 0;
}
