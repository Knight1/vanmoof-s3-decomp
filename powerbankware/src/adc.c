#include "powerbankware.h"

/* ── Peripheral bases ────────────────────────────────────────────────── */
#define RCC_BASE        0x40021000u
#define GPIOA_BASE      0x48000000u
#define ADC1_BASE       0x40012400u   /* measurement ADC instance        */
#define ADC12_CCR       (*(volatile uint32_t *)0x40012708u)  /* ADC common CCR */

/* ── SRAM globals (see docs/hardware.md) ─────────────────────────────── */
#define SYSTEM_CORE_CLOCK (*(volatile uint32_t *)0x200000c0u) /* OEM _sdata[0] */
#define ADC_HAL_HANDLE    ((uint32_t *)0x200001b4u)           /* HAL ADC handle */
#define ADC_RAW_BUF       0x20000208u                         /* raw sample buffer base */
#define ADC_RAW_IDX       (*(volatile uint8_t *)0x20000213u)  /* raw-buffer write index */
#define ADC_SAMPLE_READY  (*(volatile uint8_t *)0x20000204u)  /* "sample ready" flag */

/*
 * adc_start_it — OEM FUN_080195D0 (HAL_ADC_Start_IT).
 *
 * Kick the measurement ADC in interrupt mode. `handle` is the HAL ADC handle:
 *   +0x00  Instance  (peripheral base: ISR @+0, IER @+4, CR @+8)
 *   +0x14  Init.ContinuousConvMode-ish selector (8 = DMA-continuous variant)
 *   +0x1c  NbrOfConversion field (skip the enable prep when already 1)
 *   +0x40  Lock byte    +0x44  State    +0x48  ErrorCode
 *
 * Returns HAL status (0 = OK, 2 = BUSY/locked). If the conversion is already
 * running (CR bit2, ADSTART) or the handle is locked it bails with BUSY;
 * otherwise it runs the enable prep, marks the handle BUSY_REG (State bit8),
 * clears the ISR flags (write 0x1C), enables the EOC/EOS/OVR interrupts (IER),
 * and starts the conversion (CR bit2). Byte/word widths disasm-confirmed against
 * the OEM image.
 */

char adc_start_it(uint32_t *handle)
{
    volatile uint32_t *inst = (volatile uint32_t *)handle[0];
    volatile uint8_t  *lock = (volatile uint8_t *)((uint8_t *)handle + 0x40);
    char rc = 0;

    if ((inst[2] & 4) != 0) {              /* CR.ADSTART already set */
        return 2;
    }
    if (*lock == 1) {
        return 2;
    }
    *lock = 1;

    if (handle[7] != 1) {                  /* NbrOfConversion != 1 -> run enable prep */
        rc = adc_enable(handle);
    }
    if (rc == 0) {
        handle[0x11] = (handle[0x11] & 0xfffff0feu) | 0x100;   /* State: clear, set BUSY_REG */
        handle[0x12] = 0;                                       /* ErrorCode = NONE */
        *lock = 0;
        inst[0] = 0x1c;                    /* ISR: clear EOC|EOS|OVR */
        if (handle[5] == 8) {
            inst[1] &= 0xfffffffbu;        /* IER: clear EOSMPIE-ish bit2 */
            inst[1] |= 0x18u;              /* IER: enable EOS|OVR */
        } else {
            inst[1] |= 0x1cu;              /* IER: enable EOC|EOS|OVR */
        }
        inst[2] |= 4u;                     /* CR: ADSTART */
    }
    return rc;
}

/*
 * adc_enable — OEM FUN_080198D0 (HAL ADC_Enable).
 *
 * Bring the ADC out of disable and wait for it to report ready, with the same
 * register layout adc_start_it uses (inst = handle[0] = Instance):
 *   inst[0]  ISR   (bit0 ADRDY)
 *   inst[2]  CR    (bit0 ADEN, bit1 ADDIS, bit2 ADSTART, bit4 ADSTP, bit31 ADCAL)
 *   inst[3]  CFGR1 (bit15 AUTOFF)
 *   handle[0x11] State    handle[0x12] ErrorCode
 *
 * If the ADC is already enabled and either ready or in auto-off mode, it returns
 * OK immediately. Otherwise it enables (ADEN) only when no calibrate/stop/convert/
 * disable bit is set, burns a SystemCoreClock/1000000 stabilization delay, then
 * polls ADRDY with a 2-tick timeout. A disallowed CR state, or a timeout, sets
 * State bit4 / ErrorCode bit0 and returns ERROR. Widths disasm-confirmed against
 * the OEM image.
 */
