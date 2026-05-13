#ifndef SHIFTER_FAULT_H
#define SHIFTER_FAULT_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    FAULT_NONE              = 0u,
    FAULT_HOMING_TIMEOUT    = (1u << 0),
    FAULT_MOTOR_STALL       = (1u << 1),
    FAULT_HALL_OUT_OF_RANGE = (1u << 2),
    FAULT_SUPPLY_UNDERVOLT  = (1u << 3),
    FAULT_SUPPLY_OVERVOLT   = (1u << 4),
    FAULT_NVM_CORRUPT       = (1u << 5),
    FAULT_PROTO_FRAMING     = (1u << 6),
    FAULT_WATCHDOG_RECOVERY = (1u << 7),
} fault_bit_t;

void     fault_init(void);
void     fault_set(uint32_t bits);
void     fault_clear(uint32_t bits);
uint32_t fault_active(void);
bool     fault_any(void);
uint32_t fault_take_pending(void);   /* returns + clears the latched set */

#endif /* SHIFTER_FAULT_H */
