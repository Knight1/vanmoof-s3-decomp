#include "powerbankware.h"

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
    volatile int wait = (int)(*(volatile uint32_t *)0x200000C0u / 1000000u);
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

extern int FUN_08019344(void *hadc);                /* HAL_ADC_Init           */
extern int FUN_080199c8(void *hadc);                /* HAL ADC common config  */
extern int FUN_080196b4(void *hadc, void *sConfig); /* HAL_ADC_ConfigChannel  */

/*
 * adc_msp_init — OEM FUN_08008804. One of board_init's peripheral sub-inits.
 * Bring up ADC1 (PA0/PA1/PA4 analog), 12-bit single-ended 3-rank scan, and the
 * ADC IRQ (12 = ADC1_COMP_IRQn). HAL handle @ 0x200001b4. Handle field values
 * and the channel-config struct (sampling 0x1000, rank 7) disasm-confirmed.
 */
void adc_msp_init(void)
{
    volatile uint32_t * const RCC = (volatile uint32_t *)0x40021000u;
    uint32_t * const hadc = (uint32_t *)0x200001b4u;

    gpio_pin_cfg_t gcfg;
    uint32_t       sconf[3];
    mem_set(sconf, 0, sizeof sconf);
    mem_set(&gcfg, 0, sizeof gcfg);

    RCC[6] |= 0x200u;     (void)(RCC[6] & 0x200u);     /* APB2ENR (+0x18) ADCEN  */
    RCC[5] |= 0x20000u;   (void)(RCC[5] & 0x20000u);   /* AHBENR  (+0x14) IOPAEN */

    gcfg.pin_mask = 0x13;            /* PA0, PA1, PA4 */
    gcfg.mode     = GPIO_MODE_ANALOG;
    gcfg.pupd     = 0;
    gpio_pin_config((uint32_t *)0x48000000u, &gcfg);

    hadc[0]  = 0x40012400u;   /* Instance = ADC1     */
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

    if (FUN_08019344(hadc) != 0) { spi_error_reset(); }
    if (FUN_080199c8(hadc) != 0) { spi_error_reset(); }

    sconf[0] = 0;            /* Channel 0 */
    sconf[1] = 0x1000;      /* SamplingTime */
    sconf[2] = 7;           /* Rank */
    if (FUN_080196b4(hadc, sconf) != 0) { spi_error_reset(); }
    sconf[0] = 1;           /* Channel 1 */
    if (FUN_080196b4(hadc, sconf) != 0) { spi_error_reset(); }
    sconf[0] = 4;           /* Channel 4 */
    if (FUN_080196b4(hadc, sconf) != 0) { spi_error_reset(); }

    nvic_set_priority(12, 2, 0);   /* ADC1_COMP_IRQn */
    nvic_enable_irq(12);
}