char adc_enable(uint32_t *handle)
{
    volatile uint32_t *inst = (volatile uint32_t *)handle[0];

    if ((inst[2] & 3) == 1 &&
        ((inst[0] & 1) == 1 || (inst[3] & 0x8000) == 0x8000)) {
        return 0;                          /* already enabled and ready/auto-off */
    }

    if ((inst[2] & 0x80000017u) != 0) {    /* enabling conditions not met */
        handle[0x11] |= 0x10;              /* State: ERROR_INTERNAL */
        handle[0x12] |= 1;                 /* ErrorCode: INTERNAL */
        return 1;
    }

    inst[2] |= 1u;                         /* CR: ADEN */

    /* Stabilization delay = SystemCoreClock/1000000 (0x200000C0 = OEM _sdata[0]).
     * volatile so -Os keeps the busy-wait, matching the OEM __IO loop counter. */
    volatile int wait = (int)(SYSTEM_CORE_CLOCK / 1000000u);
    while (wait != 0) {
        wait--;
    }

    uint32_t start = tick_get();
    while ((inst[0] & 1) != 1) {           /* wait for ADRDY */
        if ((uint32_t)(tick_get() - start) >= 3) {
            handle[0x11] |= 0x10;
            handle[0x12] |= 1;
            return 1;                      /* timeout */
        }
    }
    return 0;
}

/* hal_adc_msp_init — OEM FUN_080195c0 (HAL_ADC_MspInit): empty weak callback
 * (ADC clock + GPIO are set up directly in adc_msp_init). */
void hal_adc_msp_init(void *hadc)
{
    (void)hadc;
}

/*
 * hal_adc_init — OEM FUN_08019344 (HAL_ADC_Init). Validate the handle, run the
 * (empty) MSP init on first use, then compose ADC CFGR1/CFGR2/SMPR from the Init
 * fields. State word[0x11], ErrorCode word[0x12], Lock byte +0x40. ADC regs:
 * ISR inst[0], CR inst[2], CFGR1 inst[3], CFGR2 inst[4], SMPR inst[5].
 * Constants/masks/widths disasm-confirmed.
 */
int hal_adc_init(uint32_t *hadc)
{
    if (hadc == NULL) {
        return 1;
    }
    if (hadc[0x11] == 0) {                          /* State == RESET */
        hadc[0x12] = 0;                             /* ErrorCode = NONE */
        *(volatile uint8_t *)((uint8_t *)hadc + 0x40) = 0;   /* Lock = UNLOCKED */
        hal_adc_msp_init(hadc);                     /* HAL_ADC_MspInit */
    }

    volatile uint32_t *inst = (volatile uint32_t *)hadc[0];
    if ((hadc[0x11] & 0x10) != 0 || (inst[2] & 4) != 0) {    /* busy or ADSTART */
        hadc[0x11] |= 0x10;
        return 1;
    }
    hadc[0x11] = (hadc[0x11] & 0xfffffefdu) | 0x10;          /* State = BUSY_INTERNAL */

    bool skip_cfg = ((inst[2] & 3) == 1) &&
                    (((inst[2] & 1) == 1) || ((inst[3] & 0x8000) == 0x8000));
    if (!skip_cfg) {
        inst[3] = hadc[2] | (inst[3] & 0xffffffe7u);         /* CFGR1: Resolution */
        inst[4] = hadc[1] | (inst[4] & 0x3fffffffu);         /* CFGR2: ClockPrescaler */
    }
    inst[3] &= 0xfffe0219u;

    uint32_t res_bit   = (hadc[0xd] == 1) ? 0u : 0x1000u;    /* Overrun */
    uint32_t align_bit = (hadc[4] == 2) ? 4u : 0u;
    uint32_t cfgr1 = (hadc[0xc] << 1) | (hadc[6] << 0xe) | (hadc[7] << 0xf) |
                     (hadc[8] << 0xd) | res_bit | hadc[3] | align_bit;
    if (hadc[9] == 1) {                                      /* ExternalTrigConvEdge */
        if (hadc[8] == 0) {
            cfgr1 |= 0x10000u;
        } else {
            hadc[0x11] |= 0x20;
            hadc[0x12] |= 1;
        }
    }
    if (hadc[10] != 0x1c1) {                                 /* ExternalTrigConv set */
        cfgr1 |= hadc[0xb] | hadc[10];
    }
    inst[3] |= cfgr1;

    uint32_t smp = hadc[0xe];
    if (smp == 0x10000000u || smp <= 7u) {                  /* SamplingTime in 0..7 */
        inst[5] = (inst[5] & 0xfffffff8u) | (smp & 7u);     /* SMPR */
    }

    if (cfgr1 == (inst[3] & 0x833fffe7u)) {
        hadc[0x12] = 0;
        hadc[0x11] = (hadc[0x11] & 0xfffffffcu) | 2;        /* State = READY */
        return 0;
    }
    hadc[0x11] = (hadc[0x11] & 0xffffffedu) | 0x10;
    hadc[0x12] |= 1;
    return 1;
}

