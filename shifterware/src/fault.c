/* fault.c — sticky-latched fault flag set. */

#include "fault.h"

static volatile uint32_t s_active;
static volatile uint32_t s_pending;

void fault_init(void)
{
    s_active  = 0u;
    s_pending = 0u;
}

void fault_set(uint32_t bits)
{
    s_active  |= bits;
    s_pending |= bits;
}

void fault_clear(uint32_t bits)
{
    s_active &= ~bits;
}

uint32_t fault_active(void)
{
    return s_active;
}

bool fault_any(void)
{
    return s_active != 0u;
}

uint32_t fault_take_pending(void)
{
    const uint32_t p = s_pending;
    s_pending = 0u;
    return p;
}
