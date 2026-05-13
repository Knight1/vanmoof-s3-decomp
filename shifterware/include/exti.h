#ifndef SHIFTER_EXTI_H
#define SHIFTER_EXTI_H

#include <stdint.h>
#include <stdbool.h>
#include "mm32f031.h"

typedef enum {
    EXTI_PORT_A = 0u,
    EXTI_PORT_B = 1u,
    EXTI_PORT_C = 2u,
    EXTI_PORT_D = 3u,
    EXTI_PORT_F = 5u,
} exti_port_t;

typedef enum {
    EXTI_EDGE_RISING  = 1u,
    EXTI_EDGE_FALLING = 2u,
    EXTI_EDGE_BOTH    = 3u,
} exti_edge_t;

typedef void (*exti_cb_t)(uint8_t line);

void exti_init(void);
void exti_configure_line(uint8_t line, exti_port_t port, exti_edge_t edge);
void exti_enable_line(uint8_t line);
void exti_disable_line(uint8_t line);
void exti_set_callback(uint8_t line, exti_cb_t cb);

bool exti_line_pending(uint8_t line);
void exti_clear_line(uint8_t line);

void EXTI0_1_IRQHandler(void);
void EXTI2_3_IRQHandler(void);
void EXTI4_15_IRQHandler(void);

#endif /* SHIFTER_EXTI_H */
