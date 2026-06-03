#include "powerbankware.h"

/*
 * FEDL5236 battery-AFE SPI driver.
 *
 *   fedl5236_command_write = OEM FUN_0800d558 (338 B)
 *   fedl5236_read_data     = OEM FUN_0800d6d0 (362 B)
 *
 * The AFE is wired to a hardware SPI peripheral (HAL handle at SRAM
 * 0x20000634) with the chip-select on GPIOA PA15 (bit 0x8000). Both calls
 * drive CS low, then a full-duplex HAL TransmitReceive. Retry/timeout is
 * counted in 1 ms software ticks (flag byte 0x2000077c bit 0), giving each
 * phase a ~10 ms budget. This is the powerbankware-specific framing — the
 * batteryware sibling reaches the same chip over bit-banged SMBus.
 *
 * Shared SRAM scratch (absolute addresses, also read by FEDL5236_Initialize
 * and the per-state routines):
 *   0x20000634  SPI HAL handle
 *   0x200005f4  TX frame buffer
 *   0x20000614  RX frame buffer
 *   0x200005f1  byte count for the next transfer
 *   0x20000412  register-9 shadow (cached last value)
 *   0x2000077c  SysTick flag byte (bit 0 = 1 ms tick)
 */

#define FEDL_SPI_HANDLE  ((void *)0x20000634)
#define FEDL_CS_PORT     0x48000000u      /* GPIOA */
#define FEDL_CS_PIN      0x8000u          /* PA15  */
#define BMS_RECORD       ((void *)0x200004d0)  /* 128-byte BMS record */
#define EEPROM_HANDLE    ((void *)0x20000434)  /* I2C2 handle */

static volatile uint8_t * const s_tx          = (volatile uint8_t *)0x200005f4;
static volatile uint8_t * const s_rx          = (volatile uint8_t *)0x20000614;
static volatile uint8_t * const s_count       = (volatile uint8_t *)0x200005f1;
static volatile uint8_t * const s_reg9_shadow = (volatile uint8_t *)0x20000412;
static volatile uint8_t * const s_tick        = (volatile uint8_t *)0x2000077c;
static volatile uint8_t * const s_test_mode   = (volatile uint8_t *)0x2000069c;  /* 0 = normal */

/* spi_get_state / spi_transmit_receive / spi_error_reset live in spi.c
 * (declared in the header). The rest are not yet translated — declared here
 * so this module compiles; they land in their own passes. */
/* log_print / uart_flush / uart_tx_isr live in uart.c (declared in header). */
extern void    system_reset_hang(void);        /* FUN_0800bab4: SYSRESETREQ + spin */

/* mem_zero/mem_compare/delay_ms_service, eeprom_mem_write/read and
 * bms_record_crc are all declared in the header (src/mem.c, src/delay.c,
 * src/i2c.c, src/crc.c). */
extern const char s_write_bms_record[];
extern const char s_check_bms_record[];

/*
 * FEDL5236 frame CRC-8 (OEM FUN_0801db48): a trampoline that tail-calls an
 * SRAM-resident routine whose pointer sits at 0x200000c4 (Thumb address
 * 0x200000c5), installed by the Reset_Handler's .data copy from flash. The
 * computation itself lives in that relocated block — not analysable as a
 * flash function here — so this faithfully reproduces the indirect dispatch.
 */
/* SRAM slot holding the relocated CRC-8 routine pointer (Thumb 0x200000c5). */
#define FEDL_CRC8_FN_SLOT  ((void *)0x200000c4)

uint8_t fedl5236_crc8(volatile uint8_t *buf, int len)
{
    uint8_t (* const fn)(volatile uint8_t *, int) =
        *(uint8_t (* const *)(volatile uint8_t *, int))FEDL_CRC8_FN_SLOT;
    return fn(buf, len);
}

extern const char s_fedl_cmd_busy[];
extern const char s_fedl_cmd_ng[];
extern const char s_fedl_cmd_timeout[];
extern const char s_fedl_rd_busy[];
extern const char s_fedl_rd_ng[];
extern const char s_fedl_rd_retry[];
extern const char s_check_charger_v[];
extern const char s_charger_v_fmt[];
extern const char s_check_charger_timeout[];
extern const char s_check_pupin[];
extern const char s_check_pupin_timeout[];
extern const char s_pdown_set_command[];
extern const char s_pdown_over5[];

