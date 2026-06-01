#include "powerbankware.h"

/*
 * BMS state / config routines (the deeper targets of the console commands).
 *
 *   bms_config_reset = OEM FUN_08011b08 ("Preset_BMS_System_Value")
 *   boot_mode_enter  = OEM FUN_0800ede0 (bootloader/upgrade entry, also main)
 *   shipping_enter   = OEM FUN_080113b4 (ship-mode power-down)
 *   errlog_erase     = OEM FUN_08014068 (clear one EEPROM error-log record)
 *
 * Config struct base 0x200004d0 (same struct fedl5236_record_save persists).
 */

#define CFG ((volatile uint8_t *)0x200004d0)
static volatile uint16_t * const s_mode = (volatile uint16_t *)0x200006a0;

/* Deeper sub-functions (own passes). */
extern void FUN_0800a264(void);     /* post-reset value init */

extern const char s_preset_bms[], s_real_soc[], s_shipping_mode2[],
                  s_output_dischg_on[], s_write_errlog[], s_check_errlog[];

/* Clear one 0x40-byte error-log record (index `idx`) and verify it in the
 * I2C EEPROM (addr = idx<<6) — same write/read-back/verify loop as
 * fedl5236_record_save, on the error-log buffer at 0x200005b0. */
void errlog_erase(int idx)
{
    void * const handle = (void *)0x20000434;
    uint16_t     addr   = (uint16_t)(idx << 6);
    uint8_t      scratch[64];

    *(volatile uint32_t *)(0x200005b0 + 0x3c) = bms_record_crc((void *)0x200005b0, 0xf);

    do {
        log_print(2, s_write_errlog, idx);
        while (eeprom_mem_write(handle, 0xa0, addr, 2, (void *)0x200005b0, 0x40, 6) != 0) {
            delay_ms_service(7);
        }
        do {
            log_print(2, s_check_errlog);
            delay_ms_service(7);
            mem_zero(scratch, 0x40);
        } while (eeprom_mem_read(handle, 0xa0, addr, 2, scratch, 0x40, 6) != 0);
    } while (!mem_compare((void *)0x200005b0, scratch, 0x40));
}

/*
 * Reset the BMS configuration to defaults while preserving identity:
 * stash the design capacity / ESN / firmware string, zero the config and
 * error-log buffers, restore identity, re-stamp the image version, then
 * clamp/seed the calibration, offset and thermistor cells. Finally adjust
 * the running SOC from the wakeup SOC and persist.
 */
void bms_config_reset(void)
{
    uint8_t  esn[16];
    uint8_t  fw[4];
    uint16_t design_cap;

    log_print(2, s_preset_bms);

    design_cap = *(volatile uint16_t *)(CFG + 0x54);
    if (design_cap == 0) {
        design_cap = 0x10;
    }
    mem_copy(esn, (void *)0x2000052e, 0xe);
    if (*(volatile int8_t *)(CFG + 0x6c) == 0) {
        mem_zero(fw, 4);
    } else {
        mem_copy(fw, (void *)0x2000053c, 4);
    }

    mem_zero((void *)CFG, 0x80);
    mem_zero((void *)0x200005b0, 0x40);

    uint32_t ver = *(const volatile uint32_t *)0x08008004;     /* image header version */
    *(volatile uint16_t *)(CFG + 0x52) = (uint16_t)(ver >> 16);
    *(volatile uint8_t  *)(CFG + 0x5d) = (uint8_t)(ver >> 8);

    mem_copy((void *)0x2000052e, esn, 0xe);
    mem_copy((void *)0x2000053c, fw, 4);
    *(volatile uint16_t *)(CFG + 0x54) = design_cap;

    /* Calibration / offset cells: valid (901..1099], else default 1000. */
    static const struct { uintptr_t cell; uint8_t cfg_off; } cal[] = {
        { 0x2000023e, 0x56 }, { 0x2000023c, 0x58 },
        { 0x20000200, 0x70 }, { 0x20000210, 0x72 },
    };
    for (unsigned i = 0; i < 4; i++) {
        volatile uint16_t *c = (volatile uint16_t *)cal[i].cell;
        if (*c > 0x44b || *c < 0x385) {
            *c = 1000;
        }
        *(volatile uint16_t *)(CFG + cal[i].cfg_off) = *c;
    }
    *(volatile uint8_t *)(CFG + 0x79) = 0xff;

    /* Thermistor offsets: if any is in (0x14, 0xeb) reset all three. */
    volatile uint8_t *ts0 = (volatile uint8_t *)0x2000020e;
    volatile uint8_t *ts1 = (volatile uint8_t *)0x2000021b;
    volatile uint8_t *ts2 = (volatile uint8_t *)0x20000205;
    if ((*ts0 < 0xeb && *ts0 > 0x14) ||
        (*ts1 < 0xeb && *ts1 > 0x14) ||
        (*ts2 < 0xeb && *ts2 > 0x14)) {
        *ts0 = 0;
        *ts1 = 3;
        *ts2 = 3;
    }
    CFG[8] = *ts0;
    CFG[9] = *ts1;
    CFG[0xa] = *ts2;

    *(volatile uint32_t *)(CFG + 0x1c) = 0x25e4;
    CFG[0x5b] = 100;
    FUN_0800a264();

    uint8_t wake_soc = *(volatile uint8_t *)0x2000024c;
    log_print(2, s_real_soc, wake_soc);

    if (wake_soc < CFG[0x5a]) {
        if ((int)(CFG[0x5a] - wake_soc) > 9) {
            *(volatile uint32_t *)(CFG + 0x18) = *(volatile uint32_t *)0x20000238;
            CFG[0x5a] = wake_soc;
        }
    } else if ((int)(wake_soc - CFG[0x5a]) > 9) {
        *(volatile uint32_t *)(CFG + 0x18) = *(volatile uint32_t *)0x20000238;
        CFG[0x5a] = wake_soc;
    }

    *(volatile uint32_t *)(CFG + 0x20) = *(volatile uint32_t *)(CFG + 0x18);
    if (*(volatile uint32_t *)(CFG + 0x20) > 0x383f) {
        *(volatile uint32_t *)(CFG + 0x20) = *(volatile uint32_t *)(CFG + 0x20) / 0x3840u;
    } else {
        *(volatile uint32_t *)(CFG + 0x20) = 0;
    }

    fedl5236_record_save();
}

