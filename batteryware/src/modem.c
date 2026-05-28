#include "batteryware.h"

/* CRC peripheral base — STM32L0 has CRC at 0x40023000. The previous
 * decomp labelled this USART2 because the wrapper below was thought
 * to be a USART2 init; in fact it's a CRC handle setup (the OEM
 * FUN_0800edf0 it forwards into is HAL_CRC_Init — see crc.c). */
static volatile uint32_t * const CRC_PERIPH = (volatile uint32_t *)0x40023000;

/* RCC base */
static volatile uint32_t * const RCC    = (volatile uint32_t *)0x40021000;

/* CRC handle in SRAM (was mis-labelled s_modem_cfg). */
static volatile uint32_t * const s_modem_cfg = (volatile uint32_t *)0x20002C20;

/* Timeout counter */
static volatile uint32_t * const s_modem_timeout = (volatile uint32_t *)0x200047DC;

/* Modem context struct */
static volatile uint32_t * const s_modem_ctx     = (volatile uint32_t *)0x20002BA4;

/*
 * Configure USART2/modem for communication.
 *
 * Initializes the USART config struct (base = USART2, fields cleared,
 * timeout = 3), enables USART2 clock in RCC_APB1ENR (bit 0x1000),
 * clears the timeout counter, then calls the configuration function.
 * Triggers system reset on failure.
 */
void modem_config(void)
{
    s_modem_cfg[0] = (uint32_t)CRC_PERIPH;   /* hcrc.Instance = CRC */
    *(volatile uint8_t *)(s_modem_cfg + 1) = 0;  /* field[1] = 0 */
    *(volatile uint8_t *)(s_modem_cfg + 5 / 4) = 0;  /* field[5] = 0 */
    s_modem_cfg[5 / 4] = 0;
    s_modem_cfg[6 / 4] = 0;
    s_modem_cfg[8 / 4] = 3;              /* CRCLength encoding */

    /* Enable CRC clock in RCC_AHBENR (bit 12). The original comment
     * here said "USART2 in APB1ENR" but the +0x30 offset is AHBENR on
     * STM32L0, and bit 12 of AHBENR is CRCEN. */
    RCC[0x30 / 4] |= 0x1000;

    *s_modem_timeout = 0;

    extern uint32_t crc_init(void *hcrc);  /* FUN_0800edf0 — see crc.c */
    if (crc_init((void *)s_modem_cfg) != 0) {
        system_reset();
    }
}

/*
 * Re-initialize the USART2/modem.
 *
 * Enables NVIC IRQ 0x19 (USART2), sets RCC_APB1ENR bit 0x1000,
 * masks interrupt enable, deinits the modem context via modem_deinit,
 * masks RCC_APB1RSTR, resets GPIOB pins 3-5 (0x38), and clears
 * the modem context pointer.
 */
void modem_reinit(void)
{
    nvic_enable_irq_s_dsb(0x19);

    /* Set USART2 clock bit in RCC_APB1ENR */
    RCC[0x24 / 4] |= 0x1000;
    /* Mask interrupt enable */
    RCC[0x24 / 4] &= 0xFFFFEFFF;

    /* Deinitialize modem */
    modem_deinit((void *)s_modem_ctx);

    /* Reset USART2 in RCC_APB1RSTR */
    RCC[0x34 / 4] &= 0xFFFFEFFF;

    /* Reset GPIOB pins */
    gpio_pin_reset(0x50000400, 0x38);

    *s_modem_ctx = 0;
}

/*
 * Deinitialize the USART/modem context.
 *
 * Sets the context's status byte to 2, clears bit 6 (0x40) in the base
 * register, calls the deinit thunk, then zeroes the status and control
 * fields. Returns true if ctx was null (error), false on success.
 */
bool modem_deinit(void *ctx)
{
    if (ctx != NULL) {
        volatile uint32_t *c = (volatile uint32_t *)ctx;
        *(volatile uint8_t *)((uintptr_t)c + 0x51) = 2;
        *c &= ~0x40U;
        extern void modem_deinit_thunk(void *);
        modem_deinit_thunk(ctx);
        c[0x15] = 0;
        *(volatile uint8_t *)((uintptr_t)c + 0x51) = 0;
        *(volatile uint8_t *)(c + 0x14) = 0;
    }
    return ctx == NULL;
}