/*
 * adc_calibration_start — OEM FUN_080199c8 (HAL_ADCEx_Calibration_Start). Runs
 * ADC1 self-calibration (F0 requires ADCAL while the ADC is disabled): save &
 * clear CFGR1.DMAEN/DMACFG, set CR.ADCAL (bit31), poll until it self-clears (3-
 * tick timeout), restore DMA bits, update State. Returns 0/1/2. Disasm-confirmed.
 */
char adc_calibration_start(uint32_t *handle)
{
    volatile uint32_t *inst = (volatile uint32_t *)handle[0];
    volatile uint8_t  *lock = (volatile uint8_t *)((uint8_t *)handle + 0x40);

    if (*lock == 1) {
        return 2;
    }
    *lock = 1;

    if ((inst[2] & 3) == 1 && ((inst[0] & 1) == 1 || (inst[3] & 0x8000) == 0x8000)) {
        handle[0x11] |= 0x20;                       /* already running */
        *lock = 0;
        return 1;
    }

    handle[0x11] = (handle[0x11] & 0xfffffefdu) | 2;   /* State = BUSY_INTERNAL */
    uint32_t dma_bits = inst[3] & 3;                /* save CFGR1 DMAEN|DMACFG */
    inst[3] &= 0xfffffffcu;                         /* DMA off for calibration */
    inst[2] |= 0x80000000u;                         /* CR.ADCAL: start */

    uint32_t start = tick_get();
    for (;;) {
        if ((int32_t)inst[2] >= 0) {                /* ADCAL self-cleared -> done */
            inst[3] |= (dma_bits & 3);              /* restore DMA bits */
            handle[0x11] = (handle[0x11] & 0xfffffffcu) | 1;   /* State = READY */
            *lock = 0;
            return 0;
        }
        if ((uint32_t)(tick_get() - start) >= 3) {  /* timeout */
            handle[0x11] = (handle[0x11] & 0xffffffedu) | 0x10;
            *lock = 0;
            return 1;
        }
    }
}

/*
 * adc_config_channel — OEM FUN_080196b4 (HAL_ADC_ConfigChannel). sConfig =
 * {Channel, Rank, SamplingTime}: Rank == 0x1001 (NONE) removes the channel
 * (clears CHSELR + any internal-channel CCR enable); otherwise it adds it (sets
 * CHSELR, programs the shared SMPR, enables the TSEN/VREFEN/VBATEN CCR bit for
 * channels 16/17/18, with the temp-sensor settling delay for channel 16).
 * Cannot reconfigure while ADSTART is set. Returns 0/1/2. CHSELR inst[10] (+0x28),
 * SMPR inst[5] (+0x14), CR inst[2] (+0x08), common CCR @0x40012708. Disasm-confirmed.
 */
