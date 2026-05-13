#ifndef MM32F031_H
#define MM32F031_H

/*
 * MM32F031 — minimal MMIO definitions.
 *
 * Targeted at MM32F031F6U6 (LQFP-20). Layout follows the MM32F031xx
 * reference manual; field naming mirrors STM32F031 / CMSIS conventions
 * since the silicon is a near-clone and the names are widely recognised.
 *
 * Add to this file as we discover which registers/fields the OEM
 * shifterware actually touches. Don't pull in vendor SDK headers; copy
 * the small bits we need so the build stays self-contained.
 */

#include <stdint.h>
#include "compiler.h"

#define __IO   volatile
#define __I    volatile const
#define __O    volatile

/* ---------- Memory map base addresses ----------------------------------- */
#define FLASH_BASE          (0x08000000UL)
#define SRAM_BASE           (0x20000000UL)
#define PERIPH_BASE         (0x40000000UL)
#define APB1_BASE           (PERIPH_BASE + 0x00000000UL)
#define APB2_BASE           (PERIPH_BASE + 0x00010000UL)
#define AHB1_BASE           (PERIPH_BASE + 0x00020000UL)
#define AHB2_BASE           (0x48000000UL)

/* ---------- Peripherals — APB1 ------------------------------------------ */
#define TIM2_BASE           (APB1_BASE + 0x0000)
#define TIM3_BASE           (APB1_BASE + 0x0400)
#define TIM6_BASE           (APB1_BASE + 0x1000)
#define TIM14_BASE          (APB1_BASE + 0x2000)
#define RTC_BASE            (APB1_BASE + 0x2800)
#define WWDG_BASE           (APB1_BASE + 0x2C00)
#define IWDG_BASE           (APB1_BASE + 0x3000)
#define SPI2_BASE           (APB1_BASE + 0x3800)
#define USART2_BASE         (APB1_BASE + 0x4400)
#define I2C1_BASE           (APB1_BASE + 0x5400)
#define I2C2_BASE           (APB1_BASE + 0x5800)
#define PWR_BASE            (APB1_BASE + 0x7000)

/* ---------- Peripherals — APB2 ------------------------------------------ */
#define SYSCFG_BASE         (APB2_BASE + 0x0000)
#define EXTI_BASE           (APB2_BASE + 0x0400)
#define ADC1_BASE           (APB2_BASE + 0x2400)
#define TIM1_BASE           (APB2_BASE + 0x2C00)
#define SPI1_BASE           (APB2_BASE + 0x3000)
#define USART1_BASE         (APB2_BASE + 0x3800)
#define TIM15_BASE          (APB2_BASE + 0x4000)
#define TIM16_BASE          (APB2_BASE + 0x4400)
#define TIM17_BASE          (APB2_BASE + 0x4800)
#define DBGMCU_BASE         (APB2_BASE + 0x5800)

/* ---------- Peripherals — AHB1 ------------------------------------------ */
#define DMA1_BASE           (AHB1_BASE + 0x0000)
#define RCC_BASE            (AHB1_BASE + 0x1000)
#define FLASH_R_BASE        (AHB1_BASE + 0x2000)
#define CRC_BASE            (AHB1_BASE + 0x3000)

/* ---------- Peripherals — AHB2 (GPIO) ----------------------------------- */
#define GPIOA_BASE          (AHB2_BASE + 0x0000)
#define GPIOB_BASE          (AHB2_BASE + 0x0400)
#define GPIOC_BASE          (AHB2_BASE + 0x0800)
#define GPIOD_BASE          (AHB2_BASE + 0x0C00)
#define GPIOF_BASE          (AHB2_BASE + 0x1400)

/* ---------- Cortex-M0 Core Peripherals ---------------------------------- */
#define SCS_BASE            (0xE000E000UL)
#define SYSTICK_BASE        (SCS_BASE + 0x0010)
#define NVIC_BASE           (SCS_BASE + 0x0100)
#define SCB_BASE            (SCS_BASE + 0x0D00)