/*
 * Bootloader entry sequence.
 *
 * Prints "I am VanMoof AP", turns off charge MOSFET, enters state 6,
 * then calls FUN_080050ac (power-on GPIO check). If the check passes
 * (true), verifies a config block and enters an infinite loop (bootloader
 * mode). Otherwise falls through to state_handler_01 (normal boot).
 */
void bootloader_entry(void)
{
    extern volatile uint8_t * const s_protection_cfg;
    extern void charge_mosfet_off(void);
    extern void bms_set_state(uint8_t state);
    extern void memcmp_verify(char *a, uint32_t size, char *b);
    extern void state_handler_01(void);

    s_protection_cfg[5] = 1;
    *s_modem_ctx |= 0x20000;
    uart_printf((uint8_t *)0x08007204);
    uart_tx_flush();
    charge_mosfet_off();
    gpio_bit_write(0x50000400, 0x200, 0);
    bms_configure(0);
    bms_set_state(6);
    uart_printf((uint8_t *)0x0800720C);
    uart_tx_flush();

    extern bool power_on_gpio_check(void);  /* FUN_080050ac */
    if (power_on_gpio_check()) {
        memcmp_verify((char *)0x08007214, 0x80, (char *)0x08007210);
        gpio_bit_write(0x50000400, 0x1000, 0);
        while (1) { }  /* bootloader mode — wait for reset */
    }

    *s_modem_ctx &= 0xFFFFFFFD;
    state_handler_01();
}

/*
 * Append 2 bytes to the modem response buffer.
 *
 * Writes two bytes at the current buffer position, increments the
 * index by 2, decrements the remaining count by 2, and increments
 * the outgoing counter.
 */
void modem_send_2bytes(uint8_t b1, uint8_t b2)
{
    volatile uint32_t * const s_buf_idx   = (volatile uint32_t *)0x200047D0;
    volatile uint8_t  * const s_buf_base  = (volatile uint8_t  *)0x20004648;
    volatile uint32_t * const s_buf_rem   = (volatile uint32_t *)0x200047D4;
    volatile uint32_t * const s_tx_count  = (volatile uint32_t *)0x20004748;

    s_buf_base[*s_buf_idx] = b1;
    *s_buf_idx += 1;
    *s_buf_rem -= 1;
    s_buf_base[*s_buf_idx] = b2;
    *s_buf_idx += 1;
    *s_buf_rem -= 1;
    *s_tx_count += 1;
}

/*
 * Temperature offset computation and send.
 *
 * Given a raw temperature byte (0-255):
 *   - If < 40: computes (40 - val) * (-10) → negative offset from 40°C
 *   - If >= 40: computes (val - 40) * 10 → positive offset from 40°C
 * Adds base offset 0x0AAB (2731 = 27.31°C in centikelvin, or a calibration
 * constant) and sends the resulting 16-bit value as two bytes.
 */
void temp_offset_send(uint8_t raw_temp)
{
    uint16_t val = raw_temp;
    int16_t offset;

    if (val < 0x28) {
        offset = (int16_t)((0x28 - val) * (-10));
    } else {
        offset = (int16_t)((val - 0x28) * 10);
    }

    offset += 0x0AAB;

    modem_send_2bytes((uint8_t)(offset >> 8), (uint8_t)offset);
}

/*
 * Bus ready check — returns the status byte at ctx+0x51.
 *
 * The status byte tracks the bus/I²C state machine phase.
 * 0 = idle/ready, 1 = active, 2 = deiniting, etc.
 */
uint8_t bus_ready_check(int ctx)
{
    return *(volatile uint8_t *)(ctx + 0x51);
}

/*
 * Modem restart thunk — empty ROP call site for exception vector dispatch.
 */
void modem_restart_thunk(void)
{
}

/*
 * Modem ISR acknowledgment thunks.
 * Compiler-generated duplicates used as ISR trampolines.
 */
void modem_isr_ack(void)
{
}

void modem_isr_ack_dup(void)
{
}

