/* exti.c — pin-change interrupts via EXTI + SYSCFG. */

#include "exti.h"
#include "mm32f031.h"

#define EXTI_LINE_COUNT  16u

static exti_cb_t s_cb[EXTI_LINE_COUNT];

void exti_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN_Msk;
    for (uint8_t i = 0u; i < EXTI_LINE_COUNT; i++) {
        s_cb[i] = (exti_cb_t)0;
    }
    NVIC->ISER[0] = (1u << 5) | (1u << 6) | (1u << 7);  /* EXTI 0-1, 2-3, 4-15 */
}

void exti_configure_line(uint8_t line, exti_port_t port, exti_edge_t edge)
{
    if (line >= EXTI_LINE_COUNT) return;

    const uint8_t  idx   = (uint8_t)(line >> 2);
    const uint32_t shift = (uint32_t)(line & 0x3u) * 4u;
    uint32_t v = SYSCFG->EXTICR[idx];
    v &= ~(0xFu << shift);
    v |= ((uint32_t)port & 0xFu) << shift;
    SYSCFG->EXTICR[idx] = v;

    const uint32_t bit = 1u << line;
    EXTI->RTSR = (edge & EXTI_EDGE_RISING)  ? (EXTI->RTSR | bit) : (EXTI->RTSR & ~bit);
    EXTI->FTSR = (edge & EXTI_EDGE_FALLING) ? (EXTI->FTSR | bit) : (EXTI->FTSR & ~bit);
}

void exti_enable_line(uint8_t line)
{
    if (line >= EXTI_LINE_COUNT) return;
    EXTI->IMR |= 1u << line;
}

void exti_disable_line(uint8_t line)
{
    if (line >= EXTI_LINE_COUNT) return;
    EXTI->IMR &= ~(1u << line);
}

void exti_set_callback(uint8_t line, exti_cb_t cb)
{
    if (line >= EXTI_LINE_COUNT) return;
    s_cb[line] = cb;
}

bool exti_line_pending(uint8_t line)
{
    if (line >= EXTI_LINE_COUNT) return false;
    return (EXTI->PR & (1u << line)) != 0u;
}

void exti_clear_line(uint8_t line)
{
    if (line >= EXTI_LINE_COUNT) return;
    EXTI->PR = 1u << line;     /* write 1 to clear */
}

static void dispatch(uint8_t lo, uint8_t hi)
{
    for (uint8_t i = lo; i <= hi; i++) {
        if ((EXTI->PR & (1u << i)) != 0u) {
            EXTI->PR = 1u << i;
            if (s_cb[i] != (exti_cb_t)0) {
                s_cb[i](i);
            }
        }
    }
}

void EXTI0_1_IRQHandler(void)   { dispatch(0u, 1u); }
void EXTI2_3_IRQHandler(void)   { dispatch(2u, 3u); }
void EXTI4_15_IRQHandler(void)  { dispatch(4u, 15u); }