/*
 * Enter the bootloader/upgrade mode (`mode` byte). Configures PA9 out / PA10
 * in, drives the FET/charge GPIOs into the safe pattern, resets the AFE
 * (reg 9) and re-arms the state machine via bms_state_enter, then latches the
 * requested boot mode at 0x200004b4.
 */
void boot_mode_enter(uint8_t mode)
{
    gpio_pin_cfg_t cfg;
    mem_set(&cfg, 0, 0x14);

    cfg.pin_mask = 0x200; cfg.mode = 1; cfg.pupd = 0; cfg.speed = 3;
    gpio_pin_config((uint32_t *)0x48000000, &cfg);     /* PA9 output */
    cfg.pin_mask = 0x400; cfg.mode = 0; cfg.pupd = 0;
    gpio_pin_config((uint32_t *)0x48000000, &cfg);     /* PA10 input  */
    gpio_bit_write(0x48000000, 0x200, 0);              /* PA9 = 0 */

    *(volatile uint8_t *)0x200006e5 = 0xff;
    *s_mode &= (uint16_t)~0x1000u;
    *(volatile uint8_t *)0x200005ac = 5;               /* state = 5 */
    extend_io_update();
    *(volatile uint8_t *)0x200005ac = 4;               /* state = 4 */
    *s_mode |= 0x800;
    *(volatile uint8_t *)0x20000484 = 0;
    *s_mode &= (uint16_t)~0x200u;
    *(volatile uint8_t *)0x2000048a = 0;
    *(volatile uint8_t *)0x20000489 = 0;
    *(volatile uint8_t *)0x20000480 = 0;
    *(volatile uint8_t *)0x20000485 = 2;
    *(volatile uint8_t *)0x20000488 = 0;
    *(volatile uint8_t *)0x20000484 |= 1;

    gpio_bit_write(0x48000000, 0x80, 1);               /* PA7 = 1 */
    gpio_bit_write(0x48000400, 1, 1);                  /* PB0 = 1 */
    gpio_bit_write(0x48000400, 0x800, 0);              /* PB11 = 0 */
    bypass_fet_off();
    gpio_bit_write(0x48000400, 0x200, 0);              /* PB9 = 0 */
    *(volatile uint8_t *)0x20000412 = 0;
    fedl5236_command_write(9, 0);
    bms_state_enter(4);
    gpio_bit_write(0x48000000, 0x200, 0);              /* PA9 = 0 */
    *(volatile uint8_t *)0x200004b4 = mode;
}

/* Enter shipping mode: clear the dispatch bit, drive the FET/discharge GPIOs,
 * reset the AFE and run the LED/state update, then re-arm the output. */
void shipping_enter(void)
{
    log_print(2, s_shipping_mode2);
    uart_flush();

    *s_mode &= (uint16_t)~0x1000u;
    gpio_bit_write(0x48000000, 0x80, 0);               /* PA7 = 0 */
    gpio_bit_write(0x48000400, 1, 1);                  /* PB0 = 1 */
    gpio_bit_write(0x48000400, 0x800, 0);              /* PB11 = 0 */
    bypass_fet_off();
    gpio_bit_write(0x48000400, 0x200, 0);              /* PB9 = 0 */
    *(volatile uint8_t *)0x20000412 = 0;
    fedl5236_command_write(9, 0);
    bms_state_enter(6);
    extend_io_update();

    log_print(2, s_output_dischg_on);
    uart_flush();
    gpio_bit_write(0x48000000, 0x80, 1);               /* PA7 = 1 */
    gpio_bit_write(0x48000400, 0x800, 1);              /* PB11 = 1 */
    gpio_bit_write(0x48000000, 0x200, 0);              /* PA9 = 0 */
}
