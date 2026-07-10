/*
 * bus.c — VanMoof S3 mainware UART4 ("bus") byte transport.
 *
 * UART4 is the second inter-module serial link, carrying the Modbus-RTU traffic
 * to the battery BMS (slave 0xAA); battery.c is its protocol owner and states.c /
 * update.c also drive it. This file is the raw byte layer: the locked TX/RX
 * primitives the protocol code calls and the RX/TX interrupt service routine that
 * moves bytes between the UART4 register block and the software ring buffers.
 *
 * Memory model (resolved from the OEM literal pool):
 *   0x20009964  RAM slot holding the UART4 register-block pointer (set by
 *               uart4_init); the dev block is reached by one extra dereference.
 *   0x20001A44  the shared serial context; UART4's TX ring handle is at +0x330
 *               and its RX ring handle at +0x334.
 *
 * Behaviour-equivalent reconstruction of the live disassembly. The TX/RX
 * primitives mirror uart_send_byte/ssp_rx_byte exactly: mask the relevant CR1
 * interrupt-enable bit (TXEIE 0x80 / RXNEIE 0x20) behind a DSB/ISB so the disable
 * lands, touch the ring, then re-enable. Same ABI quirk preserved — the ring
 * status that ringbuf_push_byte / ringbuf_get_byte leave in r0 survives the
 * trailing register re-enable untouched and is the implicit return value
 * (1 = byte moved, 0 = ring full/empty); bus_rx_byte_locked's callers test it.
 */

#include <stdint.h>

#include "bus.h"
#include "scheduler.h"   /* scheduler_release (bus_crc16_verify) */
#include "util.h"

/* Modbus-RTU CRC-16 (poly 0xA001): one shared accumulator at SRAM 0x200000C2 that
 * the inter-module bus RX/TX paths feed one byte at a time. */
#define BUS_CRC (*(volatile uint16_t *)0x200000C2u)

void bus_crc16_reset(void)
{
    BUS_CRC = 0xffff;
}

uint16_t bus_crc16_update(uint16_t byte)
{
    BUS_CRC ^= byte;
    for (int i = 8; i != 0; i--) {
        uint16_t c = BUS_CRC;
        BUS_CRC = (c & 1) ? (uint16_t)((c >> 1) ^ 0xa001) : (uint16_t)(c >> 1);
    }
    return BUS_CRC;
}

uint16_t bus_crc16_get(void)
{
    return BUS_CRC;
}

/* Despite the name, this resets the bus-RX framing state: release the RX-timeout
 * scheduler slot at SRAM 0x200000C4 and clear the framing flag at 0x20006E90
 * (OEM 0x08039954). */
void bus_crc16_verify(void)
{
    scheduler_release((uint8_t *)0x200000c4u);
    *(volatile uint8_t *)0x20006e90u = 0;
}

/* UART4 register-block handle (pointer-to-pointer) and the serial context base
 * holding its ring handles. */
#define UART4_HANDLE   (*(volatile uint32_t * volatile *)0x20009964u)
#define UART4_CTX      0x20001A44u
#define UART4_TX_RING  (*(ringbuf_t * volatile *)(UART4_CTX + 0x330u))
#define UART4_RX_RING  (*(ringbuf_t * volatile *)(UART4_CTX + 0x334u))

/* Locked single-byte TX into the UART4 TX ring (OEM bus_tx_enqueue_byte,
 * 0x0803639C). */
int bus_tx_enqueue_byte(uint8_t b)
{
    volatile uint32_t *dev = UART4_HANDLE;

    dev[0xC / 4] &= ~0x80u;                        /* mask TXEIE */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    uint32_t rc = ringbuf_push_byte(UART4_TX_RING, b);

    dev = UART4_HANDLE;                            /* OEM re-loads the handle */
    dev[0xC / 4] |= 0x80u;                         /* re-enable TXEIE */
    return (int)rc;
}

/* Locked single-byte RX from the UART4 RX ring (OEM bus_rx_byte_locked,
 * 0x080363EC). */
int bus_rx_byte_locked(uint8_t *out)
{
    volatile uint32_t *dev = UART4_HANDLE;

    dev[0xC / 4] &= ~0x20u;                        /* mask RXNEIE */
    __asm volatile ("dsb 0xf" ::: "memory");
    __asm volatile ("isb 0xf" ::: "memory");

    uint32_t rc = ringbuf_get_byte(UART4_RX_RING, out);

    dev = UART4_HANDLE;                            /* OEM re-loads the handle */
    dev[0xC / 4] |= 0x20u;                         /* re-enable RXNEIE */
    return (int)rc;
}

/* Clear a latched USART error flag the RM0430 way: if `flag` (PE 0x1 / FE 0x2 /
 * NE 0x4 / ORE 0x8) is set in SR, read SR then DR — that read sequence clears the
 * sticky error bit (and discards the offending byte). The OEM inlines this CubeF4
 * __HAL_UART_CLEAR_*FLAG idiom once per error bit. */
static void usart_clear_error_flag(volatile uint32_t *d, uint32_t flag)
{
    volatile uint32_t tmp;
    if ((d[0] & flag) != 0) {
        tmp = 0;
        tmp = d[0];   /* SR */
        tmp = d[1];   /* DR */
        (void)tmp;
    }
}

/* UART4 RX/TX byte-pump ISR (OEM 0x08036424), invoked via a thin vector
 * trampoline. On RXNE with no parity/framing/noise/overrun error (SR bits 0-3
 * clear) and RXNEIE set, push DR into the RX ring; then clear any latched
 * PE/FE/NE/ORE error (read SR+DR); on TXE with TXEIE set, pop the next TX byte and
 * write it to DR, disabling TXEIE when the ring drains. The RX/TX gates use SR/CR1
 * sampled once; the error-clear block and TX-path register writes re-load the
 * device handle. */
void uart4_irq_handler(void)
{
    volatile uint32_t *dev = UART4_HANDLE;
    uint32_t sr  = dev[0];
    uint32_t cr1 = dev[0xC / 4];

    if ((sr & 0xFu) == 0 && (sr & 0x20u) != 0 && (cr1 & 0x20u) != 0) {
        ringbuf_push_byte(UART4_RX_RING, (uint8_t)dev[1]);
    }

    volatile uint32_t *d = UART4_HANDLE;
    usart_clear_error_flag(d, 0x1u);
    usart_clear_error_flag(d, 0x2u);
    usart_clear_error_flag(d, 0x4u);
    usart_clear_error_flag(d, 0x8u);

    if ((sr & 0x80u) != 0 && (cr1 & 0x80u) != 0) {
        uint8_t b;
        if (ringbuf_get_byte(UART4_TX_RING, &b) == 0) {
            UART4_HANDLE[0xC / 4] &= ~0x80u;
        } else {
            UART4_HANDLE[1] = b;
        }
    }
}
