/* bms_init.c — FEDL5236 chip initialisation sequence.
 *
 * OEM: FUN_08004d04 (864 B). Programs the FEDL5236 register set over
 * SMBus, then runs two acquisition loops to read status/voltage
 * registers back into SRAM:
 *
 *   Phase 1 — write registers (sync via SMBus):
 *     reg 0   = 0xaa            unlock
 *     reg 0   = 0x55            unlock part 2
 *     reg 9   = 0x00            (uart_tx_flush between groups)
 *     reg 10  = 0x00
 *     reg 11  = 0x00
 *     reg 12  = 0x00  mask 0x7f
 *     reg 14  = 0x9a
 *     reg 16  = 0xff
 *     reg 17  = 0x0f
 *     reg 15  = 0x00
 *     reg 1   = 0x0f
 *     reg 2   = 0x02
 *     reg 3   = 0x00
 *     reg 4   = 0x00
 *     reg 8   = 0x01
 *
 *   Phase 2 — status acquisition retry loop:
 *     write reg 6 = 0x92, delay 2ms, poll GPIOC pin 12 low, then
 *     smbus_read(3, 2) → status bytes; on success extract low-4 and
 *     low-5 nibbles, resend regs 3/4 as needed; if bit 1 of low nibble
 *     was set, smbus_read(0x2e, 2) and accumulate a 16-bit status word
 *     at 0x20002878. Repeat until the status word is non-zero.
 *
 *   Phase 3 — cell-voltage acquisition retry loop:
 *     write reg 5 = 0x99, delay 20ms, poll GPIOC pin 12 low, then
 *     smbus_read(3, 2) → status; on success and if bit 0 of low nibble
 *     is set, smbus_read(0x1a, 2 * cell_count) reads the per-cell
 *     voltage bytes into the SMBus buffer at 0x20002b84+2.
 *     Each u16 raw count is scaled (raw * 5000 / 4095, same formula as
 *     calculate_rsoc) into the cell voltage table at 0x200027d4.
 *     Then call veneer_11f48(). Repeat until sentinel 0x2000281c is
 *     non-zero.
 *
 *   Phase 4 — clear assorted per-cell scratch bytes and return.
 *
 * Tries to stay close to the OEM control flow rather than collapsing
 * the two retry loops into one helper — the slight code repetition
 * mirrors the OEM disassembly.
 */

#include "batteryware.h"

extern uint32_t veneer_11f48(void);

/* The flash format string at 0x08016fb8 lives past the end of this
 * .bin (image is 0x15610 bytes) — same situation as bms_setup's
 * 0x080171f0-block. Pass the OEM address through opaquely. */
static uint8_t * const s_fmt_boot_banner = (uint8_t *)0x08016fb8;

/* SMBus / FEDL5236 read buffer (raw bytes). bms_init reads halfword
 * status from +2..+3 and cell-voltage bytes from +2 onward. */
static volatile uint8_t * const s_smbus_buf = (volatile uint8_t *)0x20002B84;

/* Saved status word from Phase 2; non-zero exits Phase 2 retry. */
static volatile uint16_t * const s_status_word = (volatile uint16_t *)0x20002878;

/* Phase 3 sentinel (cell-voltage acquisition success counter). */
static volatile uint16_t * const s_voltage_done = (volatile uint16_t *)0x2000281C;

/* Low-nibble cache of status byte (used to gate reg-3 / reg-4 resend). */
static volatile uint8_t  * const s_status_lo   = (volatile uint8_t  *)0x2000289C;

/* Misc per-cell control bits flipped at Phase 4. */
static volatile uint8_t  * const s_irq_status  = (volatile uint8_t  *)0x20002C80;
static volatile uint16_t * const s_cell_table  = (volatile uint16_t *)0x200027D4;
static volatile uint8_t  * const s_cfg_base    = (volatile uint8_t  *)0x200028D0;

#define GPIOC_BASE      0x50000800U
#define BUSY_PIN_MASK   0x1000U   /* PC12 — FEDL5236 BUSY/DRDY indicator */

static void wait_for_chip_ready(void)
{
    while (gpio_bit_read(GPIOC_BASE, BUSY_PIN_MASK)) {
        /* spin until chip de-asserts BUSY */
    }
}

static void clear_irq_bit5(void)
{
    /* Bit 5 (0x20) of the IRQ-status byte gets cleared if currently set;
     * OEM uses lsls/lsrs to test the bit, then bics. */
    if ((*s_irq_status & 0x20U) != 0) {
        *s_irq_status &= ~0x20U;
    }
}

