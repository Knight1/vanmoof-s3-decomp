#include <stdint.h>

#include "uart.h"
#include "util.h"

/* UART/serial TX primitive (OEM uart_send_byte, 0x080364F0).
 *
 * The device handle is reached through a pointer-to-pointer at 0x20009864
 * (*g_uart_dev_pp -> the peripheral register block). The control register at
 * dev+0xC has its TX-interrupt-enable in bit 7; the OEM masks it (with a
 * DSB/ISB pair so the disable lands) before touching the TX ring buffer, then
 * re-enables it afterwards.
 *
 * ABI quirk preserved: the OEM never recomputes a return value — it leaves the
 * ring-push status that ringbuf_push_byte left in r0 untouched through the
 * trailing register re-enable, so the function implicitly returns that status.
 * slip_send_frame relies on this (it maps a 0 here to a TX-full error). */

extern volatile uint32_t **g_uart_dev_pp;   /* 0x20009864: -> device reg block */
extern uint8_t             g_uart_ctx[];     /* 0x20001A44: +0xB3C = TX ring ptr */

int uart_send_byte(uint8_t b)
{
    volatile uint32_t *dev = *g_uart_dev_pp;

    dev[0xC / 4] &= ~0x80u;                       /* mask TX interrupt */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    uint32_t rc = ringbuf_push_byte(*(ringbuf_t **)(g_uart_ctx + 0xB3C), b);

    dev = *g_uart_dev_pp;                          /* OEM re-loads the handle */
    dev[0xC / 4] |= 0x80u;                         /* re-enable TX interrupt */
    return (int)rc;
}
