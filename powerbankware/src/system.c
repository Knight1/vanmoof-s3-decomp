#include "powerbankware.h"

/*
 * System clock tree, RCC HAL configurators and SystemInit.
 *
 *   pwr_enable_bkup_access      = FUN_0801b51c  (PWR_CR DBP)
 *   hal_rcc_osc_config          = FUN_0801b538  (HAL_RCC_OscConfig)
 *   hal_rcc_clock_config        = FUN_0801bbf8  (HAL_RCC_ClockConfig)
 *   hal_rccex_periph_clk_config = FUN_0801bf4c  (HAL_RCCEx_PeriphCLKConfig)
 *   system_core_clock_update    = FUN_0801d9c0  (SystemCoreClockUpdate)
 *   clock_rtc_init              = FUN_080136c0  (board clock + RTC bring-up)
 *   SystemInit                  = FUN_0801d938  (CMSIS pre-main RCC reset)
 *
 * Register offsets, bit masks, struct-field indices and timeout values are
 * disasm-confirmed against the OEM image (RCC 0x40021000, FLASH 0x40022000,
 * PWR 0x40007000). HSE_VALUE 16 MHz, HSI 8 MHz, HSI48 48 MHz.
 */

#define RCC_CR      (*(volatile uint32_t *)(0x40021000u + 0x00))
#define RCC_CFGR    (*(volatile uint32_t *)(0x40021000u + 0x04))
#define RCC_CIR     (*(volatile uint32_t *)(0x40021000u + 0x08))
#define RCC_APB1ENR (*(volatile uint32_t *)(0x40021000u + 0x1c))
#define RCC_BDCR    (*(volatile uint32_t *)(0x40021000u + 0x20))
#define RCC_CSR     (*(volatile uint32_t *)(0x40021000u + 0x24))
#define RCC_CFGR2   (*(volatile uint32_t *)(0x40021000u + 0x2c))
#define RCC_CFGR3   (*(volatile uint32_t *)(0x40021000u + 0x30))
#define RCC_CR2     (*(volatile uint32_t *)(0x40021000u + 0x34))
#define FLASH_ACR   (*(volatile uint32_t *)(0x40022000u + 0x00))
#define PWR_CR      (*(volatile uint32_t *)(0x40007000u + 0x00))

#define SYSTEM_CORE_CLOCK (*(volatile uint32_t *)0x200000c0u)

/* AHB prescaler -> right-shift amount (RCC_CFGR.HPRE index). */
static const uint8_t ahb_presc_table[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6, 7, 8, 9
};

/*
 * hal_rcc_get_hclk_freq — OEM FUN_0801bf0c (HAL_RCC_GetHCLKFreq).
 * Returns the cached SystemCoreClock (HCLK) value.
 */
uint32_t hal_rcc_get_hclk_freq(void)
{
    return SYSTEM_CORE_CLOCK;
}

/*
 * hal_rcc_get_pclk1_freq — OEM FUN_0801bf20 (HAL_RCC_GetPCLK1Freq).
 * PCLK1 = HCLK >> APBPrescTable[RCC_CFGR.PPRE1 (bits 10:8)]. The 8-entry APB
 * table (OEM flash table at 0x0801f188) is reproduced as `static const` .rodata.
 */
uint32_t hal_rcc_get_pclk1_freq(void)
{
    static const uint8_t apb_presc_table[8] = { 0, 0, 0, 0, 1, 2, 3, 4 };
    return hal_rcc_get_hclk_freq() >> apb_presc_table[(RCC_CFGR >> 8) & 7u];
}

/*
 * hal_rcc_get_sysclock_freq — OEM FUN_0801be0c (HAL_RCC_GetSysClockFreq).
 * Compute the current SYSCLK in Hz from RCC_CFGR.SWS and (for PLL) the PLLMUL /
 * PREDIV factor tables. HSI 8 / HSE 16 / HSI48 48 MHz. The two byte tables are
 * `static const` (.rodata), reproducing the OEM flash tables at 0x0801e578 /
 * 0x0801e588; their values and the byte-index reads are disasm-confirmed.
 */
