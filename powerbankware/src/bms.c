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

/*
 * bms_soc_preset — OEM FUN_0800a264.
 *
 * Derive the state-of-charge percent and a nominal capacity figure from the
 * descending voltage->SOC table (0x0801e620, u16 entries) against the measured
 * value at 0x200003d2: scan index 100..1 for the first (highest) entry that is
 * <= the measurement, store it as the SOC (0x2000024c), and 9700 * idx * 0x90 as
 * the capacity (0x20000238). The OEM's staged read-modify-write stores are kept
 * verbatim (volatile). Cross-checked against the OEM machine code.
 */
void bms_soc_preset(void)
{
    const volatile uint16_t *table = (const volatile uint16_t *)0x0801e620;
    uint16_t meas = *(volatile uint16_t *)0x200003d2;
    volatile uint8_t  *soc = (volatile uint8_t  *)0x2000024c;
    volatile uint32_t *cap = (volatile uint32_t *)0x20000238;
    uint8_t idx = 0;

    for (uint8_t i = 100; i != 0; i--) {
        if (table[i] <= meas) {
            idx = i;
            break;
        }
    }

    *soc = 0;
    *soc = (uint8_t)(*soc + idx);
    *cap = 0;
    *cap = *cap + 9700;
    *cap = *cap * idx;
    *cap = *cap * 0x90;
}

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
    bms_soc_preset();

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

/* bms_system_init banners + deeper leaves (own passes). */
extern const char s_bl_fw_version[], s_date_time[], s_check_poweron_record[],
                  s_wakeup_soc[], s_discharge_empty[], s_adjust_soc[],
                  s_vout_offset[], s_iout_offset[], s_ts0_offset[],
                  s_ts1_offset[], s_ts2_offset[], s_record_ap_state[];
extern int      FUN_08013f80(void);                 /* load + CRC-verify BMS record (EEPROM) */
extern int      FUN_08014140(short idx);            /* errlog record read/verify by index */

/*
 * bms_system_init — OEM FUN_0801156c. Secondary init, run from main() right after
 * hal_bringup().
 *
 * Clears the volatile run-state, reports the BL/FW version (flash header at
 * 0x08007ff8 / 0x08008004), brings up the FEDL5236 AFE, loads + CRC-checks the
 * persisted BMS record from EEPROM (falling back to a factory preset on any load
 * failure, version change, or out-of-range field), reads the wake reason from RTC
 * backup, derives the wake-up SOC, and logs the restored calibration.
 *
 * Cross-checked against the OEM machine code. Two capacity clamps (record +0x56/
 * +0x58 and +0x70/+0x72) test 0x44b < v && v < 0x385, which can never hold, so
 * those resets are dead in the OEM — reproduced verbatim. The temperature-offset
 * clamp (record +8/+9/+0xa, corrupt when 0x14 < t < 0xeb) is live.
 */