void bms_init(void)
{
    extern uint32_t veneer_1557c(uint32_t numerator, uint32_t divisor);

    /* --- Phase 1: program all FEDL5236 registers ------------------- */
    uart_printf(s_fmt_boot_banner);
    uart_tx_flush();
    led_flash();

    smbus_write_reg(0,  0xAA, 0xFF);
    smbus_write_reg(0,  0x55, 0xFF);
    smbus_write_reg(9,  0x00, 0xFF);
    uart_tx_flush();
    smbus_write_reg(10, 0x00, 0xFF);
    smbus_write_reg(11, 0x00, 0xFF);
    smbus_write_reg(12, 0x00, 0x7F);
    smbus_write_reg(14, 0x9A, 0xFF);
    smbus_write_reg(16, 0xFF, 0xFF);
    smbus_write_reg(17, 0x0F, 0xFF);
    smbus_write_reg(15, 0x00, 0xFF);
    smbus_write_reg(1,  0x0F, 0xFF);
    smbus_write_reg(2,  0x02, 0xFF);
    smbus_write_reg(3,  0x00, 0xFF);
    smbus_write_reg(4,  0x00, 0xFF);
    smbus_write_reg(8,  0x01, 0xFF);

    *s_status_word = 0;

    /* --- Phase 2: status acquisition retry ------------------------- */
    do {
        uart_tx_flush();
        smbus_write_reg(6, 0x92, 0xFF);
        delay_ms(2);
        wait_for_chip_ready();

        if (smbus_read(3, 2) != 0) {
            uint8_t lo = s_smbus_buf[2] & 0x0FU;
            *s_status_lo = s_smbus_buf[3] & 0x1FU;

            if (*s_status_lo != 0) {
                smbus_write_reg(4, 0, 0xFF);
            }
            if (lo != 0) {
                smbus_write_reg(3, 0, 0xFF);
            }
            if ((lo & 0x02U) != 0) {
                if (smbus_read(0x2E, 2) != 0) {
                    /* Accumulate two-byte status into the u16 word —
                     * OEM does: hi<<8 then add lo. */
                    *s_status_word = 0;
                    *s_status_word = (uint16_t)(*s_status_word + s_smbus_buf[3]);
                    *s_status_word = (uint16_t)(*s_status_word << 8);
                    *s_status_word = (uint16_t)(*s_status_word + s_smbus_buf[2]);
                }
            }
        }

        clear_irq_bit5();
        uart_resp_handler();
        uart_tx_isr();
    } while (*s_status_word == 0);

    /* --- Phase 3: cell-voltage acquisition retry ------------------- */
    *s_voltage_done = 0;

    do {
        uart_tx_flush();
        delay_ms(2);
        smbus_write_reg(5, 0x99, 0xFF);
        delay_ms(20);
        wait_for_chip_ready();

        if (smbus_read(3, 2) != 0) {
            uint8_t lo = s_smbus_buf[2] & 0x0FU;
            *s_status_lo = s_smbus_buf[3] & 0x1FU;

            if (*s_status_lo != 0) {
                smbus_write_reg(4, 0, 0xFF);
            }
            if (lo != 0) {
                smbus_write_reg(3, 0, 0xFF);
            }
            if ((lo & 0x01U) != 0) {
                uint8_t cell_count = s_cfg_base[4];
                if (smbus_read(0x1A, (uint8_t)(cell_count * 2)) != 0) {
                    /* Cell voltage table — pack 2 bytes/cell, scale,
                     * store as u16 at s_cell_table[cell_idx]. */
                    uint8_t buf_idx = 2;
                    for (uint8_t i = 0; i < cell_count; i++) {
                        uint32_t raw = s_smbus_buf[buf_idx];
                        buf_idx++;
                        raw |= ((uint32_t)s_smbus_buf[buf_idx]) << 8;
                        buf_idx++;
                        raw *= 5000U;
                        raw = veneer_1557c(raw, 0x0FFFU);
                        s_cell_table[i] = (uint16_t)raw;
                    }
                    (void)veneer_11f48();
                }
            }
        }

        clear_irq_bit5();
        uart_resp_handler();
        uart_tx_isr();
    } while (*s_voltage_done == 0);

    /* --- Phase 4: clear per-cell scratch -------------------------- */
    uart_tx_flush();
    *(volatile uint8_t  *)0x20002828 = 0;
    *(volatile uint8_t  *)0x200028C4 = 0;
    *(volatile uint8_t  *)0x200027F8 = 0;
    *(volatile uint8_t  *)0x200027F9 = 0;
    *(volatile uint8_t  *)0x200027FC = 0;
    *(volatile uint8_t  *)0x2000289D = 0;
    *(volatile uint16_t *)0x2000286C = 0;
}