int hal_rcc_get_sysclock_freq(void)
{
    static const uint8_t pllmul_table[16] = {2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,16};
    static const uint8_t prediv_table[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};

    uint32_t cfgr = RCC_CFGR;
    uint32_t sws  = cfgr & 0xcu;

    if (sws == 0x8u) {                              /* PLL */
        uint32_t pllmul = pllmul_table[(cfgr >> 0x12) & 0xfu];
        uint32_t prediv = prediv_table[RCC_CFGR2 & 0xfu];
        uint32_t base;
        if ((cfgr & 0x18000u) == 0x10000u) {
            base = 0x00f42400u;                     /* HSE */
        } else if ((cfgr & 0x18000u) == 0x18000u) {
            base = 0x02dc6c00u;                     /* HSI48 */
        } else {
            base = 0x007a1200u;                     /* HSI */
        }
        return (int)((base / prediv) * pllmul);
    }
    if (sws == 0xcu) { return 0x02dc6c00; }          /* HSI48 */
    if (sws == 0x4u) { return 0x00f42400; }          /* HSE */
    return 0x007a1200;                               /* HSI */
}

/* pwr_enable_bkup_access — OEM FUN_0801b51c (HAL_PWR_EnableBkUpAccess). */
void pwr_enable_bkup_access(void)
{
    PWR_CR |= 0x100u;   /* DBP: disable backup-domain write protection */
}

/*
 * hal_rcc_osc_config — OEM FUN_0801b538 (HAL_RCC_OscConfig).
 *
 * Configure each requested oscillator (HSE/HSI/LSE/LSI/HSI14/HSI48) and the PLL
 * from the RCC_OscInitTypeDef word array, spinning on the matching ready flag
 * with a tick_get()-based timeout. Returns 0 = OK, 1 = ERROR, 3 = TIMEOUT.
 * osc[] index map: [0]OscType [1]HSEState [2]LSEState [3]HSIState [4]HSICalib
 * [5]HSI14State [6]HSI14Calib [7]LSIState [8]HSI48State [9]PLL.State
 * [10]PLL.Source [11]PLL.MUL [12]PLL.PREDIV.
 */