/* ---------- RCC — minimal ----------------------------------------------- */
typedef struct {
    __IO uint32_t CR;        /* 0x00 */
    __IO uint32_t CFGR;      /* 0x04 */
    __IO uint32_t CIR;       /* 0x08 */
    __IO uint32_t APB2RSTR;  /* 0x0C */
    __IO uint32_t APB1RSTR;  /* 0x10 */
    __IO uint32_t AHBENR;    /* 0x14 */
    __IO uint32_t APB2ENR;   /* 0x18 */
    __IO uint32_t APB1ENR;   /* 0x1C */
    __IO uint32_t BDCR;      /* 0x20 */
    __IO uint32_t CSR;       /* 0x24 */
    __IO uint32_t AHBRSTR;   /* 0x28 */
    __IO uint32_t CFGR2;     /* 0x2C */
    __IO uint32_t CFGR3;     /* 0x30 */
    __IO uint32_t CR2;       /* 0x34 */
} rcc_t;
#define RCC                 ((rcc_t *)RCC_BASE)

/* ---------- GPIO -------------------------------------------------------- */
typedef struct {
    __IO uint32_t MODER;     /* 0x00 */
    __IO uint32_t OTYPER;    /* 0x04 */
    __IO uint32_t OSPEEDR;   /* 0x08 */
    __IO uint32_t PUPDR;     /* 0x0C */
    __I  uint32_t IDR;       /* 0x10 */
    __IO uint32_t ODR;       /* 0x14 */
    __O  uint32_t BSRR;      /* 0x18 */
    __IO uint32_t LCKR;      /* 0x1C */
    __IO uint32_t AFR[2];    /* 0x20-0x24 */
    __O  uint32_t BRR;       /* 0x28 */
} gpio_t;
#define GPIOA               ((gpio_t *)GPIOA_BASE)
#define GPIOB               ((gpio_t *)GPIOB_BASE)
#define GPIOC               ((gpio_t *)GPIOC_BASE)
#define GPIOD               ((gpio_t *)GPIOD_BASE)
#define GPIOF               ((gpio_t *)GPIOF_BASE)

/* ---------- USART (partially confirmed from OEM image) ----------------
 *
 * The MM32F031xx USART register map is NOT STM32F0-compatible. The
 * fields below are derived from observed OEM accesses; offsets and
 * field meanings marked TBD are guesses or unknown until the
 * corresponding init/RX/IRQ helpers are decomp'd. The earlier
 * (speculative) STM32F0 layout is preserved in mm32f031.h.bak.
 */
typedef struct {
    __O  uint32_t TDR;       /* 0x00 — TX data (write only) */
    __I  uint32_t RDR;       /* 0x04 — RX data (read only) */
    __IO uint32_t SR;        /* 0x08 — status; bit 0 = TX ready/complete */
    __I  uint32_t ISR;       /* 0x0C — interrupt-status; bit 1 = RX data available */
    __IO uint32_t IER;       /* 0x10 — interrupt-enable (toggled by usart_cr10_bits) */
    __O  uint32_t ICR;       /* 0x14 — interrupt-clear (write bit to clear) */
    __IO uint32_t CCR;       /* 0x18 — channel control; bit 0 = UE, bits 0-4 framing */
    __IO uint32_t FCR;       /* 0x1C — frame config; bits 0-3, 6-7 = parity/stop/bits */
    __IO uint32_t BRR_INT;   /* 0x20 — baud divisor integer (divisor >> 4) */
    __IO uint32_t BRR_FRA;   /* 0x24 — baud divisor fractional (divisor & 0xF) */
} usart_t;
#define USART1              ((usart_t *)USART1_BASE)
#define USART2              ((usart_t *)USART2_BASE)