void bms_system_init(void)
{
    *(volatile uint32_t *)0x2000072c = 0;
    *s_mode = 0;                                       /* 0x200006a0 */
    *(volatile uint8_t  *)0x2000069c = 0;
    *(volatile uint16_t *)0x2000260c = 0;
    *(volatile uint16_t *)(0x2000260c + 2) = 0;
    *(volatile uint16_t *)0x200006a6 = 0;
    *(volatile uint16_t *)0x200006bc = 0;
    *(volatile uint8_t  *)0x200006a4 = 0;
    *(volatile uint8_t  *)0x200006ec = 0;
    *(volatile uint8_t  *)0x200006e5 = 0;

    bypass_fet_off();

    if (gpio_bit_read(0x48000400u, 0x100) == 0) {      /* PB8 input */
        *s_mode |= 0x100;
        *s_mode |= 0x800;
    } else {
        *s_mode &= 0xf7ffu;                            /* clear bit11 */
    }

    mem_copy((void *)0x20000558, (const void *)0x08008000, 0x28);   /* image header */

    /* version block: byte-reverse the 0x08007ff8 word into a 3-char BL string */
    uint32_t hdr = *(volatile uint32_t *)0x08007ff8;
    uint8_t ver[4];
    ver[0] = (uint8_t)(hdr >> 24);
    ver[1] = (uint8_t)(hdr >> 16);
    ver[2] = (uint8_t)(hdr >> 8);
    ver[3] = 0;
    *(volatile uint32_t *)0x200006e8 = *(volatile uint32_t *)0x08008004;
    log_print(2, s_bl_fw_version, ver,
              *(volatile uint16_t *)0x200006ea, *(volatile uint8_t *)0x200006e9);
    log_print(2, s_date_time, (const void *)0x20000568, (const void *)0x20000574);
    uart_flush();

    fedl5236_initialize();

    *(volatile uint32_t *)0x200005a4 = 0x08535900;
    *(volatile uint32_t *)0x200005a8 = 0x06a91400;
    log_print(2, s_check_poweron_record);
    uart_flush();

    if (FUN_08013f80() != 1) {                         /* EEPROM record load + CRC */
        bms_config_reset();
    }

    /* pull persisted fields out of the record for range-checking */
    *(volatile uint16_t *)0x2000023e = *(volatile uint16_t *)(CFG + 0x56);
    *(volatile uint16_t *)0x2000023c = *(volatile uint16_t *)(CFG + 0x58);
    *(volatile uint16_t *)0x20000200 = *(volatile uint16_t *)(CFG + 0x70);
    *(volatile uint16_t *)0x20000210 = *(volatile uint16_t *)(CFG + 0x72);
    *(volatile uint8_t  *)0x2000020e = CFG[8];
    *(volatile uint8_t  *)0x2000021b = CFG[9];
    *(volatile uint8_t  *)0x20000205 = CFG[0xa];

    /* firmware-version change -> refresh stored HW id, reset TS offsets + config */
    if (*(volatile uint32_t *)0x200006e8 == 0x011105b2 &&
        (CFG[0x5d] != *(volatile uint8_t *)0x200006e9 ||
         *(volatile uint16_t *)(CFG + 0x52) != *(volatile uint16_t *)0x200006ea)) {
        CFG[0x5d] = *(volatile uint8_t *)0x200006e9;
        *(volatile uint16_t *)(CFG + 0x52) = *(volatile uint16_t *)0x200006ea;
        *(volatile uint8_t *)0x2000020e = 0;
        *(volatile uint8_t *)0x2000021b = 3;
        *(volatile uint8_t *)0x20000205 = 3;
        bms_config_reset();
    }
    if (CFG[0x5d] != *(volatile uint8_t *)0x200006e9 ||
        *(volatile uint16_t *)(CFG + 0x52) != *(volatile uint16_t *)0x200006ea) {
        CFG[0x5d] = *(volatile uint8_t *)0x200006e9;
        *(volatile uint16_t *)(CFG + 0x52) = *(volatile uint16_t *)0x200006ea;
    }

    /* OEM dead clamp: 0x44b < v && v < 0x385 is unsatisfiable (preserved). */
    if ((0x44b < *(volatile uint16_t *)0x2000023e && *(volatile uint16_t *)0x2000023e < 0x385) ||
        (0x44b < *(volatile uint16_t *)0x2000023c && *(volatile uint16_t *)0x2000023c < 0x385)) {
        *(volatile uint16_t *)0x2000023e = 1000;
        *(volatile uint16_t *)0x2000023c = 1000;
        bms_config_reset();
    }
    if ((0x44b < *(volatile uint16_t *)0x20000200 && *(volatile uint16_t *)0x20000200 < 0x385) ||
        (0x44b < *(volatile uint16_t *)0x20000210 && *(volatile uint16_t *)0x20000210 < 0x385)) {
        *(volatile uint16_t *)0x20000200 = 1000;
        *(volatile uint16_t *)0x20000210 = 1000;
        bms_config_reset();
    }

    /* live clamp: a TS offset in 0x15..0xea is corrupt -> restore defaults */
    if ((0x14 < *(volatile uint8_t *)0x2000020e && *(volatile uint8_t *)0x2000020e < 0xeb) ||
        (0x14 < *(volatile uint8_t *)0x2000021b && *(volatile uint8_t *)0x2000021b < 0xeb) ||
        (0x14 < *(volatile uint8_t *)0x20000205 && *(volatile uint8_t *)0x20000205 < 0xeb)) {
        *(volatile uint8_t *)0x2000020e = 0;
        *(volatile uint8_t *)0x2000021b = 3;
        *(volatile uint8_t *)0x20000205 = 3;
        bms_config_reset();
    }

    if (CFG[0x78] == 0 && CFG[0x79] == 0) {
        CFG[0x79] = 0xff;
    }
    if (*(volatile uint16_t *)(CFG + 0x54) == 0) {
        bms_config_reset();
    }

    *(volatile uint32_t *)0x20000724 = rtc_backup_read((void *)0x200006f0, 0);   /* RTC BKP0R */
    bms_soc_preset();
    log_print(2, s_wakeup_soc, *(volatile uint8_t *)0x2000024c);

    /* SOC high-water debounce: move it when |wake - stored| exceeds 9 */
    {
        uint8_t soc = *(volatile uint8_t *)0x2000024c;
        if (soc < CFG[0x5a]) {
            if ((int)((uint8_t)CFG[0x5a] - soc) > 9) {
                *(volatile uint32_t *)(CFG + 0x18) = *(volatile uint32_t *)0x20000238;
                CFG[0x5a] = soc;
            }
        } else if ((int)(soc - (uint8_t)CFG[0x5a]) > 9) {
            *(volatile uint32_t *)(CFG + 0x18) = *(volatile uint32_t *)0x20000238;
            CFG[0x5a] = soc;
        }
    }

    *(volatile uint32_t *)(CFG + 0x20) = *(volatile uint32_t *)(CFG + 0x18);
    if (*(volatile uint32_t *)(CFG + 0x20) > 0x383f) {
        *(volatile uint32_t *)(CFG + 0x20) = *(volatile uint32_t *)(CFG + 0x20) / 0x3840u;
    } else {
        *(volatile uint32_t *)(CFG + 0x20) = 0;
    }

    /* wake-reason bit24 of the RTC backup word -> "discharge empty" recovery */
    if ((*(volatile uint32_t *)0x20000724 & 0x01000000u) != 0) {
        CFG[0x5a] = 0;
        *(volatile uint32_t *)(CFG + 0x18) = 0;
        *(volatile uint32_t *)(CFG + 0x20) = 0;
        log_print(2, s_discharge_empty);
    }

    log_print(2, s_adjust_soc, CFG[0x5a]);
    log_print(2, s_vout_offset, *(volatile uint16_t *)0x20000200);
    log_print(2, s_iout_offset, *(volatile uint16_t *)0x20000210);
    log_print(2, s_ts0_offset, CFG[8]);
    log_print(2, s_ts1_offset, CFG[9]);
    log_print(2, s_ts2_offset, CFG[0xa]);
    uart_flush();

    *(volatile uint8_t *)0x200005ac = *(volatile uint8_t *)0x20000725;
    log_print(2, s_record_ap_state, *(volatile uint8_t *)0x20000725);
    mem_zero((void *)0x200005b0, 0x40);

    if (*(volatile int16_t *)(CFG + 0x2a) != 0) {
        /* FUN_0800823c is the unsigned divmod helper; only the remainder is used */
        uint16_t rem = (uint16_t)(*(volatile uint16_t *)(CFG + 0x2a) % 1000u);
        FUN_08014140((short)((rem - 1) & 0xffff));
    }
}
