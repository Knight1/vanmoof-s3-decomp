/* adc.c — ADC1 single-channel sample. Used for supply voltage and the
 * analog Hall outputs that report shifter shaft position. */

#include "adc.h"
#include "gpio.h"
#include "mm32f031.h"

void adc_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_ADCEN_Msk;

    /* Calibrate before first enable. ADRDY must be 0, ADEN must be 0. */
    if ((ADC1->CR & ADC_CR_ADEN_Msk) != 0u) {
        ADC1->CR |= ADC_CR_ADDIS_Msk;
        while ((ADC1->CR & ADC_CR_ADEN_Msk) != 0u) {
            /* wait disable */
        }
    }
    ADC1->CR |= ADC_CR_ADCAL_Msk;
    while ((ADC1->CR & ADC_CR_ADCAL_Msk) != 0u) {
        /* wait cal */
    }

    ADC1->CFGR1 = 0u;          /* 12-bit, right-aligned, single conversion */
    ADC1->SMPR  = 0x7u;        /* longest sample time for accuracy */

    ADC1->ISR = ADC_ISR_ADRDY_Msk;
    ADC1->CR |= ADC_CR_ADEN_Msk;
    while ((ADC1->ISR & ADC_ISR_ADRDY_Msk) == 0u) {
        /* wait ready */
    }
    ADC1->ISR = ADC_ISR_ADRDY_Msk;
}

uint16_t adc_read_channel(uint8_t channel)
{
    ADC1->CHSELR = 1u << (channel & 0x1Fu);
    ADC1->ISR    = ADC_ISR_EOC_Msk | ADC_ISR_EOSEQ_Msk;
    ADC1->CR    |= ADC_CR_ADSTART_Msk;
    while ((ADC1->ISR & ADC_ISR_EOC_Msk) == 0u) {
        /* wait sample */
    }
    return (uint16_t)(ADC1->DR & 0xFFFFu);
}