char adc_config_channel(uint32_t *handle, uint32_t *sconfig)
{
    volatile uint32_t *inst = (volatile uint32_t *)handle[0];
    volatile uint8_t  *lock = (volatile uint8_t *)((uint8_t *)handle + 0x40);
    volatile uint32_t *ccr  = &ADC12_CCR;                         /* ADC common CCR */
    char rc = 0;

    if (*lock == 1) {
        return 2;
    }
    *lock = 1;

    if ((inst[2] & 4) != 0) {                       /* CR.ADSTART: conversion running */
        handle[0x11] |= 0x20;
        rc = 1;
    } else if (sconfig[1] == 0x1001u) {             /* Rank == NONE: remove channel */
        inst[10] &= ~(1u << (sconfig[0] & 0xffu));  /* CHSELR clear bit */
        if (sconfig[0] == 16 || sconfig[0] == 17 || sconfig[0] == 18) {
            uint32_t mask = (sconfig[0] == 16) ? 0xff7fffffu
                          : (sconfig[0] == 17) ? 0xffbfffffu : 0xfeffffffu;
            *ccr &= mask;
        }
    } else {                                        /* add channel */
        inst[10] |= (1u << (sconfig[0] & 0xffu));    /* CHSELR set bit */

        uint32_t trig = handle[0xe];
        if (trig != 0x10000000u && trig != 1 && trig != 2 && trig != 3 && trig != 4 &&
            trig != 5 && trig != 6 && trig != 7 && sconfig[2] != (inst[5] & 7u)) {
            inst[5] = (inst[5] & 0xfffffff8u) | (sconfig[2] & 7u);   /* SMPR */
        }

        if (sconfig[0] == 16 || sconfig[0] == 17 || sconfig[0] == 18) {
            uint32_t bit = (sconfig[0] == 16) ? 0x800000u
                         : (sconfig[0] == 17) ? 0x400000u : 0x1000000u;
            *ccr |= bit;
            if (sconfig[0] == 16) {                  /* temp-sensor startup delay */
                volatile int wait = (int)(SYSTEM_CORE_CLOCK / 1000000u) * 10;
                while (wait != 0) {
                    wait--;
                }
            }
        }
    }

    *lock = 0;
    return rc;
}

/*
 * adc_msp_init — OEM FUN_08008804. One of board_init's peripheral sub-inits.
 * Bring up ADC1 (PA0/PA1/PA4 analog), 12-bit single-ended 3-rank scan, and the
 * ADC IRQ (12 = ADC1_COMP_IRQn). HAL handle @ 0x200001b4. Handle field values
 * and the channel-config struct (sampling 0x1000, rank 7) disasm-confirmed.
 */
void adc_msp_init(void)
{
    volatile uint32_t * const RCC = (volatile uint32_t *)RCC_BASE;
    uint32_t * const hadc = ADC_HAL_HANDLE;

    gpio_pin_cfg_t gcfg;
    uint32_t       sconf[3];
    mem_set(sconf, 0, sizeof sconf);
    mem_set(&gcfg, 0, sizeof gcfg);

    RCC[6] |= 0x200u;     (void)(RCC[6] & 0x200u);     /* APB2ENR (+0x18) ADCEN  */
    RCC[5] |= 0x20000u;   (void)(RCC[5] & 0x20000u);   /* AHBENR  (+0x14) IOPAEN */

    gcfg.pin_mask = 0x13;            /* PA0, PA1, PA4 */
    gcfg.mode     = GPIO_MODE_ANALOG;
    gcfg.pupd     = 0;
    gpio_pin_config((uint32_t *)GPIOA_BASE, &gcfg);

    hadc[0]  = ADC1_BASE;     /* Instance = ADC1     */
    hadc[1]  = 0x80000000u;   /* Init.ClockPrescaler */
    hadc[2]  = 0;             /* Resolution 12-bit   */
    hadc[3]  = 0;             /* DataAlign           */
    hadc[4]  = 1;             /* ScanConvMode        */
    hadc[5]  = 4;             /* EOCSelection        */
    hadc[6]  = 0;
    hadc[7]  = 0;
    hadc[8]  = 0;
    hadc[9]  = 0;
    hadc[10] = 0x1c1;
    hadc[11] = 0;
    hadc[12] = 0;
    hadc[13] = 1;

    if (hal_adc_init(hadc) != 0)         { spi_error_reset(); }
    if (adc_calibration_start(hadc) != 0) { spi_error_reset(); }

    sconf[0] = 0;           /* Channel 0 */
    sconf[1] = 0x1000;      /* Rank = ADC_RANK_CHANNEL_NUMBER (add) */
    sconf[2] = 7;           /* SamplingTime */
    if (adc_config_channel(hadc, sconf) != 0) { spi_error_reset(); }
    sconf[0] = 1;           /* Channel 1 */
    if (adc_config_channel(hadc, sconf) != 0) { spi_error_reset(); }
    sconf[0] = 4;           /* Channel 4 */
    if (adc_config_channel(hadc, sconf) != 0) { spi_error_reset(); }

    nvic_set_priority(12, 2, 0);   /* ADC1_COMP_IRQn */
    nvic_enable_irq(12);
}

