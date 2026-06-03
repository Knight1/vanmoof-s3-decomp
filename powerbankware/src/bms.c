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
 *
 * The bare absolute addresses the OEM scatters through these routines are
 * given names below; each is the SRAM/flash cell documented in
 * docs/hardware.md (and, for the LED/indicator/modbus cells, the file that
 * owns them: timer.c, extend_io.c, modbus.c). Names only — every access keeps
 * the OEM's exact width and offset so the image stays byte-equivalent.
 */

/* ── Peripheral / GPIO bases ─────────────────────────────────────────── */
#define GPIOA_BASE   0x48000000u
#define GPIOB_BASE   0x48000400u
#define PIN_PA7      0x80u
#define PIN_PA9      0x200u
#define PIN_PA10     0x400u
#define PIN_PB0      0x1u
#define PIN_PB8      0x100u
#define PIN_PB9      0x200u
#define PIN_PB11     0x800u

/* ── EEPROM (I2C2) and RTC HAL handles ───────────────────────────────── */
#define EEPROM_HANDLE  ((void *)0x20000434)   /* I2C2 handle, 24-series EEPROM */
#define RTC_HANDLE     ((void *)0x200006f0)

/* ── Flash: image header / version words / SOC table / magics ────────── */
#define FLASH_IMG_HEADER  ((const void *)0x08008000)         /* 28-byte header block  */
#define FLASH_BL_VERSION  (*(volatile uint32_t *)0x08007ff8) /* BL version word       */
#define FLASH_FW_VERSION  (*(volatile uint32_t *)0x08008004) /* image FW version word */
#define SOC_TABLE         ((const volatile uint16_t *)0x0801e620) /* descending V->SOC */
#define FW_MAGIC_1_11_05  0x011105b2u                        /* firmware v1.11.05     */
#define WAKE_DISCHG_EMPTY_Msk  0x01000000u                   /* RTC backup word bit24 */

/* ── BMS record (persisted) + error-log / trace buffer ───────────────── */
#define CFG       ((volatile uint8_t *)0x200004d0)   /* 128-byte BMS record       */
#define ERRLOG    ((volatile uint8_t *)0x200005b0)   /* 64-byte error-log / trace */
#define S_ESN     ((void *)0x2000052e)               /* 14-byte ESN  (record +0x5e) */
#define S_FW_ID   ((void *)0x2000053c)               /* 4-byte fw id (record +0x6c) */
#define S_IMG_HEADER ((void *)0x20000558)            /* image-header copy dest      */
#define S_BUILD_DATE ((const void *)0x20000568)      /* date string in header copy  */
#define S_BUILD_TIME ((const void *)0x20000574)      /* time string in header copy  */