/*
 * Modem (USART1) initialization.
 *
 * Enables NVIC IRQ 12 (USART1), calls the USART1 config function
 * with the DMA context at 0x200024F4, masks RCC bit 9, resets
 * GPIOA pins 5-7 (0xE0), then zeroes the DMA context pointer.
 */
void modem_init(void)
{
    volatile uint32_t * const s_dma_ctx_init = (volatile uint32_t *)0x200024F4;
    volatile uint32_t * const RCC_INIT       = (volatile uint32_t *)0x40021000;

    nvic_enable_irq_s_dsb(12);

    usart1_dma_setup((int *)s_dma_ctx_init);

    RCC_INIT[0x34 / 4] &= 0xFFFFFDFF;

    gpio_pin_reset(0x50000000, 0xE0);

    *s_dma_ctx_init = 0;
}

/*
 * SMBus/I²C transmit engine.
 *
 * Sets up a DMA/USART transfer context for I²C/SMBus communication.
 * Returns status codes: 0 = success, 1 = null parameter, 2 = busy.
 *
 * Flow:
 *   1. If tx_active flag (ctx[0x14]) is set → busy (2)
 *   2. Sets tx_active = 1
 *   3. Checks bus ready (status byte 1) or special mode (ctx[1]==0x104
 *      with ctx[2]==0 and status 4)
 *   4. Null-check: tx_buf, rx_buf, or count zero → error (1)
 *   5. Sets status = 5 (if not already 4)
 *   6. Configures DMA pointers: ctx[0xC]=tx_buf, ctx[0xE]=rx_buf,
 *      sets transfer counts at ctx+0x36 and ctx+0x3E
 *   7. Picks callback pairs based on ctx[3]:
 *        - 0: tx_done=0x08015FA1, tx_err=0x08016057
 *        - other: tx_done=0x080160E9, tx_err=0x0801618F
 *   8. If ctx[10]==0x2000: masks *ctx with 0xFFFFDFFF, ORs 0x2000
 *   9. Sets SR bits 5-7 (0xE0) and enables USART (bit 6)
 *  10. Clears tx_active flag
 */
uint8_t smbus_transmit(int *ctx, int tx_buf, int rx_buf, int16_t count)
{
    if (((uint8_t)ctx[0x14]) == 1) {
        return 2;
    }

    *(volatile uint8_t *)(ctx + 0x14) = 1;

    uint8_t status = *(volatile uint8_t *)((uintptr_t)ctx + 0x51);

    if ((status == 1) ||
        ((ctx[1] == 0x104) && (ctx[2] == 0) &&
         (*(volatile uint8_t *)((uintptr_t)ctx + 0x51) == 4))) {

        if ((tx_buf == 0) || (rx_buf == 0) || (count == 0)) {
            *(volatile uint8_t *)(ctx + 0x14) = 0;
            return 1;
        }

        if (*(volatile uint8_t *)((uintptr_t)ctx + 0x51) != 4) {
            *(volatile uint8_t *)((uintptr_t)ctx + 0x51) = 5;
        }

        ctx[0x15] = 0;
        ctx[0xC] = tx_buf;
        *(volatile int16_t *)(ctx + 0xD) = count;
        *(volatile int16_t *)((uintptr_t)ctx + 0x36) = count;
        ctx[0xE] = rx_buf;
        *(volatile int16_t *)(ctx + 0xF) = count;
        *(volatile int16_t *)((uintptr_t)ctx + 0x3E) = count;

        if (ctx[3] == 0) {
            ctx[0x10] = (int)0x08015FA1;
            ctx[0x11] = (int)0x08016057;
        } else {
            ctx[0x10] = (int)0x080160E9;
            ctx[0x11] = (int)0x0801618F;
        }

        volatile uint32_t *reg = (volatile uint32_t *)*ctx;
        if (ctx[10] == 0x2000) {
            reg[0] &= 0xFFFFDFFF;
            reg[0] |= 0x2000;
        }

        reg[1] |= 0xE0;
        if ((reg[0] & 0x40) != 0x40) {
            reg[0] |= 0x40;
        }
    } else {
        *(volatile uint8_t *)(ctx + 0x14) = 0;
        return 2;
    }

    *(volatile uint8_t *)(ctx + 0x14) = 0;
    return 0;
}

