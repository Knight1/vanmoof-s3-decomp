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
 * Prints "\nShipping Mode\r" and "\nFEDL5236_PowerDown_Start()\r", turns
 * off the charge MOSFET, enters state 6, then calls FUN_080050ac (power-on
 * GPIO check). If the check passes (true), verifies the 0x80-byte EEPROM
 * config block at flash 0x08080C00 against the RAM copy at 0x200029A8 and
 * enters an infinite loop (bootloader mode). Otherwise falls through to
 * state_handler_01 (normal boot).
 */
void bootloader_entry(void)
{
    volatile uint8_t * const s_protection_cfg = (volatile uint8_t *)0x200028D0;
    extern void charge_mosfet_off(void);
    extern void bms_set_state(uint8_t state);
    extern void memcmp_verify(char *a, uint32_t size, char *b);
    extern void state_handler_01(void);

    s_protection_cfg[5] = 1;
    *s_modem_ctx |= 0x20000;
    uart_printf((uint8_t *)s_shipping_mode);
    uart_tx_flush();
    charge_mosfet_off();
    gpio_bit_write(0x50000400, 0x200, 0);
    bms_configure(0);
    bms_set_state(6);
    uart_printf((uint8_t *)s_pdown_start);
    uart_tx_flush();

    if (button_entry_check()) {  /* FUN_080050ac */
        memcmp_verify((char *)0x08080C00, 0x80, (char *)0x200029A8);
        gpio_bit_write(0x50000400, 0x1000, 0);
        while (1) { }  /* bootloader mode — wait for reset */
    }

    *s_modem_ctx &= 0xFFFFFDFF;
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
 * Command dispatch tail (FUN_0800ce9e) — the cmd != 3 branch of the UART
 * command processor (uart_protocol_handler). Operates on the same command
 * buffer at 0x20004548. In the OEM image this shares the processor's stack
 * frame; modelled here as an independent function (the shared slots are not
 * live across the call).
 *
 * Dispatches on the 16-bit command word cw = cmdbuf[2:3] (big-endian);
 * arg = cmdbuf[4:5]:
 *   0xF020/F021/F022/F023 — counter-store commands
 *   0xF45                  — history hex-dump readout
 *   0x95                   — shipping mode (charge off, BMS idle)
 *   cw < 0x1b              — per-command config table (table1)
 *   0x80                   — firmware-update / OAD bring-up
 *   otherwise              — default tail: echo + bootloader entry +
 *                            config-bit tables (table2/table3)
 */

/* Config-table index predicate: states whose table2/table3 arm toggles a
 * control bit (rather than just bumping the counter). */
static bool cfg_idx_special(uint8_t st)
{
    switch (st) {
    case 1: case 3: case 7: case 8: case 11: case 12: case 18: case 19:
        return true;
    default:
        return false;
    }
}

/* table2 arm: clear control bit 11 / mode bit 0 for the special states,
 * then bump the response counter and send. */
static void table2_arm(uint8_t st)
{
    if (cfg_idx_special(st)) {
        *(volatile uint32_t *)0x20002C00 &= 0xFFFFF7FFu;
        *(volatile uint8_t *)0x20002870 &= (uint8_t)~1u;
        bms_configure(*(volatile uint8_t *)0x20002870);
    }
    *(volatile uint8_t *)0x20004748 += 1;
    cmd_send_response();
}

/* table3 arm: set control bit 11 / mode bit 0 for the special states. */
static void table3_arm(uint8_t st)
{
    if (cfg_idx_special(st)) {
        *(volatile uint32_t *)0x20002C00 |= 0x800u;
        *(volatile uint8_t *)0x20002870 |= 1u;
        bms_configure(*(volatile uint8_t *)0x20002870);
    }
    *(volatile uint8_t *)0x20004748 += 1;
    cmd_send_response();
}

/* Default-tail config dispatch (FUN_0800cfc0): arg selects table3 (set) vs
 * table2 (clear), indexed by the live state at 0x20002B58. */
static void dispatch_config_tables(uint16_t arg)
{
    uint8_t st = *(volatile uint8_t *)0x20002B58;
    if (arg != 0) {
        if (arg != 1) cmd_send_response();
        table3_arm(st);
    } else {
        table2_arm(st);
    }
}

/* Default tail (FUN_0800cf72): echo the 8-byte command, drop to the
 * bootloader-entry hook, then run the config tables. */
static void dispatch_default_tail(uint16_t arg)
{
    uint8_t *cmdbuf = (uint8_t *)0x20004548;
    if (arg != 0) cmd_send_response();
    for (uint8_t i = 0; i <= 7; i++) {
        uart_putchar(cmdbuf[i]);
    }
    uart_tx_flush();
    bootloader_entry();
    cmd_send_response();
    dispatch_config_tables(arg);
}

/* cw 0xF45 — dump a 0x38-byte history record (from the ext-flash ring
 * buffers 0x08080200 / 0x08080E00) as hex over the service UART. */
static void history_dump(uint16_t arg)
{
    uint8_t *scratch = (uint8_t *)0x20002B10;
    for (uint8_t i = 0; i < 0x38; i++) scratch[i] = 0;

    uint16_t idx = (uint16_t)(arg / 100);
    if (idx < 0x32) {
        memcpy_oem((const uint8_t *)(uintptr_t)(0x08080200 + idx * 0x38), 0x38, scratch);
    } else {
        memcpy_oem((const uint8_t *)(uintptr_t)(0x08080E00 + (idx - 0x32) * 0x38), 0x38, scratch);
    }
    uart_printf((uint8_t *)0x080173EC);
    for (uint8_t i = 0; i < 0x38; i += 8) {
        uart_printf((uint8_t *)0x080173FC);
    }
    uart_printf((uint8_t *)0x08017420);
    uart_tx_flush();
    *(volatile uint8_t *)0x20004748 += 1;
    cmd_send_response();
}

/* cw 0x95 — shipping mode: charge MOSFET off, enable line low, BMS idle. */
static void shipping_mode(void)
{
    *(volatile uint8_t *)0x20004748 += 1;
    *(volatile uint32_t *)0x200047D8 = 0;
    charge_mosfet_off();
    gpio_bit_write(0x50000400, 0x200, 0);
    *(volatile uint32_t *)0x20002C00 &= 0xFFFFF7FFu;
    bms_configure(0);
    cmd_send_response();
}

/* table1[9] (FUN_0800d0de) — persist a config flag (0x200028D0+5) to
 * ext-flash 0x08080008 based on arg. */
static void arm_cfg_flag(uint16_t arg)
{
    if (arg == 0) {
        *(volatile uint8_t *)(0x200028D0 + 5) = 0;
        memcmp_verify((char *)0x08080008, 2, (char *)0x200028D5);
    } else if (arg == 1) {
        *(volatile uint8_t *)(0x200028D0 + 5) = 1;
        memcmp_verify((char *)0x08080008, 2, (char *)0x200028D5);
    }
    *(volatile uint8_t *)0x20004748 += 1;
    cmd_send_response();
}

/* table1[10] (FUN_0800d128) — capture the tick triplet, and if all three
 * differ from the ext-flash references, persist arg to 0x08080006. */
static void arm_tick_persist(uint16_t arg)
{
    uint32_t t1 = tick_val_get();
    uint32_t t2 = tick_ms_get();
    uint32_t t3 = tick_timeout_get();
    uint32_t ref1, ref2, ref3;
    memcpy_oem((const uint8_t *)0x08080021, 4, (uint8_t *)&ref1);
    memcpy_oem((const uint8_t *)0x08080025, 4, (uint8_t *)&ref2);
    memcpy_oem((const uint8_t *)0x08080029, 4, (uint8_t *)&ref3);
    if (t1 != ref1 && t2 != ref2 && t3 != ref3) {
        memcmp_verify((char *)0x08080006, 2, (char *)&arg);
    }
    *(volatile uint8_t *)0x20004748 += 1;
    cmd_send_response();
}

/* table1[26] (FUN_0800d1be) — toggle control bit 12 (0x20002C00) and mode
 * bit 1 (0x20002870) when the live state is in {1..3, 0xd..0x11}. */
static void arm_state_bit(uint16_t arg)
{
    if (arg == 0) {
        uint8_t st = *(volatile uint8_t *)0x20002B58;
        if ((st > 0 && st <= 3) || (st >= 0xd && st <= 0x11)) {
            *(volatile uint32_t *)0x20002C00 &= 0xFFFFEFFFu;
            *(volatile uint8_t *)0x20002870 &= (uint8_t)~2u;
            bms_configure(*(volatile uint8_t *)0x20002870);
        }
        *(volatile uint8_t *)0x20004748 += 1;
        cmd_send_response();
    } else if (arg == 1) {
        uint8_t st = *(volatile uint8_t *)0x20002B58;
        if ((st > 0 && st <= 3) || (st >= 0xd && st <= 0x11)) {
            *(volatile uint32_t *)0x20002C00 |= 0x1000u;
            *(volatile uint8_t *)0x20002870 |= 2u;
            bms_configure(*(volatile uint8_t *)0x20002870);
        }
        *(volatile uint8_t *)0x20004748 += 1;
        cmd_send_response();
    } else {
        cmd_send_response();
    }
}

/* cw 0x80 (FUN_0800d272) — firmware-update path: quiesce the pack, reinit
 * the modem UART, and when a full 0x5000-byte image has been staged at
 * 0x0801A800, reconfigure the oscillator/clock tree and copy the staged
 * image down to the application area at 0x08000000 (page-verified), then
 * CRC-check it against 0x08004FFC. */
static void oad_firmware_update(void)
{
    uint8_t *cmdbuf = (uint8_t *)0x20004548;
    volatile uint8_t *retry = (volatile uint8_t *)0x20002C72;

    gpio_bit_write(0x50000400, 1, 0);
    charge_mosfet_off();
    gpio_bit_write(0x50000400, 0x200, 0);
    *(volatile uint32_t *)0x20002C00 &= 0xFFFFF7FFu;
    bms_configure(0);
    modem_init();
    modem_reinit();
    nvic_enable_irq_s_dsb(5);
    nvic_enable_irq_s_dsb(7);
    *(volatile uint32_t *)(0x40010400 + 0x14) = 1;
    *(volatile uint32_t *)(0x40010400 + 0x14) = 0x2000;
    *(volatile uint8_t *)0x20002BFC &= 0xfd;
    *(volatile uint8_t *)0x20002BFC &= 0xfe;
    for (uint8_t i = 0; i < 8; i++) {
        uart_putchar(cmdbuf[i]);
    }
    *(volatile uint32_t *)0x20002C04 = 0;
    uart_tx_flush();
    delay_ms(10);
    uart_printf((uint8_t *)0x080173D0);
    uart_tx_flush();
    delay_ms(10);

    if (*(volatile uint32_t *)0x200047D8 != 0x5000) {
        memcmp_verify((char *)0x08080C00, 0x80, (char *)0x200029A8);
        uint8_t marker = 0xcc;
        memcmp_verify((char *)0x08080000, 1, (char *)&marker);
        fault_led_trigger();
        cmd_send_response();
        return;
    }

    uint32_t osc_cfg[14];
    uint32_t clk_cfg[5];
    uint32_t rcc_cfg[9];
    memset_byte_fill((uint8_t *)osc_cfg, 0, 0x38);
    memset_byte_fill((uint8_t *)clk_cfg, 0, 0x14);
    memset_byte_fill((uint8_t *)rcc_cfg, 0, 0x24);

    osc_cfg[0] = 10; osc_cfg[3] = 1; osc_cfg[4] = 0x10; osc_cfg[5] = 1;
    osc_cfg[10] = 2; osc_cfg[12] = 0x40000; osc_cfg[13] = 0x400000;
    *retry = 0;
    do {
        *(volatile uint8_t *)0x200047DC = 0;
        if (rcc_osc_config(osc_cfg) == 0) {
            *retry = 0x32;
        } else if ((uint8_t)(*retry += 1) > 0x31) {
            system_reset();
        }
    } while (*retry < 0x32);

    clk_cfg[0] = 0xf; clk_cfg[1] = 1; clk_cfg[2] = 0; clk_cfg[3] = 0; clk_cfg[4] = 0;
    *retry = 0;
    do {
        *(volatile uint8_t *)0x200047DC = 0;
        if (rcc_configure(clk_cfg, 1) == 0) {
            *retry = 0x32;
        } else if ((uint8_t)(*retry += 1) > 0x31) {
            system_reset();
        }
    } while (*retry < 0x32);

    rcc_cfg[0] = 1; rcc_cfg[2] = 2;
    *retry = 0;
    do {
        *(volatile uint8_t *)0x200047DC = 0;
        if (rcc_reconfigure(rcc_cfg) == 0) {
            *retry = 0x32;
        } else if ((uint8_t)(*retry += 1) > 0x31) {
            system_reset();
        }
    } while (*retry < 0x32);

    do {
        uint32_t dst = 0x08000000;
        uint32_t *src = (uint32_t *)0x0801A800;
        int32_t remaining = 0x5000;
        do {
            word_to_bytes(src, 0x80, 0x2000474C);
            do {
                flash_dma_start(dst);
            } while (dma_compare(dst, 0x80, 0x2000474C) != 0);
            remaining -= 0x80;
            dst += 0x80;
            src += 0x20;
        } while (remaining != 0);
    } while (dma_transfer_irq((volatile uint32_t *)0x20002C20, (void *)0x08000000,
                              (*(volatile uint32_t *)0x200047D8 - 4) >> 2) !=
             *(volatile uint32_t *)0x08004FFC);
    flash_dma_start(0x0801A800);

    fault_led_trigger();
    cmd_send_response();
}

void modem_command_dispatch(void)
{
    uint8_t *cmdbuf = (uint8_t *)0x20004548;

    if (cmdbuf[1] != 6) {
        cmd_send_8byte();
    }
    uint16_t cw  = (uint16_t)((cmdbuf[2] << 8) | cmdbuf[3]);
    uint16_t arg = (uint16_t)((cmdbuf[4] << 8) | cmdbuf[5]);

    if (cw == 0xf020) { cmd_counter_inc(arg); return; }
    if (cw == 0xf021) { cmd_counter_inc_v2(arg); return; }
    if (cw == 0xf022) { cmd_counter_inc_v3(arg); return; }
    if (cw == 0xf023) {
        cmd_write_and_inc(arg, (volatile uint8_t *)0x200029A8, (volatile uint8_t *)0x08080C00);
        return;
    }
    if (cw == 0xf45) { history_dump(arg); return; }
    if (cw == 0x95)  { shipping_mode(); return; }

    if (cw < 0x1b) {
        switch (cw) {
        case 1:    dispatch_default_tail(arg); break;
        case 8:    dispatch_config_tables(arg); break;
        case 9:    arm_cfg_flag(arg); break;
        case 10:   arm_tick_persist(arg); break;
        case 0x1a: arm_state_bit(arg); break;
        default:   cmd_send_response(); break;
        }
        return;
    }

    if (cw == 0x80) { oad_firmware_update(); return; }

    cmd_send_response();
    dispatch_default_tail(arg);
}