/* ── Live BMS globals (SRAM) ─────────────────────────────────────────── */
static volatile uint16_t * const s_mode          = (volatile uint16_t *)0x200006a0; /* mode/cfg word */
static volatile uint8_t  * const s_state         = (volatile uint8_t  *)0x200005ac; /* current state */
static volatile uint8_t  * const s_idblk         = (volatile uint8_t  *)0x20000724; /* identity/wake word; [1]=saved state */
static volatile uint16_t * const s_min_cell      = (volatile uint16_t *)0x200003d2; /* min cell mV (SOC lookup input) */
static volatile uint8_t  * const s_soc           = (volatile uint8_t  *)0x2000024c; /* wake-up SOC %  */
static volatile uint32_t * const s_remaining_cap = (volatile uint32_t *)0x20000238; /* remaining-cap accumulator */
static volatile uint16_t * const s_chg_cal       = (volatile uint16_t *)0x2000023c; /* CHG current cal */
static volatile uint16_t * const s_dsg_cal       = (volatile uint16_t *)0x2000023e; /* DSG current cal */
static volatile uint16_t * const s_vout_cal      = (volatile uint16_t *)0x20000200; /* Vout offset cal */
static volatile uint16_t * const s_iout_cal      = (volatile uint16_t *)0x20000210; /* Iout offset cal */
static volatile uint8_t  * const s_ts0_cal       = (volatile uint8_t  *)0x2000020e; /* TS0 offset */
static volatile uint8_t  * const s_ts1_cal       = (volatile uint8_t  *)0x2000021b; /* TS1 offset */
static volatile uint8_t  * const s_ts2_cal       = (volatile uint8_t  *)0x20000205; /* TS2 offset */
static volatile uint8_t  * const s_fet_ctrl      = (volatile uint8_t  *)0x20000412; /* AFE reg-9 FET shadow */
static volatile uint32_t * const s_evtlog_en     = (volatile uint32_t *)0x2000072c; /* event-log enable/counter */
static volatile uint8_t  * const s_test_mode     = (volatile uint8_t  *)0x2000069c; /* 0 = normal, else test-mode */
static volatile uint32_t * const s_fw_ver_cache  = (volatile uint32_t *)0x200006e8; /* cached image version */
static volatile uint8_t  * const s_hw_id_lo      = (volatile uint8_t  *)0x200006e9; /* HW id byte */
static volatile uint16_t * const s_hw_id_hi      = (volatile uint16_t *)0x200006ea; /* HW id halfword */
static volatile uint32_t * const s_full_cap_limit= (volatile uint32_t *)0x200005a4;
static volatile uint32_t * const s_cycle_thresh  = (volatile uint32_t *)0x200005a8;
/* indicator-pattern / boot state machine (owned by timer.c, states.c) */
static volatile uint8_t  * const s_ind_patlen    = (volatile uint8_t  *)0x20000480;
static volatile uint8_t  * const s_ind_flags     = (volatile uint8_t  *)0x20000484;
static volatile uint8_t  * const s_ind_repeat    = (volatile uint8_t  *)0x20000485;
static volatile uint8_t  * const s_ind_substate  = (volatile uint8_t  *)0x20000488;
static volatile uint8_t  * const s_ind_pos       = (volatile uint8_t  *)0x20000489;
static volatile uint8_t  * const s_ind_phase     = (volatile uint8_t  *)0x2000048a;
static volatile uint8_t  * const s_fault_class   = (volatile uint8_t  *)0x200004b4; /* boot-mode / fault class latch */
/* LED / extend-IO shadows (owned by extend_io.c) */
static volatile uint8_t  * const s_led_level     = (volatile uint8_t  *)0x200006a4;
static volatile uint8_t  * const s_led_out       = (volatile uint8_t  *)0x200006ec;
static volatile uint8_t  * const s_led_shadow    = (volatile uint8_t  *)0x200006e5;
/* misc cells cleared at init */
static volatile uint16_t * const s_mb_reg28      = (volatile uint16_t *)0x200006a6; /* modbus reg 0x28 shadow */
static volatile uint16_t * const s_softcnt_bc    = (volatile uint16_t *)0x200006bc; /* per-state soft counter */
static volatile uint16_t * const s_modbus_state  = (volatile uint16_t *)0x2000260c; /* per-channel RX state [ch] */

/*
 * bms_soc_preset — OEM FUN_0800a264.
 *
 * Derive the state-of-charge percent and a nominal capacity figure from the
 * descending voltage->SOC table (SOC_TABLE, u16 entries) against the measured
 * value at s_min_cell: scan index 100..1 for the first (highest) entry that is
 * <= the measurement, store it as the SOC (s_soc), and 9700 * idx * 0x90
 * as the capacity (s_remaining_cap). The OEM's staged read-modify-write stores
 * are kept verbatim (volatile). Cross-checked against the OEM machine code.
 */