/* ---------- TIM (general purpose / advanced layout) --------------------- */
typedef struct {
    __IO uint32_t CR1;       /* 0x00 */
    __IO uint32_t CR2;       /* 0x04 */
    __IO uint32_t SMCR;      /* 0x08 */
    __IO uint32_t DIER;      /* 0x0C */
    __IO uint32_t SR;        /* 0x10 */
    __O  uint32_t EGR;       /* 0x14 */
    __IO uint32_t CCMR1;     /* 0x18 */
    __IO uint32_t CCMR2;     /* 0x1C */
    __IO uint32_t CCER;      /* 0x20 */
    __IO uint32_t CNT;       /* 0x24 */
    __IO uint32_t PSC;       /* 0x28 */
    __IO uint32_t ARR;       /* 0x2C */
    __IO uint32_t RCR;       /* 0x30 */
    __IO uint32_t CCR1;      /* 0x34 */
    __IO uint32_t CCR2;      /* 0x38 */
    __IO uint32_t CCR3;      /* 0x3C */
    __IO uint32_t CCR4;      /* 0x40 */
    __IO uint32_t BDTR;      /* 0x44 */
    __IO uint32_t DCR;       /* 0x48 */
    __IO uint32_t DMAR;      /* 0x4C */
} tim_t;
#define TIM1                ((tim_t *)TIM1_BASE)
#define TIM2                ((tim_t *)TIM2_BASE)
#define TIM3                ((tim_t *)TIM3_BASE)
#define TIM6                ((tim_t *)TIM6_BASE)
#define TIM14               ((tim_t *)TIM14_BASE)
#define TIM15               ((tim_t *)TIM15_BASE)
#define TIM16               ((tim_t *)TIM16_BASE)
#define TIM17               ((tim_t *)TIM17_BASE)

#define TIM_CR1_CEN_Msk         (1u << 0)
#define TIM_CR1_OPM_Msk         (1u << 3)
#define TIM_CR1_ARPE_Msk        (1u << 7)
#define TIM_DIER_UIE_Msk        (1u << 0)
#define TIM_SR_UIF_Msk          (1u << 0)
#define TIM_EGR_UG_Msk          (1u << 0)
#define TIM_CCMR1_OC1M_PWM1     (0x6u << 4)
#define TIM_CCMR1_OC1PE_Msk     (1u << 3)
#define TIM_CCER_CC1E_Msk       (1u << 0)
#define TIM_BDTR_MOE_Msk        (1u << 15)

/* ---------- ADC --------------------------------------------------------- */
typedef struct {
    __IO uint32_t ISR;       /* 0x00 */
    __IO uint32_t IER;       /* 0x04 */
    __IO uint32_t CR;        /* 0x08 */
    __IO uint32_t CFGR1;     /* 0x0C */
    __IO uint32_t CFGR2;     /* 0x10 */
    __IO uint32_t SMPR;      /* 0x14 */
    uint32_t      _r0[2];
    __IO uint32_t TR;        /* 0x20 */
    uint32_t      _r1;
    __IO uint32_t CHSELR;    /* 0x28 */
    uint32_t      _r2[5];
    __I  uint32_t DR;        /* 0x40 */
} adc_t;
#define ADC1                ((adc_t *)ADC1_BASE)

#define ADC_CR_ADEN_Msk         (1u << 0)
#define ADC_CR_ADDIS_Msk        (1u << 1)
#define ADC_CR_ADSTART_Msk      (1u << 2)
#define ADC_CR_ADCAL_Msk        (1u << 31)
#define ADC_ISR_ADRDY_Msk       (1u << 0)
#define ADC_ISR_EOC_Msk         (1u << 2)
#define ADC_ISR_EOSEQ_Msk       (1u << 3)

/* ---------- I2C --------------------------------------------------------- */
typedef struct {
    __IO uint32_t CR1;       /* 0x00 */
    __IO uint32_t CR2;       /* 0x04 */
    __IO uint32_t OAR1;      /* 0x08 */
    __IO uint32_t OAR2;      /* 0x0C */
    __IO uint32_t TIMINGR;   /* 0x10 */
    __IO uint32_t TIMEOUTR;  /* 0x14 */
    __IO uint32_t ISR;       /* 0x18 */
    __O  uint32_t ICR;       /* 0x1C */
    __I  uint32_t PECR;      /* 0x20 */
    __I  uint32_t RXDR;      /* 0x24 */
    __IO uint32_t TXDR;      /* 0x28 */
} i2c_t;
#define I2C1                ((i2c_t *)I2C1_BASE)
#define I2C2                ((i2c_t *)I2C2_BASE)

