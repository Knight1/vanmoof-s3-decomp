/* handlers.c — exception / interrupt handlers (the VanMoof-custom ones).
 *
 * Reconstructed from nmi_css_handler (0x080016AC), HardFault_Handler
 * (0x080016FC), svc_trap_handler (0x08001718), PendSV_Handler (0x08001734),
 * SysTick_Handler (0x0800214C) and stl_clock_meas_capture_irq (0x08001158).
 *
 * The four fault handlers all trace an "NG" line and drop into the IEC-60730
 * fail-safe. SysTick paces the download server loop by posting sub-rate event
 * bits. The TIM6_DAC vector carries the STL clock cross-measurement capture.
 */
#include "powerbankboot.h"

/* X-CUBE-STL helpers (vendor-stock). */
extern void     stl_clock_recover(void);                     /* FUN_08000A80     */
extern uint16_t stl_tick_phase(uint32_t subdiv, uint32_t period); /* FUN_08000214 */

/* SysTick scratch (OEM SRAM). g_tick_valid_* is the armed value+complement pair
 * set up by comms_rx_state_init(); events are only posted while it is intact. */
extern uint32_t g_comms_a, g_comms_d;        /* validity pair (a ^ d == ~0)      */
static uint32_t s_systick_ms;
static uint16_t s_subdiv;

#define SUBDIV_ROLLOVER  1000u

/* ---- fault traps ---- */
void NMI_Handler(void)            /* clock-security (CSS) trap */
{
    if ((REG32(RCC_BASE + 0x08) & 0x80u) == 0x80u) {   /* RCC_CIR.CSSF set        */
        while ((REG32(RCC_BASE + 0x00) & 0x02u) != 0x02u)
            ;                                            /* wait for HSI ready      */
        stl_clock_recover();
        dbg_printf(STR_NMI_NG1);
    } else {
        dbg_printf(STR_NMI_NG2);
    }
    stl_failsafe();
}

void HardFault_Handler(void)
{
    dbg_printf(STR_HARDFAULT_NG);
    stl_failsafe();
}

void SVC_Handler(void)
{
    dbg_printf(STR_SVC_NG);
    stl_failsafe();
}

void PendSV_Handler(void)
{
    dbg_printf(STR_SVC_NG);          /* shared trap message */
    stl_failsafe();
}

/* ---- SysTick: pace the download server loop ----
 * Every tick posts bit0; every ~50 ticks bit1; every ~250 ticks bit2; at the
 * sub-divider rollover all four bits. boot_main consumes these to run its
 * keepalive, watchdog kick and finalise-mirror steps. */
void SysTick_Handler(void)
{
    s_systick_ms++;

    if ((g_comms_a ^ g_comms_d) != 0xFFFFFFFFu)   /* armed-state guard            */
        return;

    if (++s_subdiv > SUBDIV_ROLLOVER) {
        s_subdiv = 0;
        g_boot_events |= 0x0Fu;
    } else if ((stl_tick_phase(s_subdiv, 0xFA) & 0xFFFFu) == 0) {
        g_boot_events |= 0x07u;                    /* ~250-tick boundary           */
    } else if ((stl_tick_phase(s_subdiv, 0x32) & 0xFFFFu) == 0) {
        g_boot_events |= 0x03u;                    /* ~50-tick boundary            */
    } else {
        g_boot_events |= 0x01u;                    /* every tick                   */
    }
}

/* ---- TIM6_DAC: STL clock cross-measurement input capture ----
 * The Class-B clock test captures an independent reference edge and stores the
 * measured period as a value+complement pair for the clock-test routine to read
 * ("Xmeas" / "LSE = %ld" / "HSE = %ld"). This is STL-support glue. */
extern uint32_t g_meas_tim_base;     /* capture timer instance                   */
extern uint16_t g_meas_prev, g_meas_curr;
extern uint32_t g_meas_period, g_meas_period_inv;
extern uint8_t  g_meas_ready;

void TIM6_DAC_IRQHandler(void)
{
    if ((REG32(g_meas_tim_base + 0x10) & 0x02u) == 0)   /* TIM_SR capture flag    */
        return;

    g_meas_prev = g_meas_curr;
    g_meas_curr = (uint16_t)REG32(g_meas_tim_base + 0x34);   /* TIM_CCR1           */

    if (g_meas_ready == 0) {
        if ((REG32(g_meas_tim_base + 0x10) & 0x200u) == 0) { /* no over-capture    */
            g_meas_period     = (uint32_t)(uint16_t)(g_meas_curr - g_meas_prev);
            g_meas_period_inv = ~g_meas_period;
            g_meas_ready      = 1;
        } else {
            REG32(g_meas_tim_base + 0x10) &= ~0x202u;        /* clear CC/OC flags  */
        }
    }
}
