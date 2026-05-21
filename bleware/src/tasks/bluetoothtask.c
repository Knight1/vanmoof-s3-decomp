/* tasks/bluetoothtask.c — bleware Bluetooth task body.
 *
 * Path confirmed by embedded string `Fsource/tasks/bluetoothtask.c`
 * at flash 0x00006A23 in bleware 1.4.01. The task entry is at
 * 0x000067C8.
 *
 * The body is the BLE event loop:
 *   1. Init: spawn service objects, post several log lines.
 *   2. Event_pend(handle, 0, 0xC0000000, 0xFFFFFFFF) — wait for any
 *      of the two reserved BLE-stack event bits.
 *   3. For each pended event:
 *      - If a stack-msg event: fetch the message via ICall and
 *        dispatch on the leading byte:
 *          0x91: sub-codes 0x0E/0x0F/0x10/0x3E (hardware error etc.)
 *          0xB0: sub-codes 0x7F/0x1E/0x7E
 *          0xD0: sub-codes 0x00/0x05/0x06/0x07/0x11
 *      - If bit 30 set: drain the user-msg queue, dispatch on first
 *        byte (0x02 connection, 0x03 small BLE, 0x04 force-discon,
 *        0x32 misc).
 *   4. Free the message buffer.
 *
 * Skeleton: stub the body as an infinite no-op loop so the link
 * resolves. The full event dispatch + handlers land as we decode
 * each sub-handler.
 */

#include "bleware.h"

#include <stdint.h>

void bluetoothtask_main(void)
{
    /* Skeleton — replace with the OEM event loop. */
    for (;;) {
        /* trap until properly hooked up */
    }
}