#define I2C_CR1_PE_Msk          (1u << 0)
#define I2C_CR2_START_Msk       (1u << 13)
#define I2C_CR2_STOP_Msk        (1u << 14)
#define I2C_CR2_AUTOEND_Msk     (1u << 25)
#define I2C_CR2_RD_WRN_Msk      (1u << 10)
#define I2C_ISR_TXIS_Msk        (1u << 1)
#define I2C_ISR_RXNE_Msk        (1u << 2)
#define I2C_ISR_TC_Msk          (1u << 6)
#define I2C_ISR_STOPF_Msk       (1u << 5)
#define I2C_ISR_NACKF_Msk       (1u << 4)
#define I2C_ISR_BUSY_Msk        (1u << 15)
#define I2C_ICR_STOPCF_Msk      (1u << 5)
#define I2C_ICR_NACKCF_Msk      (1u << 4)

/* ---------- USART bits --------------------------------------------------
 *
 * Most of the old STM32F0-style bit names below referenced fields in
 * the wrong registers under the corrected struct. The only confirmed
 * bit is in SR (offset 0x08). The remaining macros are commented out
 * until the OEM init/RX is decomp'd and the correct CR/SR semantics
 * can be re-derived.
 */
#define USART_SR_TX_READY_Msk    (1u << 0)  /* TX-complete / ready-for-next */
#define USART_ISR_RX_READY_Msk   (1u << 1)  /* RX data available (in ISR @ 0x0C, not SR) */

/* speculative; left for reference but not currently usable:
#define USART_CR1_UE_Msk        (1u << 0)
#define USART_CR1_RE_Msk        (1u << 2)
#define USART_CR1_TE_Msk        (1u << 3)
#define USART_CR1_RXNEIE_Msk    (1u << 5)
#define USART_ISR_RXNE_Msk      (1u << 5)
#define USART_ISR_TC_Msk        (1u << 6)
#define USART_ISR_TXE_Msk       (1u << 7)
*/

/* ---------- RCC enable bits we use ------------------------------------- */
#define RCC_AHBENR_DMA1EN_Msk   (1u <<  0)
#define RCC_AHBENR_SRAMEN_Msk   (1u <<  2)
#define RCC_AHBENR_FLITFEN_Msk  (1u <<  4)
#define RCC_AHBENR_CRCEN_Msk    (1u <<  6)
#define RCC_AHBENR_IOPAEN_Msk   (1u << 17)
#define RCC_AHBENR_IOPBEN_Msk   (1u << 18)
#define RCC_AHBENR_IOPCEN_Msk   (1u << 19)
#define RCC_AHBENR_IOPDEN_Msk   (1u << 20)
#define RCC_AHBENR_IOPFEN_Msk   (1u << 22)

#define RCC_APB1ENR_TIM2EN_Msk     (1u <<  0)
#define RCC_APB1ENR_TIM3EN_Msk     (1u <<  1)
#define RCC_APB1ENR_TIM6EN_Msk     (1u <<  4)
#define RCC_APB1ENR_TIM14EN_Msk    (1u <<  8)
#define RCC_APB1ENR_WWDGEN_Msk     (1u << 11)
#define RCC_APB1ENR_USART2EN_Msk   (1u << 17)
#define RCC_APB1ENR_I2C1EN_Msk     (1u << 21)
#define RCC_APB1ENR_I2C2EN_Msk     (1u << 22)
#define RCC_APB1ENR_PWREN_Msk      (1u << 28)