/*
 * Modem command handler (modem_command_handler) — a duplicate/larger
 * version of the command parser that handles commands received over
 * the modem/USART interface, including flash programming commands.
 *
 * This handler processes commands with additional state tracking for
 * the modem communication layer. It supports:
 *   - Flash page programming commands (0x10, 0x11, 0x12)
 *   - Configuration read/write commands
 *   - Status query commands
 *   - Reset/bootloader commands
 *
 * The command frame format is the same as uart_protocol_handler:
 *   [0xAA] [cmd_byte] [data...] [CRC16]
 *
 * But this handler additionally tracks modem state and handles
 * longer multi-packet exchanges for flash programming.
 */
void modem_command_handler(uint8_t byte)
{
    volatile uint8_t  * const s_state    = (volatile uint8_t  *)0x20002CF4;
    volatile uint8_t  * const s_buf      = (volatile uint8_t  *)0x20002CF5;
    volatile uint16_t * const s_rx_idx   = (volatile uint16_t *)0x20002CF8;
    volatile uint16_t * const s_rx_total = (volatile uint16_t *)0x20002CFA;
    volatile uint8_t  * const s_modem_st = (volatile uint8_t  *)0x20002CFC;

    uint8_t state = *s_state;

    if (state == 0) {
        /* Waiting for sync byte 0xAA */
        if (byte == 0xAA) {
            *s_state = 1;
            s_buf[0] = 0xAA;
            *s_rx_idx = 1;
        }
        return;
    }

    if (state == 1) {
        /* Got sync, store command byte */
        s_buf[*s_rx_idx] = byte;
        *s_rx_idx = 2;
        *s_state = 2;
        *s_modem_st = byte;

        /* Command byte determines expected frame length */
        if (byte < 0x80) {
            *s_rx_total = 8;
        } else if (byte == 0x80) {
            *s_rx_total = 0x80;
        } else {
            *s_rx_total = byte & 0x7F;
            if (*s_rx_total < 8) {
                *s_rx_total = 8;
            }
        }
        return;
    }

    /* State 2+: accumulating data */
    if (*s_rx_idx < *s_rx_total) {
        s_buf[*s_rx_idx] = byte;
        *s_rx_idx += 1;
    }

    /* Check if frame is complete */
    if (*s_rx_idx >= *s_rx_total) {
        uint16_t frame_len = *s_rx_total;
        uint16_t calc_crc = crc16_calc((uint8_t *)s_buf, (int16_t)(frame_len - 2));
        uint16_t rx_crc   = (uint16_t)s_buf[frame_len - 1] << 8 | s_buf[frame_len - 2];

        if (calc_crc == rx_crc) {
            /* CRC OK — dispatch based on command byte */
            uint8_t cmd = s_buf[1];

            if (cmd == 0x10) {
                /* Flash page program command */
                extern void flash_program_handler(uint8_t *data, uint16_t len);
                flash_program_handler((uint8_t *)s_buf, frame_len);
            } else if (cmd == 0x11) {
                /* Flash erase command */
                extern void flash_erase_handler(uint8_t *data, uint16_t len);
                flash_erase_handler((uint8_t *)s_buf, frame_len);
            } else if (cmd == 0x12) {
                /* Flash verify command */
                extern void flash_verify_handler(uint8_t *data, uint16_t len);
                flash_verify_handler((uint8_t *)s_buf, frame_len);
            } else if (cmd < 0x10) {
                /* Standard commands — dispatch to command_parser */
                extern void command_parser(uint32_t buf_addr, int buf_len, uint8_t cmd);
                command_parser((uint32_t)(uintptr_t)s_buf, (int)(frame_len - 2), cmd);
            } else {
                /* Unknown modem command */
                extern void veneer_a6aa(void);
                veneer_a6aa();
            }
        } else {
            /* CRC error */
            extern void veneer_a6aa(void);
            veneer_a6aa();
        }

        /* Reset state machine */
        *s_state = 0;
        *s_rx_idx = 0;
        *s_rx_total = 0;
        *s_modem_st = 0;
    }
}