int hal_rcc_osc_config(const uint32_t *osc)
{
    uint32_t t0;
    uint32_t pwrclk_changed;

    /* ---- HSE ---- */
    if ((osc[0] & 1u) != 0u) {
        if (((RCC_CFGR & 0xcu) == 0x4u) ||
            (((RCC_CFGR & 0xcu) == 0x8u) && ((RCC_CFGR & 0x18000u) == 0x10000u))) {
            if (((RCC_CR & 0x20000u) != 0u) && (osc[1] == 0u)) {
                return 1;
            }
        } else {
            if (osc[1] == 1u) {                 /* HSE ON */
                RCC_CR |= 0x10000u;
            } else if (osc[1] == 0u) {          /* HSE OFF */
                RCC_CR &= 0xfffeffffu;
                RCC_CR &= 0xfffbffffu;
            } else if (osc[1] == 5u) {          /* HSE BYPASS */
                RCC_CR |= 0x40000u;
                RCC_CR |= 0x10000u;
            } else {
                RCC_CR &= 0xfffeffffu;
                RCC_CR &= 0xfffbffffu;
            }
            if (osc[1] == 0u) {
                t0 = tick_get();
                while ((RCC_CR & 0x20000u) != 0u) {
                    if ((uint32_t)(tick_get() - t0) > 100u) { return 3; }
                }
            } else {
                t0 = tick_get();
                while ((RCC_CR & 0x20000u) == 0u) {
                    if ((uint32_t)(tick_get() - t0) > 100u) { return 3; }
                }
            }
        }
    }

    /* ---- HSI ---- */
    if ((osc[0] & 2u) != 0u) {
        if (((RCC_CFGR & 0xcu) == 0u) ||
            (((RCC_CFGR & 0xcu) == 0x8u) && ((RCC_CFGR & 0x18000u) == 0x8000u))) {
            if (((RCC_CR & 2u) != 0u) && (osc[3] != 1u)) {
                return 1;
            }
            RCC_CR = (osc[4] << 3) | (RCC_CR & 0xffffff07u);    /* HSITRIM */
        } else if (osc[3] == 0u) {              /* HSI OFF */
            RCC_CR &= 0xfffffffeu;
            t0 = tick_get();
            while ((RCC_CR & 2u) != 0u) {
                if ((uint32_t)(tick_get() - t0) > 2u) { return 3; }
            }
        } else {                                /* HSI ON */
            RCC_CR |= 1u;
            t0 = tick_get();
            while ((RCC_CR & 2u) == 0u) {
                if ((uint32_t)(tick_get() - t0) > 2u) { return 3; }
            }
            RCC_CR = (osc[4] << 3) | (RCC_CR & 0xffffff07u);    /* HSITRIM */
        }
    }

    /* ---- LSI ---- */
    if ((osc[0] & 8u) != 0u) {
        if (osc[7] == 0u) {                     /* LSI OFF */
            RCC_CSR &= 0xfffffffeu;
            t0 = tick_get();
            while ((RCC_CSR & 2u) != 0u) {
                if ((uint32_t)(tick_get() - t0) > 2u) { return 3; }
            }
        } else {                                /* LSI ON */
            RCC_CSR |= 1u;
            t0 = tick_get();
            while ((RCC_CSR & 2u) == 0u) {
                if ((uint32_t)(tick_get() - t0) > 2u) { return 3; }
            }
        }
    }

    /* ---- LSE ---- */
    if ((osc[0] & 4u) != 0u) {
        pwrclk_changed = 0u;
        if ((RCC_APB1ENR & 0x10000000u) == 0u) {
            RCC_APB1ENR |= 0x10000000u;
            (void)(RCC_APB1ENR & 0x10000000u);   /* read-back */
            pwrclk_changed = 1u;
        }
        if ((PWR_CR & 0x100u) == 0u) {
            PWR_CR |= 0x100u;
            t0 = tick_get();
            while ((PWR_CR & 0x100u) == 0u) {
                if ((uint32_t)(tick_get() - t0) > 100u) { return 3; }
            }
        }
        if (osc[2] == 1u) {                     /* LSE ON */
            RCC_BDCR |= 1u;
        } else if (osc[2] == 0u) {              /* LSE OFF */
            RCC_BDCR &= 0xfffffffeu;
            RCC_BDCR &= 0xfffffffbu;
        } else if (osc[2] == 5u) {              /* LSE BYPASS */
            RCC_BDCR |= 4u;
            RCC_BDCR |= 1u;
        } else {
            RCC_BDCR &= 0xfffffffeu;
            RCC_BDCR &= 0xfffffffbu;
        }
        if (osc[2] == 0u) {
            t0 = tick_get();
            while ((RCC_BDCR & 2u) != 0u) {
                if ((uint32_t)(tick_get() - t0) > 5000u) { return 3; }
            }
        } else {
            t0 = tick_get();
            while ((RCC_BDCR & 2u) == 0u) {
                if ((uint32_t)(tick_get() - t0) > 5000u) { return 3; }
            }
        }
        if (pwrclk_changed == 1u) {
            RCC_APB1ENR &= 0xefffffffu;
        }
    }

    /* ---- HSI14 ---- */
    if ((osc[0] & 0x10u) != 0u) {
        if (osc[5] == 1u) {                     /* HSI14 ON */
            RCC_CR2 |= 4u;
            RCC_CR2 |= 1u;
            t0 = tick_get();
            while ((RCC_CR2 & 2u) == 0u) {
                if ((uint32_t)(tick_get() - t0) > 2u) { return 3; }
            }
            RCC_CR2 = (osc[6] << 3) | (RCC_CR2 & 0xffffff07u);  /* HSI14TRIM */
        } else if (osc[5] == 0xfffffffbu) {     /* keep on, ADC-disabled */
            RCC_CR2 &= 0xfffffffbu;
            RCC_CR2 = (osc[6] << 3) | (RCC_CR2 & 0xffffff07u);
        } else {                                /* HSI14 OFF */
            RCC_CR2 |= 4u;
            RCC_CR2 &= 0xfffffffeu;
            t0 = tick_get();
            while ((RCC_CR2 & 2u) != 0u) {
                if ((uint32_t)(tick_get() - t0) > 2u) { return 3; }
            }
        }
    }

    /* ---- HSI48 ---- */
    if ((osc[0] & 0x20u) != 0u) {
        if (((RCC_CFGR & 0xcu) == 0xcu) ||
            (((RCC_CFGR & 0xcu) == 0x8u) && ((RCC_CFGR & 0x18000u) == 0x18000u))) {
            if (((RCC_CR2 & 0x10000u) != 0u) && (osc[8] != 1u)) {
                return 1;
            }
        } else if (osc[8] == 0u) {              /* HSI48 OFF */
            RCC_CR2 &= 0xfffeffffu;
            t0 = tick_get();
            while ((RCC_CR2 & 0x10000u) != 0u) {
                if ((uint32_t)(tick_get() - t0) > 2u) { return 3; }
            }
        } else {                                /* HSI48 ON */
            RCC_CR2 |= 0x10000u;
            t0 = tick_get();
            while ((RCC_CR2 & 0x10000u) == 0u) {
                if ((uint32_t)(tick_get() - t0) > 2u) { return 3; }
            }
        }
    }

    /* ---- PLL ---- */
    if (osc[9] != 0u) {
        if ((RCC_CFGR & 0xcu) == 0x8u) {        /* PLL is the system clock */
            return 1;
        }
        if (osc[9] == 2u) {                     /* PLL ON */
            RCC_CR &= 0xfeffffffu;              /* PLLON = 0 */
            t0 = tick_get();
            while ((RCC_CR & 0x2000000u) != 0u) {
                if ((uint32_t)(tick_get() - t0) > 2u) { return 3; }
            }
            RCC_CFGR2 = osc[12] | (RCC_CFGR2 & 0xfffffff0u);          /* PREDIV */
            RCC_CFGR  = osc[10] | osc[11] | (RCC_CFGR & 0xffc27fffu); /* SRC|MUL */
            RCC_CR   |= 0x1000000u;             /* PLLON = 1 */
            t0 = tick_get();
            while ((RCC_CR & 0x2000000u) == 0u) {
                if ((uint32_t)(tick_get() - t0) > 2u) { return 3; }
            }
        } else {                                /* PLL OFF */
            RCC_CR &= 0xfeffffffu;
            t0 = tick_get();
            while ((RCC_CR & 0x2000000u) != 0u) {
                if ((uint32_t)(tick_get() - t0) > 2u) { return 3; }
            }
        }
    }

    return 0;
}