/*
 * ADC1_COMP_IRQHandler — OEM @0x08008688 (HAL_ADC_IRQHandler for ADC1).
 *
 * 360 bytes of code (0x08008688..0x080087ee) plus a 5-word literal pool to
 * 0x08008804. handle = measurement ADC handle @0x200001B4 (DAT_080087f0);
 * inst = handle[0] = ADC1 @0x40012400. Register/field indices match adc.c:
 *   ISR inst[0] (+0x00), IER inst[1] (+0x04), CR inst[2] (+0x08),
 *   CFGR1 inst[3] (+0x0c), DR inst[0x10] (+0x40); handle State word[0x11]
 *   (+0x44), ErrorCode word[0x12] (+0x48), Init.ContinuousConvMode word[8]
 *   (+0x20), Init.Overrun word[0xd] (+0x34).
 *
 * Literal pool (LE words read from flash):
 *   DAT_080087f0 -> 0x200001b4  (ADC HAL handle)
 *   DAT_080087f4 -> 0xfffffefe  (State AND-mask: clear bits 8 and 0 before READY)
 *   DAT_080087f8 -> 0x20000213  (raw-buffer halfword write index, byte)
 *   DAT_080087fc -> 0x20000208  (raw ADC halfword buffer base)
 *   DAT_08008800 -> 0x20000204  ("sample ready" flag byte)
 *
 * On EOC it latches the 12-bit DR sample into the raw buffer indexed by the
 * byte counter; on EOS it raises the sample-ready flag (the inlined
 * HAL_ADC_ConvCpltCallback) and stops a single-shot scan back to READY.
 * The overrun branch runs whether or not the EOC/EOS gate fired.
 *
 * WHY the magic literals: 0xfffffefe clears State bits 8 and 0 before setting
 * READY; the DR mask 0x0fff is the 12-bit resolution. The post-store re-read of
 * the index byte mirrors the OEM (ldrb at 0x8008776 after the strh).
 */
void ADC1_COMP_IRQHandler(void)
{
    uint32_t * const handle = ADC_HAL_HANDLE;                  /* DAT_080087f0 */
    volatile uint32_t *inst = (volatile uint32_t *)handle[0];

    /* Act only on a flag whose interrupt is enabled (EOC&EOCIE || EOS&EOSIE). */
    if (((inst[0] & 4) == 4 && (inst[1] & 4) == 4) ||
        ((inst[0] & 8) == 8 && (inst[1] & 8) == 8)) {

        if ((handle[0x11] & 0x10) == 0) {
            handle[0x11] |= 0x200u;                            /* State: REG_BUSY */
        }

        /* Single-shot scan: on EOS, stop the conversion and return to READY. */
        if ((inst[3] & 0xc00u) == 0 &&                         /* CFGR1.EXTEN == 0 */
            handle[8] == 0 &&                                  /* not continuous   */
            (inst[0] & 8) == 8) {                              /* ISR.EOS          */
            if ((inst[2] & 4) == 0) {                          /* CR.ADSTART clear */
                inst[1] &= ~0xcu;                              /* IER: clear EOCIE|EOSIE */
                handle[0x11] = (handle[0x11] & 0xfffffefeu) | 1u;   /* State: READY */
            } else {
                handle[0x11] |= 0x20u;                         /* State: ERROR_INTERNAL */
                handle[0x12] |= 1u;                            /* ErrorCode: INTERNAL   */
            }
        }

        /* EOC: latch the 12-bit DR sample into the raw buffer. */
        if ((inst[0] & 4) == 4) {
            uint16_t val = (uint16_t)(inst[0x10] & 0x0fffu);   /* DR, 12-bit */
            uint8_t  idx = ADC_RAW_IDX;                        /* DAT_080087f8 */
            *(volatile uint16_t *)(ADC_RAW_BUF + (uint32_t)idx * 2u) = val; /* DAT_080087fc */
            /* OEM re-reads the index byte after the store (ldrb @0x8008776). */
            idx = ADC_RAW_IDX;
            ADC_RAW_IDX = (uint8_t)(idx + 1u);
        }

        /* EOS: HAL_ADC_ConvCpltCallback — raise the "sample ready" flag. */
        if ((inst[0] & 8) == 8) {
            ADC_SAMPLE_READY |= 1u;                            /* DAT_08008800 */
        }

        inst[0] = 0xcu;                                        /* ISR: W1C EOC|EOS */
    }

    /* Overrun, only when its interrupt is enabled (reached regardless of above). */
    if ((inst[0] & 0x10u) == 0x10u && (inst[1] & 0x10u) == 0x10u) {
        if (handle[0xd] == 1 || (inst[3] & 1u) != 0) {         /* DATA_PRESERVED or DMAEN */
            handle[0x12] |= 2u;                                /* ErrorCode: OVR */
        }
        inst[0] = 0x10u;                                       /* ISR: W1C OVR */
    }
}
