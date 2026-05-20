#ifndef SHIFTERBOOT_MODBUS_H
#define SHIFTERBOOT_MODBUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Modbus RTU CRC16 — polynomial 0xA001 (reflected 0x8005), init 0xFFFF,
 * no final XOR. Returns the 16-bit accumulated CRC for `count` bytes
 * starting at `data`.
 *
 * Unlike shifterware's same-algorithm `modbus_crc16_compute`, the
 * shifterboot variant returns the value in r0 rather than splitting it
 * into two RAM-global bytes. All four callers consume it as a single
 * 16-bit value (typical pattern: `uxtb` for low byte, `asrs #8` for
 * high byte). */
uint16_t modbus_crc16(const uint8_t *data, uint16_t count);

/* USART1 RX interrupt handler — the bottom edge of shifterboot's Modbus
 * stack. Reads one byte per RXNE event and appends it to the 45-byte
 * inbound frame buffer at SRAM `0x200000C4`, indexed by the halfword
 * counter at SRAM `0x20000014`. Wired into the Cortex-M0 vector table
 * at slot 43 (USART1) — see `startup_mm32f031.S`. */
void USART1_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* SHIFTERBOOT_MODBUS_H */