/*
 * hal_rcc_clock_config — OEM FUN_0801bbf8 (HAL_RCC_ClockConfig).
 *
 * FLASH latency + SYSCLK source switch + AHB/APB1 prescalers. Raises flash
 * latency before speeding up and lowers it after slowing down; spins on
 * RCC_CFGR.SWS to confirm the switch. Recomputes SystemCoreClock and the
 * SysTick base. clk[] = [0]ClockType [1]SYSCLKSource [2]AHBDiv [3]APB1Div;
 * `flatency` is the FLASH_ACR latency. Returns 0 = OK, 1 = ERROR, 3 = TIMEOUT.
 */
int hal_rcc_clock_config(const uint32_t *clk, uint32_t flatency)
{
    uint32_t t0;

    if ((FLASH_ACR & 1u) < flatency) {
        FLASH_ACR = (FLASH_ACR & 0xfffffffeu) | flatency;
        if (flatency != (FLASH_ACR & 1u)) { return 1; }
    }

    if ((clk[0] & 2u) != 0u) {                              /* HCLK: AHB prescaler */
        RCC_CFGR = (RCC_CFGR & 0xffffff0fu) | clk[2];
    }

    if ((clk[0] & 1u) != 0u) {                              /* SYSCLK source */
        if (clk[1] == 1u) {
            if ((RCC_CR & 0x20000u) == 0u) { return 1; }    /* HSERDY */
        } else if (clk[1] == 2u) {
            if ((RCC_CR & 0x2000000u) == 0u) { return 1; }  /* PLLRDY */
        } else if (clk[1] == 3u) {
            if ((RCC_CR2 & 0x10000u) == 0u) { return 1; }   /* HSI48RDY */
        } else {
            if ((RCC_CR & 2u) == 0u) { return 1; }          /* HSIRDY */
        }
        RCC_CFGR = (RCC_CFGR & 0xfffffffcu) | clk[1];       /* SW */
        t0 = tick_get();
        if (clk[1] == 1u) {
            while ((RCC_CFGR & 0xcu) != 0x4u) {
                if ((uint32_t)(tick_get() - t0) > 5000u) { return 3; }
            }
        } else if (clk[1] == 2u) {
            while ((RCC_CFGR & 0xcu) != 0x8u) {
                if ((uint32_t)(tick_get() - t0) > 5000u) { return 3; }
            }
        } else if (clk[1] == 3u) {
            while ((RCC_CFGR & 0xcu) != 0xcu) {
                if ((uint32_t)(tick_get() - t0) > 5000u) { return 3; }
            }
        } else {
            while ((RCC_CFGR & 0xcu) != 0x0u) {
                if ((uint32_t)(tick_get() - t0) > 5000u) { return 3; }
            }
        }
    }

    if (flatency < (FLASH_ACR & 1u)) {
        FLASH_ACR = (FLASH_ACR & 0xfffffffeu) | flatency;
        if (flatency != (FLASH_ACR & 1u)) { return 1; }
    }

    if ((clk[0] & 4u) != 0u) {                              /* PCLK1: APB1 prescaler */
        RCC_CFGR = (RCC_CFGR & 0xfffff8ffu) | clk[3];
    }

    SYSTEM_CORE_CLOCK = hal_rcc_get_sysclock_freq() >> ahb_presc_table[(RCC_CFGR >> 4) & 0xfu];
    hal_init_tick(0);                                       /* reprogram SysTick */
    return 0;
}