/*
 * Write `val` to FEDL5236 register `reg`. Waits up to ~10 ms for the SPI to
 * go ready, sends the 3-byte write frame, then waits up to ~10 ms for the
 * transfer to complete. Register 9 also caches `val` in the shadow byte.
 */
void fedl5236_command_write(uint8_t reg, uint8_t val)
{
    uint8_t retry = 0;

    do {
        do {
            if (spi_get_state(FEDL_SPI_HANDLE) == 1) {
                if (reg == 9) {
                    *s_reg9_shadow = val;
                }
                s_tx[0] = (uint8_t)((reg << 1) | 0x80);
                s_tx[1] = val;
                s_tx[2] = fedl5236_crc8(s_tx, 2);
                *s_count = 3;

                gpio_bit_write(FEDL_CS_PORT, FEDL_CS_PIN, 0);   /* CS low */

                if (spi_transmit_receive(FEDL_SPI_HANDLE, s_tx, s_rx, *s_count) != 0) {
                    log_print(2, s_fedl_cmd_ng);
                    uart_flush();
                    spi_error_reset();
                }

                retry = 0;
                do {
                    do {
                        if (spi_get_state(FEDL_SPI_HANDLE) == 1) {
                            return;                              /* transfer done */
                        }
                    } while ((*s_tick & 1) == 0);
                    *s_tick &= (uint8_t)~1u;
                    retry++;
                } while (retry < 10);

                log_print(2, s_fedl_cmd_timeout);
                uart_flush();
                return;
            }
        } while ((*s_tick & 1) == 0);
        *s_tick &= (uint8_t)~1u;
        retry++;
    } while (retry < 10);

    log_print(2, s_fedl_cmd_busy);
    uart_flush();
}

/*
 * Read `count` data bytes from FEDL5236 register `reg`. Up to 10 attempts:
 * each waits (≤10 ms) for ready, sends the read frame, waits for completion,
 * patches the command echo back into the RX buffer, and verifies the
 * trailing CRC-8. Returns 1 on a CRC-valid read, 0 on busy/retry failure.
 */
int fedl5236_read_data(uint8_t reg, uint8_t count)
{
    uint8_t retry = 0;

    for (;;) {
        uint8_t busy = 0;
        while (spi_get_state(FEDL_SPI_HANDLE) != 1) {
            if ((*s_tick & 1) != 0) {
                *s_tick &= (uint8_t)~1u;
                busy++;
                if (busy > 9) {
                    log_print(2, s_fedl_rd_busy);
                    uart_flush();
                    return 0;
                }
            }
        }

        s_tx[0] = (uint8_t)((reg << 1) | 0x81);
        s_tx[1] = count;
        s_tx[2] = 0xff;
        *s_count = (uint8_t)(count + 3);

        gpio_bit_write(FEDL_CS_PORT, FEDL_CS_PIN, 0);           /* CS low */

        if (spi_transmit_receive(FEDL_SPI_HANDLE, s_tx, s_rx, *s_count) != 0) {
            log_print(2, s_fedl_rd_ng);
            uart_flush();
            spi_error_reset();
        }

        while (spi_get_state(FEDL_SPI_HANDLE) != 1) {
            /* wait for the read to complete */
        }

        s_rx[0] = s_tx[0];
        s_rx[1] = s_tx[1];

        retry++;
        if (retry > 9) {
            break;
        }
        if (fedl5236_crc8(s_rx, count + 2) == s_rx[count + 2]) {
            return 1;
        }
    }

    log_print(2, s_fedl_rd_retry);
    uart_flush();
    spi_error_reset();
    return 0;
}

/* ----------------------------------------------------------------------- *
 * FEDL5236_Initialize — OEM FUN_0800d85c.
 *
 * Full AFE bring-up. Stages (each prints its banner):
 *   1. Initialize     — wake the AFE, soft-reset (reg 9).
 *   2. Default_Setting— program the default register block.
 *   3. Zero_Current_Offset — pulse reg 6, poll the INT line (GPIOC PC13),
 *      read status (reg 3), service TS/balance flags, and read the total
 *      voltage (reg 0x2e) until it reads non-zero.
 *   4. Total_Voltage_Check — pulse reg 5, read the 10 cell voltages
 *      (reg 0x1a, 20 bytes), scale each raw 12-bit sample to mV
 *      (raw*5000/4095), accumulate sum + track max/min, and read the
 *      charger voltage (reg 0x34, *19536/1000). Loops until the cell sum
 *      is non-zero.
 *   5. Temperature watch — alternate TS0/TS1 selects (reg 7 = 0x81/0x83),
 *      convert each NTC sample (ntc_temp_read), and require both in
 *      [0x15,0x6e) °C. On a clean reading it clears the BMS fault snapshot
 *      and returns. After 200 consecutive bad readings it records a TS
 *      fault, powers the AFE down, raises PB12, waits for PA11, and halts.
 *
 * The INT line is GPIOC PC13 (0x2000): the AFE holds it high while busy,
 * so `while (gpio_bit_read(GPIOC, 0x2000))` waits for the conversion.
 */