#define RCC_APB2ENR_SYSCFGEN_Msk   (1u <<  0)
#define RCC_APB2ENR_ADCEN_Msk      (1u <<  9)
#define RCC_APB2ENR_TIM1EN_Msk     (1u << 11)
#define RCC_APB2ENR_SPI1EN_Msk     (1u << 12)
#define RCC_APB2ENR_USART1EN_Msk   (1u << 14)
#define RCC_APB2ENR_TIM15EN_Msk    (1u << 16)
#define RCC_APB2ENR_TIM16EN_Msk    (1u << 17)
#define RCC_APB2ENR_TIM17EN_Msk    (1u << 18)
#define RCC_APB2ENR_DBGMCUEN_Msk   (1u << 22)

#define RCC_CR_HSION_Msk       (1u <<  0)
#define RCC_CR_HSIRDY_Msk      (1u <<  1)
#define RCC_CR_PLLON_Msk       (1u << 24)
#define RCC_CR_PLLRDY_Msk      (1u << 25)
#define RCC_CFGR_SW_Msk        (0x3u << 0)
#define RCC_CFGR_SW_PLL        (0x2u << 0)
#define RCC_CFGR_SWS_Msk       (0x3u << 2)
#define RCC_CFGR_SWS_PLL       (0x2u << 2)
#define RCC_CFGR_PLLSRC_HSI_DIV2 (0u << 16)
#define RCC_CFGR_PLLMUL_12     (0xAu << 18)   /* HSI/2 * 12 = 48 MHz */

/* ---------- EXTI -------------------------------------------------------- */
typedef struct {
    __IO uint32_t IMR;       /* 0x00 */
    __IO uint32_t EMR;       /* 0x04 */
    __IO uint32_t RTSR;      /* 0x08 */
    __IO uint32_t FTSR;      /* 0x0C */
    __IO uint32_t SWIER;     /* 0x10 */
    __IO uint32_t PR;        /* 0x14 */
} exti_t;
#define EXTI                ((exti_t *)EXTI_BASE)

/* ---------- SYSCFG ------------------------------------------------------ */
typedef struct {
    __IO uint32_t CFGR1;     /* 0x00 */
    uint32_t      _r0;
    __IO uint32_t EXTICR[4]; /* 0x08-0x14 */
    __IO uint32_t CFGR2;     /* 0x18 */
} syscfg_t;
#define SYSCFG              ((syscfg_t *)SYSCFG_BASE)

/* ---------- SPI --------------------------------------------------------- */
typedef struct {
    __IO uint32_t CR1;       /* 0x00 */
    __IO uint32_t CR2;       /* 0x04 */
    __IO uint32_t SR;        /* 0x08 */
    __IO uint32_t DR;        /* 0x0C */
    __IO uint32_t CRCPR;     /* 0x10 */
    __I  uint32_t RXCRCR;    /* 0x14 */
    __I  uint32_t TXCRCR;    /* 0x18 */
    __IO uint32_t I2SCFGR;   /* 0x1C */
    __IO uint32_t I2SPR;     /* 0x20 */
} spi_t;
#define SPI1                ((spi_t *)SPI1_BASE)
#define SPI2                ((spi_t *)SPI2_BASE)

#define SPI_CR1_CPHA_Msk       (1u << 0)
#define SPI_CR1_CPOL_Msk       (1u << 1)
#define SPI_CR1_MSTR_Msk       (1u << 2)
#define SPI_CR1_BR_Msk         (0x7u << 3)
#define SPI_CR1_SPE_Msk        (1u << 6)
#define SPI_CR1_SSI_Msk        (1u << 8)
#define SPI_CR1_SSM_Msk        (1u << 9)
#define SPI_CR2_DS_8BIT        (0x7u << 8)
#define SPI_CR2_FRXTH_Msk      (1u << 12)
#define SPI_SR_RXNE_Msk        (1u << 0)
#define SPI_SR_TXE_Msk         (1u << 1)
#define SPI_SR_BSY_Msk         (1u << 7)