/*
 * hal_rccex_periph_clk_config — OEM FUN_0801bf4c (HAL_RCCEx_PeriphCLKConfig).
 *
 * Selects the RTC clock (RCC_BDCR.RTCSEL, with the backup-domain unlock + BDRST
 * reset pulse + LSE re-ready wait when RTCSEL changes) and the USART1/2/3, I2C1
 * and USB clock muxes (RCC_CFGR3). pclk[] = [0]Selection [1]RTCSel [2]Usart1
 * [3]Usart2 [4]Usart3 [5]I2c1 [6]Usb. Returns 0 = OK, 3 = TIMEOUT.
 */
int hal_rccex_periph_clk_config(const uint32_t *pclk)
{
    uint32_t t0;
    uint32_t tmpreg;

    if ((pclk[0] & 0x10000u) != 0u) {                       /* RTC */
        uint8_t pwrclkchanged = 0;
        if ((RCC_APB1ENR & 0x10000000u) == 0u) {
            RCC_APB1ENR |= 0x10000000u;
            (void)(RCC_APB1ENR & 0x10000000u);              /* read-back */
            pwrclkchanged = 1;
        }
        if ((PWR_CR & 0x100u) == 0u) {
            PWR_CR |= 0x100u;
            t0 = tick_get();
            while ((PWR_CR & 0x100u) == 0u) {
                if ((uint32_t)(tick_get() - t0) > 100u) { return 3; }
            }
        }
        tmpreg = RCC_BDCR & 0x300u;
        if ((tmpreg != 0u) && (tmpreg != (pclk[1] & 0x300u))) {
            tmpreg = RCC_BDCR & 0xfffffcffu;                /* preserve all but RTCSEL */
            RCC_BDCR |= 0x10000u;                           /* BDRST set */
            RCC_BDCR &= 0xfffeffffu;                        /* BDRST clear */
            RCC_BDCR  = tmpreg;
            if ((tmpreg & 1u) != 0u) {                      /* LSEON: re-wait LSERDY */
                t0 = tick_get();
                while ((RCC_BDCR & 2u) == 0u) {
                    if ((uint32_t)(tick_get() - t0) > 5000u) { return 3; }
                }
            }
        }
        RCC_BDCR = (RCC_BDCR & 0xfffffcffu) | pclk[1];      /* RTCSEL */
        if (pwrclkchanged == 1) {
            RCC_APB1ENR &= 0xefffffffu;
        }
    }

    if ((pclk[0] & 1u) != 0u) {                             /* USART1 */
        RCC_CFGR3 = (RCC_CFGR3 & 0xfffffffcu) | pclk[2];
    }
    if ((pclk[0] & 2u) != 0u) {                             /* USART2 */
        RCC_CFGR3 = (RCC_CFGR3 & 0xfffcffffu) | pclk[3];
    }
    if ((pclk[0] & 0x40000u) != 0u) {                       /* USART3 */
        RCC_CFGR3 = (RCC_CFGR3 & 0xfff3ffffu) | pclk[4];
    }
    if ((pclk[0] & 0x20u) != 0u) {                          /* I2C1 */
        RCC_CFGR3 = (RCC_CFGR3 & 0xffffffefu) | pclk[5];
    }
    if ((pclk[0] & 0x400u) != 0u) {                         /* USB */
        RCC_CFGR3 = (RCC_CFGR3 & 0xffffffbfu) | pclk[6];
    }
    return 0;
}