#define GPIOA_BASE 0x48000000u
#define GPIOB_BASE 0x48000400u
#define GPIOC_BASE 0x48000800u
#define FEDL_INT_PIN 0x2000u            /* PC13 */
#define PIN_PA11     0x800u             /* AFE power-down ack */
#define PIN_PB12     0x1000u            /* AFE self-power-down */

static volatile uint16_t * const s_total_voltage   = (volatile uint16_t *)0x20000418;
static volatile uint8_t  * const s_ts_error        = (volatile uint8_t  *)0x2000041d;
static volatile uint16_t * const s_cell_voltage    = (volatile uint16_t *)0x20000380; /* [10] */
static volatile uint16_t * const s_cell_sum        = (volatile uint16_t *)0x200003ce;
static volatile uint16_t * const s_cell_max        = (volatile uint16_t *)0x200003a2;
static volatile uint8_t  * const s_cell_max_idx    = (volatile uint8_t  *)0x20000430;
static volatile uint16_t * const s_cell_min        = (volatile uint16_t *)0x200003d2;
static volatile uint8_t  * const s_cell_min_idx    = (volatile uint8_t  *)0x200003c4;
static volatile uint32_t * const s_charger_voltage = (volatile uint32_t *)0x2000042c;
static volatile uint8_t  * const s_temp            = (volatile uint8_t  *)0x20000218; /* [1]=TS0 [2]=TS1 */
static volatile uint8_t  * const s_state           = (volatile uint8_t  *)0x200005ac;
static volatile uint16_t * const s_mode            = (volatile uint16_t *)0x200006a0;
static volatile uint8_t  * const s_led_shadow      = (volatile uint8_t  *)0x200006e5;

/* BMS fault-snapshot cells cleared on a clean temperature reading. */
static volatile uint8_t  * const s_fault_a0  = (volatile uint8_t  *)0x200003a0;
static volatile uint8_t  * const s_fault_a1  = (volatile uint8_t  *)0x200003a1;
static volatile uint8_t  * const s_fault_a4  = (volatile uint8_t  *)0x200003a4;
static volatile uint8_t  * const s_fault_d0  = (volatile uint8_t  *)0x200003d0;
static volatile uint8_t  * const s_fault_0f  = (volatile uint8_t  *)0x2000020f;
static volatile uint8_t  * const s_fault_12  = (volatile uint8_t  *)0x20000212;
static volatile uint8_t  * const s_fault_1c  = (volatile uint8_t  *)0x2000021c;
static volatile uint8_t  * const s_fault_1e  = (volatile uint8_t  *)0x2000041e;
static volatile uint8_t  * const s_fault_28  = (volatile uint8_t  *)0x20000428;
static volatile uint16_t * const s_fault_reg = (volatile uint16_t *)0x20000410;

extern const char s_fedl_init[];          /* "\nFEDL5236_Initialize()\r"        */
extern const char s_fedl_default[];       /* "\nFEDL5236_Default_Setting()\r"   */
extern const char s_fedl_zero_offset[];   /* "\nFEDL5236_Zero_Current_Offset()\r" */
extern const char s_fedl_tv_check[];      /* "\nFEDL5236_Total_Voltage_Check()\r" */
extern const char s_fedl_charger_v[];     /* "\nFEDL5236_Charger_Voltage = %l\r" */
extern const char s_fedl_total_v[];       /* "\nFEDL5236_Total_Voltage = %i\r"   */
extern const char s_fedl_ts_error[];      /* "\nFEDL5236 TS Error()\r"           */
extern const char s_fedl_pd_start[];      /* "\nFEDL5236_PowerDown_Start()\r"    */
extern const char s_fedl_pd_finish[];     /* "\nFEDL5236_PowerDown_Finish()\r"   */

/* Service the status byte (rx[2]) + TS-error nibble (rx[3] & 0x1f) shared by
 * every read-status step. */