/* ---------- CRC --------------------------------------------------------- */
typedef struct {
    __IO uint32_t DR;        /* 0x00 */
    __IO uint8_t  IDR;       /* 0x04 */
    uint8_t       _r0[3];
    __IO uint32_t CR;        /* 0x08 */
    uint32_t      _r1;
    __IO uint32_t INIT;      /* 0x10 */
    __IO uint32_t POL;       /* 0x14 */
} crc_t;
#define CRC_HW              ((crc_t *)CRC_BASE)

#define CRC_CR_RESET_Msk       (1u << 0)
#define CRC_CR_REV_IN_Msk      (0x3u << 5)
#define CRC_CR_REV_OUT_Msk     (1u << 7)

/* ---------- FLASH program/erase bits ----------------------------------- */
#define FLASH_KEY1             (0x45670123u)
#define FLASH_KEY2             (0xCDEF89ABu)
#define FLASH_SR_BSY_Msk       (1u << 0)
#define FLASH_SR_PGERR_Msk     (1u << 2)
#define FLASH_SR_WRPRTERR_Msk  (1u << 4)
#define FLASH_SR_EOP_Msk       (1u << 5)
#define FLASH_CR_PG_Msk        (1u << 0)
#define FLASH_CR_PER_Msk       (1u << 1)
#define FLASH_CR_MER_Msk       (1u << 2)
#define FLASH_CR_STRT_Msk      (1u << 6)
#define FLASH_CR_LOCK_Msk      (1u << 7)

/* ---------- FLASH interface (access control) ---------------------------- */
typedef struct {
    __IO uint32_t ACR;       /* 0x00 */
    __IO uint32_t KEYR;      /* 0x04 */
    __IO uint32_t OPTKEYR;   /* 0x08 */
    __IO uint32_t SR;        /* 0x0C */
    __IO uint32_t CR;        /* 0x10 */
    __IO uint32_t AR;        /* 0x14 */
    uint32_t      _r0;
    __IO uint32_t OBR;       /* 0x1C */
    __IO uint32_t WRPR;      /* 0x20 */
} flash_t;
#define FLASH               ((flash_t *)FLASH_R_BASE)

#define FLASH_ACR_LATENCY_Msk  (0x7u << 0)
#define FLASH_ACR_LATENCY_1WS  (0x1u << 0)
#define FLASH_ACR_PRFTBE_Msk   (1u << 4)

/* ---------- IWDG -------------------------------------------------------- */
typedef struct {
    __IO uint32_t KR;        /* 0x00 */
    __IO uint32_t PR;        /* 0x04 */
    __IO uint32_t RLR;       /* 0x08 */
    __I  uint32_t SR;        /* 0x0C */
    __IO uint32_t WINR;      /* 0x10 */
} iwdg_t;
#define IWDG                ((iwdg_t *)IWDG_BASE)

#define IWDG_KEY_RELOAD     (0x0000AAAAUL)
#define IWDG_KEY_ENABLE     (0x0000CCCCUL)
#define IWDG_KEY_WRITE      (0x00005555UL)

/* ---------- SysTick (Cortex-M0) ----------------------------------------- */
typedef struct {
    __IO uint32_t CTRL;      /* 0x00 */
    __IO uint32_t LOAD;      /* 0x04 */
    __IO uint32_t VAL;       /* 0x08 */
    __I  uint32_t CALIB;     /* 0x0C */
} systick_t;
#define SYSTICK             ((systick_t *)SYSTICK_BASE)

/* ---------- NVIC (Cortex-M0) -------------------------------------------- */
typedef struct {
    __IO uint32_t ISER[1];   /* 0x000 */
    uint32_t      _r0[31];
    __IO uint32_t ICER[1];   /* 0x080 */
    uint32_t      _r1[31];
    __IO uint32_t ISPR[1];   /* 0x100 */
    uint32_t      _r2[31];
    __IO uint32_t ICPR[1];   /* 0x180 */
    uint32_t      _r3[31];
    uint32_t      _r4[64];
    __IO uint32_t IPR[8];    /* 0x300 */
} nvic_t;
#define NVIC                ((nvic_t *)NVIC_BASE)

#endif /* MM32F031_H */