/*
 * system_core_clock_update — OEM FUN_0801d9c0 (SystemCoreClockUpdate).
 * Recompute the SystemCoreClock global (0x200000c0) from RCC_CFGR.SWS, the PLL
 * fields and the AHB prescaler. AHB table read via ldrb (unsigned).
 */
void system_core_clock_update(void)
{
    uint32_t sws = RCC_CFGR & 0xcu;

    if (sws == 0x4u) {
        SYSTEM_CORE_CLOCK = 0x00f42400u;        /* HSE = 16 MHz */
    } else if (sws == 0x8u) {                   /* PLL */
        uint32_t pllsrc = RCC_CFGR & 0x18000u;
        uint32_t pllmul = ((RCC_CFGR & 0x3c0000u) >> 18) + 2;
        uint32_t prediv = (RCC_CFGR2 & 0xfu) + 1;
        uint32_t base;
        if (pllsrc == 0x10000u) {
            base = 0x00f42400u;                 /* HSE */
        } else if (pllsrc == 0x18000u) {
            base = 0x02dc6c00u;                 /* HSI48 */
        } else {
            base = 0x007a1200u;                 /* HSI = 8 MHz */
        }
        SYSTEM_CORE_CLOCK = pllmul * (base / prediv);
    } else {
        SYSTEM_CORE_CLOCK = 0x007a1200u;        /* HSI = 8 MHz */
    }

    SYSTEM_CORE_CLOCK >>= ahb_presc_table[(RCC_CFGR >> 4) & 0xfu];
}