static void fedl5236_service_status(uint8_t status)
{
    *s_ts_error = (uint8_t)(s_rx[3] & 0x1f);
    if (*s_ts_error != 0) {
        fedl5236_command_write(4, 0);
    }
    if ((status & 0xf) != 0) {
        fedl5236_command_write(3, 0);
    }
}

void fedl5236_wakeup(void);   /* FUN_0800e160 — AFE power-on wake, defined below
                                 (kept after initialize to preserve OEM order) */

void fedl5236_initialize(void)
{
    uint8_t fail = 0;

    log_print(2, s_fedl_init);
    uart_flush();
    fedl5236_wakeup();
    fedl5236_command_write(9, 0);

    /* --- Default register block --- */
    log_print(2, s_fedl_default);
    uart_flush();
    fedl5236_command_write(10, 0);
    fedl5236_command_write(0xb, 0);
    fedl5236_command_write(0xc, 0);
    fedl5236_command_write(0xe, 10);
    fedl5236_command_write(0x10, 0xff);
    fedl5236_command_write(0x11, 0xff);
    fedl5236_command_write(0xf, 0);
    fedl5236_command_write(1, 0xf);
    fedl5236_command_write(2, 2);
    fedl5236_command_write(3, 0);
    fedl5236_command_write(4, 0);
    fedl5236_command_write(8, 1);
    *s_total_voltage = 0;

    /* --- Zero current offset: loop until total voltage reads valid --- */
    log_print(2, s_fedl_zero_offset);
    uart_flush();
    do {
        fedl5236_command_write(6, 0x92);
        while (gpio_bit_read(GPIOC_BASE, FEDL_INT_PIN)) {
        }
        if (fedl5236_read_data(3, 2) != 0) {
            uint8_t status = s_rx[2];
            fedl5236_service_status(status);
            if ((status & 2) != 0 && fedl5236_read_data(0x2e, 2) != 0) {
                *s_total_voltage = (uint16_t)((s_rx[3] << 8) | s_rx[2]);
            }
        }
    } while (*s_total_voltage == 0);

    /* --- Total voltage / cell scan: loop until the cell sum is valid --- */
    *s_cell_sum = 0;
    log_print(2, s_fedl_tv_check);
    uart_flush();
    do {
        delay_ms(2);
        fedl5236_command_write(5, 0x99);
        delay_ms(2);
        while (gpio_bit_read(GPIOC_BASE, FEDL_INT_PIN)) {
        }
        if (fedl5236_read_data(3, 2) != 0) {
            uint8_t status = s_rx[2];
            fedl5236_service_status(status);
            if ((status & 1) != 0 && fedl5236_read_data(0x1a, 0x14) != 0) {
                *s_cell_max = 0;
                *s_cell_max_idx = 0;
                *s_cell_min = 0xffff;
                *s_cell_min_idx = 0;
                *s_cell_sum = 0;

                uint8_t b = 2;
                for (uint8_t cell = 0; cell < 10; cell++) {
                    uint32_t raw = (uint32_t)(s_rx[b] | (s_rx[b + 1] << 8));
                    b += 2;
                    uint16_t mv = (uint16_t)((5000u * raw) / 0xfffu);
                    s_cell_voltage[cell] = mv;
                    *s_cell_sum = (uint16_t)(*s_cell_sum + mv);
                    if (*s_cell_max < mv) {
                        *s_cell_max = mv;
                        *s_cell_max_idx = (uint8_t)(cell + 1);
                    }
                    if (mv < *s_cell_min) {
                        *s_cell_min = mv;
                        *s_cell_min_idx = (uint8_t)(cell + 1);
                    }
                }
            }
        }

        if (*s_cell_sum != 0) {
            uint32_t charger_mv = 0;
            delay_ms(2);
            fedl5236_command_write(8, 0x91);
            while (gpio_bit_read(GPIOC_BASE, FEDL_INT_PIN)) {
            }
            if (fedl5236_read_data(3, 2) != 0) {
                uint8_t status = s_rx[2];
                fedl5236_service_status(status);
                if (fedl5236_read_data(0x34, 2) != 0) {
                    charger_mv = (uint32_t)(s_rx[2] | (s_rx[3] << 8));
                    charger_mv = (19536u * charger_mv) / 1000u;
                    *s_charger_voltage = charger_mv;
                }
                log_print(2, s_fedl_charger_v, charger_mv);
            }
        }
    } while (*s_cell_sum == 0);

    log_print(2, s_fedl_total_v, *s_cell_sum);
    uart_flush();

    /* --- Temperature watch --- */
    fail = 0;
    s_temp[1] = 0;
    s_temp[2] = 0;
    for (;;) {
        delay_ms(2);
        fedl5236_command_write(7, 0x81);          /* select TS0 */
        delay_ms(2);
        while (gpio_bit_read(GPIOC_BASE, FEDL_INT_PIN)) {
        }
        if (fedl5236_read_data(0x30, 2) != 0) {
            s_temp[1] = ntc_temp_read();
            fedl5236_command_write(7, 0x83);      /* select TS1 */
            delay_ms(2);
            while (gpio_bit_read(GPIOC_BASE, FEDL_INT_PIN)) {
            }
            if (fedl5236_read_data(0x32, 2) != 0) {
                s_temp[2] = ntc_temp_read();
                fedl5236_command_write(7, 0);
            }
        }

        if (s_temp[1] < 0x6e && s_temp[2] < 0x6e) {
            if (s_temp[1] < 0x15 || s_temp[2] < 0x15) {
                fail++;
            } else {
                fail = 0;
            }
        } else {
            fail++;
        }

        if (fail > 199) {
            break;
        }
        if (fail == 0) {
            /* Clean reading — clear the BMS fault snapshot and return. */
            *s_fault_d0  = 0;
            *s_fault_28  = 0;
            *s_fault_a0  = 0;
            *s_fault_a1  = 0;
            *s_fault_a4  = 0;
            *s_fault_1e  = 0;
            *s_fault_0f  = 0;
            *s_fault_1c  = 0;
            *s_fault_12  = 0;
            *s_fault_reg = 0;
            return;
        }
    }

    /* --- TS-fault path: power down and halt --- */
    *s_state = (s_temp[1] < 0x6e && s_temp[2] < 0x6e) ? 0x15 : 0x14;
    *s_mode &= (uint16_t)0xefffu;                  /* clear bit 12 */
    *s_led_shadow = 0;
    extend_io_update();
    log_print(2, s_fedl_ts_error);
    uart_flush();
    delay_ms(5000);
    log_print(2, s_fedl_pd_start);
    uart_flush();
    fedl5236_powerdown(0);
    log_print(2, s_fedl_pd_finish);
    uart_flush();
    gpio_bit_write(GPIOB_BASE, PIN_PB12, 1);       /* PB12 high */
    while (!gpio_bit_read(GPIOA_BASE, PIN_PA11)) {  /* wait PA11 */
    }
    gpio_bit_write(GPIOB_BASE, PIN_PB12, 0);       /* PB12 low */
    for (;;) {
    }
}

