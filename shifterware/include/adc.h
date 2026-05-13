#ifndef SHIFTER_ADC_H
#define SHIFTER_ADC_H

#include <stdint.h>

#define ADC_CH_HALL_A      0u   /* PA0 */
#define ADC_CH_HALL_B      1u   /* PA1 */
#define ADC_CH_VSENSE      2u   /* PA2 — supply divider */
#define ADC_CH_TEMP_INT   16u
#define ADC_CH_VREFINT    17u

void     adc_init(void);
uint16_t adc_read_channel(uint8_t channel);

#endif /* SHIFTER_ADC_H */