/*
 * clock_rtc_init — OEM FUN_080136c0.
 *
 * System clock tree + RTC bring-up, called from hal_bringup. Enables backup-
 * domain write access, sets the LSE drive strength, runs the HAL oscillator /
 * clock / peripheral-clock config, enables the RTC clock, then inits the RTC
 * peripheral and recomputes SystemCoreClock.
 *
 * The HAL init structs are zeroed with the project mem_set (an explicit call —
 * a `= {0}` initialiser would lower to a libc memset that this -nostdlib build
 * cannot link). Note the local names: `pclk` (4 words) is the RCC_ClkInitTypeDef
 * fed to hal_rcc_clock_config; `clk` (7 words) is the RCC_PeriphCLKInitTypeDef
 * fed to hal_rccex_periph_clk_config (named from the OEM stack, which orders
 * them this way). Offsets/constants disasm-confirmed.
 */
void clock_rtc_init(void)
{
    uint32_t osc[13];   /* RCC_OscInitTypeDef        (0x34) */
    uint32_t pclk[4];   /* RCC_ClkInitTypeDef        (0x10) -> hal_rcc_clock_config */
    uint32_t clk[7];    /* RCC_PeriphCLKInitTypeDef  (0x1c) -> hal_rccex_periph_clk_config */

    mem_set(osc, 0, sizeof osc);
    mem_set(pclk, 0, sizeof pclk);
    mem_set(clk, 0, sizeof clk);

    pwr_enable_bkup_access();                             /* PWR_CR |= DBP */
    RCC_BDCR |= 0x18u;                                    /* LSEDRV = 0b11 */

    osc[0]  = 0x0000000du;   /* OscillatorType = HSE | LSE | LSI */
    osc[1]  = 1;
    osc[2]  = 1;
    osc[5]  = 0;
    osc[6]  = 0x10;
    osc[7]  = 1;
    osc[4]  = 0x10;
    osc[9]  = 2;
    osc[10] = 0x00010000u;
    osc[11] = 0;
    osc[12] = 0;
    *(volatile uint32_t *)0x20002614 = 0;                 /* reset ms tick */
    if (hal_rcc_osc_config(osc) != 0) { spi_error_reset(); }

    pclk[0] = 7;
    pclk[1] = 2;
    pclk[2] = 0;
    pclk[3] = 0;
    if (hal_rcc_clock_config(pclk, 1) != 0) { spi_error_reset(); }

    clk[0] = 0x00010003u;
    clk[2] = 0;
    clk[3] = 0;
    clk[1] = 0x100;
    if (hal_rccex_periph_clk_config(clk) != 0) { spi_error_reset(); }

    RCC_BDCR |= 0x8000u;                                  /* RTCEN */

    /* RTC handle @0x200006f0 (shared with rtc.c): Instance = RTC (0x40002800). */
    volatile uint32_t *hrtc = (volatile uint32_t *)0x200006f0;
    hrtc[0] = 0x40002800u;
    hrtc[1] = 0;
    hrtc[2] = 0x7f;
    hrtc[3] = 0xff;
    hrtc[4] = 0;
    hrtc[5] = 0;
    hrtc[6] = 0;
    if (hal_rtc_init((void *)hrtc) != 0) { spi_error_reset(); }

    system_core_clock_update();
}

/*
 * SystemInit — OEM FUN_0801d938.
 *
 * The CMSIS pre-main reset of the RCC clock tree, run from Reset_Handler before
 * __libc_init_array / main: force HSI on, then clear every clock-select,
 * prescaler, PLL, HSE, peripheral-clock-source and HSI14 field back to its reset
 * state and disable the RCC interrupts. The mask constants are this part's reset
 * values — note CFGR3 is 0xfffcfe2c here, not the smaller-part 0xfffffeac — and
 * the register offsets/widths are disasm-confirmed against the OEM image.
 */
