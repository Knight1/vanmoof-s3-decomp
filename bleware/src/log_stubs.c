/* log_stubs.c — log injection/dump stubs.
 *
 * Called by cmd_log_inject and cmd_log_dump (both decoded). The OEM
 * bodies push formatted log blocks through the ICall logger service
 * and ext-flash ring buffer. These stubs are thin wrappers. */

#include <stdint.h>

#include "bleware.h"

uint32_t log_block_count(void)
{
    return log_block_count_get();
}

void log_format_block(uint32_t index, void *out_16B)
{
    (void)index; (void)out_16B;
}

void log_submit(const void *block_16B)
{
    (void)block_16B;
}