void bms_soc_preset(void)
{
    const volatile uint16_t *table = SOC_TABLE;
    uint16_t meas = *s_min_cell;
    volatile uint8_t  *soc = s_soc;
    volatile uint32_t *cap = s_remaining_cap;
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
 * fedl5236_record_save, on the error-log buffer at ERRLOG. */
void errlog_erase(int idx)
{
    void * const handle = EEPROM_HANDLE;
    uint16_t     addr   = (uint16_t)(idx << 6);
    uint8_t      scratch[64];

    *(volatile uint32_t *)(ERRLOG + 0x3c) = bms_record_crc((void *)ERRLOG, 0xf);

    do {
        log_print(2, s_write_errlog, idx);
        while (eeprom_mem_write(handle, 0xa0, addr, 2, (void *)ERRLOG, 0x40, 6) != 0) {
            delay_ms_service(7);
        }
        do {
            log_print(2, s_check_errlog);
            delay_ms_service(7);
            mem_zero(scratch, 0x40);
        } while (eeprom_mem_read(handle, 0xa0, addr, 2, scratch, 0x40, 6) != 0);
    } while (!mem_compare((void *)ERRLOG, scratch, 0x40));
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
    mem_copy(esn, S_ESN, 0xe);
    if (*(volatile int8_t *)(CFG + 0x6c) == 0) {
        mem_zero(fw, 4);
    } else {
        mem_copy(fw, S_FW_ID, 4);
    }

    mem_zero((void *)CFG, 0x80);
    mem_zero((void *)ERRLOG, 0x40);

    uint32_t ver = FLASH_FW_VERSION;                           /* image header version */
    *(volatile uint16_t *)(CFG + 0x52) = (uint16_t)(ver >> 16);
    *(volatile uint8_t  *)(CFG + 0x5d) = (uint8_t)(ver >> 8);

    mem_copy(S_ESN, esn, 0xe);
    mem_copy(S_FW_ID, fw, 4);
    *(volatile uint16_t *)(CFG + 0x54) = design_cap;

    /* Calibration / offset cells: valid (901..1099], else default 1000. */
    static const struct { uintptr_t cell; uint8_t cfg_off; } cal[] = {
        { 0x2000023e, 0x56 },   /* s_dsg_cal  -> record +0x56 */
        { 0x2000023c, 0x58 },   /* s_chg_cal  -> record +0x58 */
        { 0x20000200, 0x70 },   /* s_vout_cal -> record +0x70 */
        { 0x20000210, 0x72 },   /* s_iout_cal -> record +0x72 */
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
    volatile uint8_t *ts0 = s_ts0_cal;
    volatile uint8_t *ts1 = s_ts1_cal;
    volatile uint8_t *ts2 = s_ts2_cal;
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

    uint8_t wake_soc = *s_soc;
    log_print(2, s_real_soc, wake_soc);

    if (wake_soc < CFG[0x5a]) {
        if ((int)(CFG[0x5a] - wake_soc) > 9) {
            *(volatile uint32_t *)(CFG + 0x18) = *s_remaining_cap;
            CFG[0x5a] = wake_soc;
        }
    } else if ((int)(wake_soc - CFG[0x5a]) > 9) {
        *(volatile uint32_t *)(CFG + 0x18) = *s_remaining_cap;
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
 * requested boot mode at s_fault_class.
 */
void boot_mode_enter(uint8_t mode)
{
    gpio_pin_cfg_t cfg;
    mem_set(&cfg, 0, 0x14);

    cfg.pin_mask = PIN_PA9; cfg.mode = 1; cfg.pupd = 0; cfg.speed = 3;
    gpio_pin_config((uint32_t *)GPIOA_BASE, &cfg);     /* PA9 output */
    cfg.pin_mask = PIN_PA10; cfg.mode = 0; cfg.pupd = 0;
    gpio_pin_config((uint32_t *)GPIOA_BASE, &cfg);     /* PA10 input  */
    gpio_bit_write(GPIOA_BASE, PIN_PA9, 0);

    *s_led_shadow = 0xff;
    *s_mode &= (uint16_t)~0x1000u;
    *s_state = 5;
    extend_io_update();
    *s_state = 4;
    *s_mode |= 0x800;
    *s_ind_flags = 0;
    *s_mode &= (uint16_t)~0x200u;
    *s_ind_phase = 0;
    *s_ind_pos = 0;
    *s_ind_patlen = 0;
    *s_ind_repeat = 2;
    *s_ind_substate = 0;
    *s_ind_flags |= 1;

    gpio_bit_write(GPIOA_BASE, PIN_PA7, 1);
    gpio_bit_write(GPIOB_BASE, PIN_PB0, 1);
    gpio_bit_write(GPIOB_BASE, PIN_PB11, 0);
    bypass_fet_off();
    gpio_bit_write(GPIOB_BASE, PIN_PB9, 0);
    *s_fet_ctrl = 0;
    fedl5236_command_write(9, 0);
    bms_state_enter(4);
    gpio_bit_write(GPIOA_BASE, PIN_PA9, 0);
    *s_fault_class = mode;
}

/* Enter shipping mode: clear the dispatch bit, drive the FET/discharge GPIOs,
 * reset the AFE and run the LED/state update, then re-arm the output. */
void shipping_enter(void)
{
    log_print(2, s_shipping_mode2);
    uart_flush();

    *s_mode &= (uint16_t)~0x1000u;
    gpio_bit_write(GPIOA_BASE, PIN_PA7, 0);
    gpio_bit_write(GPIOB_BASE, PIN_PB0, 1);
    gpio_bit_write(GPIOB_BASE, PIN_PB11, 0);
    bypass_fet_off();
    gpio_bit_write(GPIOB_BASE, PIN_PB9, 0);
    *s_fet_ctrl = 0;
    fedl5236_command_write(9, 0);
    bms_state_enter(6);
    extend_io_update();

    log_print(2, s_output_dischg_on);
    uart_flush();
    gpio_bit_write(GPIOA_BASE, PIN_PA7, 1);
    gpio_bit_write(GPIOB_BASE, PIN_PB11, 1);
    gpio_bit_write(GPIOA_BASE, PIN_PA9, 0);
}

/* bms_system_init banners (own passes). */
extern const char s_bl_fw_version[], s_date_time[], s_check_poweron_record[],
                  s_wakeup_soc[], s_discharge_empty[], s_adjust_soc[],
                  s_vout_offset[], s_iout_offset[], s_ts0_offset[],
                  s_ts1_offset[], s_ts2_offset[], s_record_ap_state[];
extern const char s_record_read_proc[], s_record_read_fail[], s_record_crc_error[],
                  s_errlog_read_proc[], s_errlog_read_fail[], s_errlog_crc_error[];

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
    *s_evtlog_en = 0;
    *s_mode = 0;
    *s_test_mode = 0;
    s_modbus_state[0] = 0;
    s_modbus_state[1] = 0;
    *s_mb_reg28 = 0;
    *s_softcnt_bc = 0;
    *s_led_level = 0;
    *s_led_out = 0;
    *s_led_shadow = 0;

    bypass_fet_off();

    if (gpio_bit_read(GPIOB_BASE, PIN_PB8) == 0) {     /* PB8 input */
        *s_mode |= 0x100;
        *s_mode |= 0x800;
    } else {
        *s_mode &= 0xf7ffu;                            /* clear bit11 */
    }

    mem_copy(S_IMG_HEADER, FLASH_IMG_HEADER, 0x28);    /* image header */

    /* version block: byte-reverse the 0x08007ff8 word into a 3-char BL string */
    uint32_t hdr = FLASH_BL_VERSION;
    uint8_t ver[4];
    ver[0] = (uint8_t)(hdr >> 24);
    ver[1] = (uint8_t)(hdr >> 16);
    ver[2] = (uint8_t)(hdr >> 8);
    ver[3] = 0;
    *s_fw_ver_cache = FLASH_FW_VERSION;
    log_print(2, s_bl_fw_version, ver, *s_hw_id_hi, *s_hw_id_lo);
    log_print(2, s_date_time, S_BUILD_DATE, S_BUILD_TIME);
    uart_flush();

    fedl5236_initialize();

    *s_full_cap_limit = 0x08535900;
    *s_cycle_thresh   = 0x06a91400;
    log_print(2, s_check_poweron_record);
    uart_flush();

    if (bms_record_load() != 1) {                      /* EEPROM record load + CRC */
        bms_config_reset();
    }

    /* pull persisted fields out of the record for range-checking */
    *s_dsg_cal = *(volatile uint16_t *)(CFG + 0x56);
    *s_chg_cal = *(volatile uint16_t *)(CFG + 0x58);
    *s_vout_cal = *(volatile uint16_t *)(CFG + 0x70);
    *s_iout_cal = *(volatile uint16_t *)(CFG + 0x72);
    *s_ts0_cal = CFG[8];
    *s_ts1_cal = CFG[9];
    *s_ts2_cal = CFG[0xa];

    /* firmware-version change -> refresh stored HW id, reset TS offsets + config */
    if (*s_fw_ver_cache == FW_MAGIC_1_11_05 &&
        (CFG[0x5d] != *s_hw_id_lo ||
         *(volatile uint16_t *)(CFG + 0x52) != *s_hw_id_hi)) {
        CFG[0x5d] = *s_hw_id_lo;
        *(volatile uint16_t *)(CFG + 0x52) = *s_hw_id_hi;
        *s_ts0_cal = 0;
        *s_ts1_cal = 3;
        *s_ts2_cal = 3;
        bms_config_reset();
    }
    if (CFG[0x5d] != *s_hw_id_lo ||
        *(volatile uint16_t *)(CFG + 0x52) != *s_hw_id_hi) {
        CFG[0x5d] = *s_hw_id_lo;
        *(volatile uint16_t *)(CFG + 0x52) = *s_hw_id_hi;
    }

    /* OEM dead clamp: 0x44b < v && v < 0x385 is unsatisfiable (preserved). */
    if ((0x44b < *s_dsg_cal && *s_dsg_cal < 0x385) ||
        (0x44b < *s_chg_cal && *s_chg_cal < 0x385)) {
        *s_dsg_cal = 1000;
        *s_chg_cal = 1000;
        bms_config_reset();
    }
    if ((0x44b < *s_vout_cal && *s_vout_cal < 0x385) ||
        (0x44b < *s_iout_cal && *s_iout_cal < 0x385)) {
        *s_vout_cal = 1000;
        *s_iout_cal = 1000;
        bms_config_reset();
    }

    /* live clamp: a TS offset in 0x15..0xea is corrupt -> restore defaults */
    if ((0x14 < *s_ts0_cal && *s_ts0_cal < 0xeb) ||
        (0x14 < *s_ts1_cal && *s_ts1_cal < 0xeb) ||
        (0x14 < *s_ts2_cal && *s_ts2_cal < 0xeb)) {
        *s_ts0_cal = 0;
        *s_ts1_cal = 3;
        *s_ts2_cal = 3;
        bms_config_reset();
    }

    if (CFG[0x78] == 0 && CFG[0x79] == 0) {
        CFG[0x79] = 0xff;
    }
    if (*(volatile uint16_t *)(CFG + 0x54) == 0) {
        bms_config_reset();
    }

    *(volatile uint32_t *)s_idblk = rtc_backup_read(RTC_HANDLE, 0);   /* RTC BKP0R */
    bms_soc_preset();
    log_print(2, s_wakeup_soc, *s_soc);

    /* SOC high-water debounce: move it when |wake - stored| exceeds 9 */
    {
        uint8_t soc = *s_soc;
        if (soc < CFG[0x5a]) {
            if ((int)((uint8_t)CFG[0x5a] - soc) > 9) {
                *(volatile uint32_t *)(CFG + 0x18) = *s_remaining_cap;
                CFG[0x5a] = soc;
            }
        } else if ((int)(soc - (uint8_t)CFG[0x5a]) > 9) {
            *(volatile uint32_t *)(CFG + 0x18) = *s_remaining_cap;
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
    if ((*(volatile uint32_t *)s_idblk & WAKE_DISCHG_EMPTY_Msk) != 0) {
        CFG[0x5a] = 0;
        *(volatile uint32_t *)(CFG + 0x18) = 0;
        *(volatile uint32_t *)(CFG + 0x20) = 0;
        log_print(2, s_discharge_empty);
    }

    log_print(2, s_adjust_soc, CFG[0x5a]);
    log_print(2, s_vout_offset, *s_vout_cal);
    log_print(2, s_iout_offset, *s_iout_cal);
    log_print(2, s_ts0_offset, CFG[8]);
    log_print(2, s_ts1_offset, CFG[9]);
    log_print(2, s_ts2_offset, CFG[0xa]);
    uart_flush();

    *s_state = s_idblk[1];
    log_print(2, s_record_ap_state, s_idblk[1]);
    mem_zero((void *)ERRLOG, 0x40);

    if (*(volatile int16_t *)(CFG + 0x2a) != 0) {
        /* FUN_0800823c is the unsigned divmod helper; only the remainder is used */
        uint16_t rem = (uint16_t)(*(volatile uint16_t *)(CFG + 0x2a) % 1000u);
        bms_errlog_load((short)((rem - 1) & 0xffff));
    }
}

/*
 * bms_record_load — OEM FUN_08013f80 (BMS_Record_Read_EEPROM_Process).
 *
 * Read the 128-byte BMS record from the I2C EEPROM (device 0xa0, address 0xff80)
 * into the record at CFG and verify its stored CRC (word at +0x7c) against
 * a fresh CRC over the first 0x1f words. Retries up to 10 times on a read failure
 * or CRC mismatch. Returns 1 on success, 0 if the retries are exhausted (the
 * caller then falls back to a factory preset). Cross-checked against the OEM image.
 */
int bms_record_load(void)
{
    uint8_t retries = 0;

    log_print(2, s_record_read_proc);
    uart_flush();

    bool again;
    do {
        again = false;
        mem_zero((void *)CFG, 0x80);
        int rc = eeprom_mem_read(EEPROM_HANDLE, 0xa0, 0xff80, 2,
                                 (void *)CFG, 0x80, 6);
        if (rc == 0) {
            uint32_t stored = *(volatile uint32_t *)(CFG + 0x7c);
            uint32_t calc = bms_record_crc((const void *)CFG, 0x1f);
            if (stored != calc) {
                retries++;
                if (retries < 10) {
                    again = true;
                    log_print(2, s_record_crc_error);
                    uart_flush();
                }
            }
        } else {
            retries++;
            if (retries < 10) {
                again = true;
                log_print(2, s_record_read_fail);
                uart_flush();
            }
        }
    } while (again);

    return retries < 10;
}

/*
 * bms_errlog_load — OEM FUN_08014140 (BMS_ErrorLog_Read_EEPROM_Process).
 *
 * Read one 64-byte error-log record at EEPROM offset `index * 0x40` (device 0xa0)
 * into a stack buffer and verify its stored CRC (word at +0x3c) over the first
 * 0xf words, retrying up to 10 times. On success the validated record is copied
 * to ERRLOG. Returns 1 on success, 0 on exhaustion. The buffer is word-aligned
 * so the +0x3c CRC load is a valid Cortex-M0 word access. Disasm-confirmed.
 */
int bms_errlog_load(short index)
{
    uint32_t buf32[16];                        /* 64 bytes, word-aligned */
    uint8_t *buf = (uint8_t *)buf32;
    uint8_t retries = 0;

    log_print(2, s_errlog_read_proc, index);
    uart_flush();

    uint16_t addr = (uint16_t)(index * 0x40);

    char again;
    do {
        again = 0;
        mem_zero(buf, 0x40);
        int rc = eeprom_mem_read(EEPROM_HANDLE, 0xa0, addr, 2, buf, 0x40, 6);
        if (rc == 0) {
            uint32_t stored = *(uint32_t *)(buf + 0x3c);
            uint32_t calc = bms_record_crc(buf, 0xf);
            if (stored != calc) {
                retries++;
                if (retries < 10) {
                    again = 1;
                    log_print(2, s_errlog_crc_error);
                    uart_flush();
                }
            }
        } else {
            retries++;
            if (retries < 10) {
                again = 1;
                log_print(2, s_errlog_read_fail, index);
                uart_flush();
            }
        }
    } while (again);

    int ok = retries < 10;
    if (ok) {
        block_copy((void *)ERRLOG, buf, 0x40);
    }
    return ok;
}
