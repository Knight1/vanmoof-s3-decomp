/* state.c — shared bootloader SRAM globals.
 *
 * These are the cross-module RAM variables the loader keeps in SRAM. OEM
 * addresses are recorded in docs/hardware.md; here they are ordinary globals
 * (the decomp is behaviour-equivalent, not yet byte-placed).
 */
#include "powerbankboot.h"

/* comms framing validity pair + counters (comms_rx_state_init / SysTick guard).
 * g_comms_a ^ g_comms_d == 0xFFFFFFFF marks the tick scheduler as armed. */
uint32_t g_comms_a, g_comms_b, g_comms_c, g_comms_d = 0xFFFFFFFFu;

/* address the 0x21 "verify" command must match to finalise a transfer. */
uint32_t g_ota_end_addr;

/* STL clock cross-measurement capture state (TIM6_DAC ISR). */
uint32_t g_meas_tim_base;
uint16_t g_meas_prev, g_meas_curr;
uint32_t g_meas_period, g_meas_period_inv;
uint8_t  g_meas_ready;

/* HAL handle objects — opaque to this decomp, sized to the STM32F0 HAL structs:
 * RTC_HandleTypeDef (~0x20 B), UART_HandleTypeDef (~0x6C B). The CRC handle is
 * referenced only by pointer. */
uint8_t  g_hrtc_obj[0x20];
uint8_t  g_huart1[0x6C];
void    *g_hcrc;