void SystemInit(void)
{
    RCC_CR   |= 0x00000001u;   /* CR:    HSION */
    RCC_CFGR &= 0x08ffb80cu;   /* CFGR:  reset SW/HPRE/PPRE/ADCPRE/MCO[PRE] */
    RCC_CR   &= 0xfef6ffffu;   /* CR:    reset HSEON/CSSON/PLLON */
    RCC_CR   &= 0xfffbffffu;   /* CR:    reset HSEBYP */
    RCC_CFGR &= 0xffc0ffffu;   /* CFGR:  reset PLLSRC/PLLXTPRE/PLLMUL */
    RCC_CFGR2 &= 0xfffffff0u;  /* CFGR2: reset PREDIV */
    RCC_CFGR3 &= 0xfffcfe2cu;  /* CFGR3: reset USART/I2C/CEC/ADC clock selects */
    RCC_CR2  &= 0xfffffffeu;   /* CR2:   reset HSI14ON */
    RCC_CIR   = 0x00000000u;   /* CIR:   disable all RCC interrupts */
}

/*
 * __libc_init_array_lite — OEM FUN_0801dac4 (the standard __libc_init_array),
 * run by Reset_Handler before main(). Execute the .preinit_array constructors,
 * then the .init_array constructors; the OEM's empty _init() (FUN_0801db30)
 * between them is omitted (no effect). Array bounds are the linker-script
 * PROVIDE_HIDDEN symbols. In this rebuild both arrays are empty — the OEM's lone
 * init_array entry is GCC's register_tm_clones, a no-op when the TM clone table
 * is null — so no constructor runs.
 */
extern void (* const __preinit_array_start[])(void);
extern void (* const __preinit_array_end[])(void);
extern void (* const __init_array_start[])(void);
extern void (* const __init_array_end[])(void);

void __libc_init_array_lite(void)
{
    void (* const *fn)(void);

    for (fn = __preinit_array_start; fn != __preinit_array_end; fn++) {
        (*fn)();
    }
    for (fn = __init_array_start; fn != __init_array_end; fn++) {
        (*fn)();
    }
}

/*
 * SysTick_Handler — OEM FUN_08014a14 (size 162 bytes).
 *
 * The 1 ms system-tick ISR and sole producer of the firmware's time base.
 * It advances the free-running 32-bit ms counter that tick_get() reads
 * (0x20002614), advances a 16-bit sub-counter (0x20000778, cleared by
 * tick_state_reset), and OR-folds the software tick-flag byte (0x2000077c,
 * consumed by delay_ms / board_init):
 *   bit0 set every 1 ms, bit1 every 50 ms, bit2 every 250 ms, bit3 each
 *   full 1000 ms wrap (the |=0xf / |=7 / |=3 / |=1 masks).
 *
 * Disasm-confirmed against the OEM image: the 32-bit counter is ldr/str, the
 * sub-counter ldrh/strh with 16-bit wrap (uxth), the flag byte ldrb/strb. The
 * wrap test is `cmp counter, #0x3e7` + `bls` (unsigned), i.e. reset and |=0xf
 * once the post-increment sub-counter exceeds 999 (>= 1000). The two
 * FUN_0800823c calls are the unsigned div/mod helper taking the remainder,
 * here the C `%` operator on the re-loaded (== incremented) sub-counter.
 * 0x3e7 is an inline immediate, not a pointer.
 */
void SysTick_Handler(void)
{
    volatile uint32_t * const uw_tick   = (volatile uint32_t *)0x20002614u;
    volatile uint16_t * const sub_count = (volatile uint16_t *)0x20000778u;
    volatile uint8_t  * const tick_flag = (volatile uint8_t  *)0x2000077cu;

    *uw_tick = *uw_tick + 1u;

    uint16_t count = (uint16_t)(*sub_count + 1u);
    *sub_count = count;

    if (count > 999u) {
        *sub_count = 0;
        *tick_flag |= 0x0fu;
    } else if ((count % 250u) == 0u) {
        *tick_flag |= 0x07u;
    } else if ((count % 50u) == 0u) {
        *tick_flag |= 0x03u;
    } else {
        *tick_flag |= 0x01u;
    }
}