/* ----------------------------------------------------------------------- *
 * AFE power-on wake (OEM FUN_0800e160) + its two GPIO pulse helpers
 * (FUN_080149c4 / FUN_080149ec). Drives the GPIOB PB2/PB10 control lines
 * (level-shifter / discharge enable) high, waits, drops them, waits again.
 * The dwell times switch on the boot-mode flag at 0x2000069c (0 = normal,
 * else the shorter test-mode delays).
 */
void fedl5236_aux_on(void)
{
    gpio_bit_write(GPIOB_BASE, 4, 1);      /* PB2  */
    gpio_bit_write(GPIOB_BASE, 0x400, 1);  /* PB10 */
}

void fedl5236_aux_off(void)
{
    gpio_bit_write(GPIOB_BASE, 4, 0);
    gpio_bit_write(GPIOB_BASE, 0x400, 0);
}

void fedl5236_wakeup(void)
{
    volatile uint8_t * const boot_mode = s_test_mode;

    fedl5236_aux_on();
    delay_ms(*boot_mode == 0 ? 100 : 0x14);
    fedl5236_aux_off();
    delay_ms(*boot_mode == 0 ? 0x32 : 10);
    delay_ms(100);
}

/* ----------------------------------------------------------------------- *
 * FEDL5236_PowerDown — OEM FUN_0800df20.
 *
 * The shutdown handshake (cross-checked vs batteryware button_entry_check,
 * which folds the same reg-0x34 charger poll + reg-0x0C bit-7 wait into one
 * loop; powerbankware splits it into named phases and retries the whole thing
 * up to 6×):
 *   Check_Charger_Voltage — pulse reg 8, read the charger voltage (reg 0x34,
 *     scaled raw·19536/1000); loop until it falls to <= 10000 (~10 V). The
 *     fallback 100000 on a failed read keeps it looping.
 *   Check_PUPIN — read reg 0x0C until bit 7 (the PUPIN/power-down-ready flag)
 *     clears.
 *   POWER_DOWN — write reg 0x0C = 0x10. If PA11 is then high the part is down;
 *     optionally persist the BMS record and return. Each phase times out after
 *     250 ticks (-> reset); 6 failed full attempts -> halt.
 *
 * The 0x2000077c tick-flag bits 2/3 are opportunistic "re-print" triggers,
 * cleared as they're seen.
 */
void fedl5236_powerdown(int enable_record)
{
    volatile uint8_t  * const rx    = s_rx;
    volatile uint8_t  * const flags = s_tick;
    uint8_t attempt = 0;

    for (;;) {
        uint8_t  ticks = 0;
        uint32_t charger_mv;

        fedl5236_command_write(8, 1);
        log_print(2, s_check_charger_v);
        uart_flush();
        do {
            delay_ms(10);
            fedl5236_command_write(8, 0x91);
            delay_ms(10);
            charger_mv = 100000;                       /* fallback: read failed */
            if (fedl5236_read_data(0x34, 2) != 0) {
                charger_mv = *(volatile uint16_t *)(rx + 2);
                charger_mv = (19536u * charger_mv) / 1000u;
                if ((*flags & 4) != 0) {
                    *flags &= (uint8_t)~4u;
                    log_print(2, s_charger_v_fmt, charger_mv);
                }
                if (charger_mv <= 9999) {
                    log_print(2, s_charger_v_fmt, charger_mv);
                }
            }
            if (++ticks > 0xf9) {
                log_print(2, s_check_charger_timeout);
                uart_flush();
                system_reset_hang();
            }
            if ((*flags & 8) != 0) {
                *flags &= (uint8_t)~8u;
                log_print(2, s_check_charger_v);
            }
            uart_tx_isr();
        } while (charger_mv > 10000);

        ticks = 0;
        log_print(2, s_check_pupin);
        uart_flush();
        do {
            delay_ms(5);
            if (++ticks > 0xf9) {
                log_print(2, s_check_pupin_timeout);
                uart_flush();
                system_reset_hang();
            }
            rx[2] = 0;
            fedl5236_read_data(0xc, 1);
            if ((*flags & 8) != 0) {
                *flags &= (uint8_t)~8u;
                log_print(2, s_check_pupin);
            }
            uart_tx_isr();
        } while ((rx[2] & 0x80) == 0x80);

        delay_ms(5);
        log_print(2, s_pdown_set_command);
        uart_flush();
        fedl5236_command_write(0xc, 0x10);
        delay_ms(100);

        if (++attempt > 5) {
            break;
        }
        if (gpio_bit_read(GPIOA_BASE, PIN_PA11)) {     /* PA11 high -> down */
            if (enable_record != 0) {
                fedl5236_record_save();
            }
            return;
        }
    }

    log_print(2, s_pdown_over5);
    uart_flush();
    for (;;) {
    }
}

/* ----------------------------------------------------------------------- *
 * fedl5236_record_save — OEM FUN_08013ed8.
 *
 * Persists the 0x80-byte BMS record (SRAM 0x200004d0) to the I2C EEPROM
 * (device 0xa0, memory address 0xff80, 2-byte addressing) and verifies it:
 *   1. Stamp the record CRC into record[+0x7c] (CRC over the first 0x1f bytes).
 *   2. Write the record; retry (servicing the UART) until the write succeeds.
 *   3. Read it back into a scratch buffer; retry until the read succeeds.
 *   4. Compare; if it differs, restart from the write.
 *
 * batteryware persists its records to memory-mapped flash via the SPI
 * write-verify `memcmp_verify`; powerbank uses a discrete I2C EEPROM, so the
 * mechanism is image-specific (the verify loop structure is the same idea).
 */
void fedl5236_record_save(void)
{
    void * const  record = BMS_RECORD;
    void * const  handle = EEPROM_HANDLE;
    uint8_t       scratch[128];

    *(volatile uint32_t *)((uint8_t *)record + 0x7c) = bms_record_crc(record, 0x1f);

    do {
        log_print(2, s_write_bms_record);
        while (eeprom_mem_write(handle, 0xa0, 0xff80, 2, record, 0x80, 6) != 0) {
            delay_ms_service(7);
        }

        do {
            log_print(2, s_check_bms_record);
            delay_ms_service(7);
            mem_zero(scratch, 0x80);
        } while (eeprom_mem_read(handle, 0xa0, 0xff80, 2, scratch, 0x80, 6) != 0);
    } while (!mem_compare(record, scratch, 0x80));
}
