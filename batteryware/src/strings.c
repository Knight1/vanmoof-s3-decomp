/* strings.c — OEM rodata strings from batteryware_1.17.1.bin.
 *
 * Two contiguous rodata regions in the OEM image carry every textual
 * literal the firmware uses:
 *
 *   0x08011f97 .. 0x08012431  printf-style debug/log format strings,
 *                             emitted by `uart_printf` from `fg_scan`,
 *                             `bms_state_machine`, the various
 *                             `state_handler_*` routines, etc.
 *
 *   0x08012b9e .. 0x08012fab  command tokens consumed by the
 *                             `command_parser` dispatch table.
 *                             Some tokens appear in pairs ("FOO" + "FOO?")
 *                             — the trailing-`?` form is the read/query
 *                             variant of the bare set/action form.
 *
 * The OEM build does NOT pool duplicate literals (the strings
 * "I am VanMoof AP", "Temp_Log = %i" and the eight-%x hex line each
 * appear twice at distinct addresses). Preserve that by giving each
 * occurrence its own storage.
 */

#include "batteryware.h"

/* ---- Log / printf format strings (0x08011f97 .. 0x08012431) ---------- */

const char s_chg_cal_ok[]            = " \nCHG CAL OK\r";                  /* 0x08011f97 */
const char s_dsg_cal_ok[]            = "\nDSG CAL OK\r";                   /* 0x08011fa8 */
const char s_fedl5236_init[]         = "\nFEDL5236_Initialize()\r";        /* 0x08011fb8 */
const char s_chargeon_off_pb9_0[]    = "\nChargeOn_Off() --> PB9=0\r";     /* 0x08011fd0 */
const char s_pdown_set_command[]     = "\nFEDL5236_POWER_DOWN_Set_Command()\r"; /* 0x08011fec */
const char s_pdown_error[]           = "\nFEDL5236_POWER_DOWN_Error()\r";  /* 0x08012010 */
const char s_pdown_finish[]          = "\nFEDL5236_POWER_DOWN_Finish()\r"; /* 0x08012030 */
const char s_i_am_vanmoof_ap_lead[]  = " \nI am VanMoof AP\r";             /* 0x08012073 */
const char s_mos_failure_mode[]      = "\nMOS Failure Mode\r";             /* 0x08012088 */
const char s_dp_mode[]               = "\nDP Mode\r";                      /* 0x0801209c */
const char s_vanmoof_mode[]          = "\nVanMoof Mode\r";                 /* 0x080120a8 */
const char s_power_on_uvp2_mode[]    = "\nPower On UVP2 Mode\r";           /* 0x080120b8 */
const char s_power_on_uvp1_mode[]    = "\nPower On UVP1 Mode\r";           /* 0x080120d0 */
const char s_power_on_ovp2_mode[]    = "\nPower On OVP2 Mode\r";           /* 0x080120e8 */
const char s_power_on_ovp1_mode[]    = "\nPower On OVP1 Mode\r";           /* 0x08012100 */
const char s_pwron_detect_uvp2[]     = "\nPower On Detect UVP2 Mode\r";    /* 0x08012118 */
const char s_pwron_detect_uvp1[]     = "\nPower On Detect UVP1 Mode\r";    /* 0x08012134 */
const char s_pwron_detect_ovp2[]     = "\nPower On Detect OVP2 Mode\r";    /* 0x08012150 */
const char s_pwron_detect_ovp1[]     = "\nPower On Detect OVP1 Mode\r";    /* 0x0801216c */
const char s_predischg_lock_ctr[]    = "\nPreDischargeLockCounter = %d\r"; /* 0x08012188 */
const char s_ui_timeout[]            = "\nUserInterfaceTimeout\r";         /* 0x080121a8 */
const char s_shipping_mode[]         = "\nShipping Mode\r";                /* 0x080121c0 */
const char s_pdown_start[]           = "\nFEDL5236_PowerDown_Start()\r";   /* 0x080121d0 */
const char s_max_cell_voltage[]      = "\nFEDL5236_Max_Cell_Voltage = %i\r"; /* 0x080121f0 */
const char s_min_cell_voltage[]      = "\nFEDL5236_Min_Cell_Voltage = %i\r"; /* 0x08012214 */
const char s_total_voltage[]         = "\nFEDL5236_Total_Voltage = %i\r";  /* 0x08012238 */
const char s_real_rsoc[]             = "\nReal_RSOC = %d\r";               /* 0x08012258 */
const char s_record_soc[]            = "\nRecord_SOC = %d\r";              /* 0x0801226c */
const char s_rsoc_adjust[]           = "\nRSOC_Adjust(%d)\r";              /* 0x08012280 */
const char s_i_am_vanmoof_ap[]       = "\nI am VanMoof AP\r";              /* 0x08012294 */
const char s_chg_cal_eq_pct_i[]      = "\nCHG CAL = %i\r";                 /* 0x080122a8 */
const char s_dsg_cal_eq_pct_i[]      = "\nDSG CAL = %i\r";                 /* 0x080122b8 */
const char s_fmt_bl_banner[]         = "\nBL --> %s %s %d%d%d\r";          /* 0x080122c8 */
const char s_fmt_ap_banner[]         = "\nAP --> %s %s %w\r";              /* 0x080122e0 */
const char s_chg_cal_eq_i_compact[]  = "\nCHG CAL=%i\r";                   /* 0x080122fc */
const char s_dsg_cal_eq_i_compact[]  = "\nDSG CAL=%i\r";                   /* 0x0801230c */
const char s_wait_log_clear[]        = "\nWait Log Clear .....\r";         /* 0x0801231c */
const char s_ts0_offset[]            = "\nTS0 Offset = %d\r";              /* 0x08012334 */
const char s_ts1_offset[]            = "\nTS1 Offset = %d\r";              /* 0x08012348 */
const char s_ts2_offset[]            = "\nTS2 Offset = %d\r";              /* 0x0801235c */
const char s_log_clear_ok[]          = "\nLog Clear OK\r";                 /* 0x08012370 */
const char s_temp_log_a[]            = "\nTemp_Log = %i\r";                /* 0x08012380 */
const char s_fmt_hex8_a[]            = "\n%x, %x, %x, %x, %x, %x, %x, %x\r"; /* 0x08012390 */
const char s_new_bootloader_crc[]    = "\nNew BootLoader CRC = %l\r";      /* 0x080123b4 */
const char s_flash_write_count[]     = "\nFlashWriteDataCount = %l\r";     /* 0x080123d0 */
const char s_temp_log_b[]            = "\nTemp_Log = %i\r";                /* 0x080123ec */
const char s_fmt_hex8_b[]            = "\n%x, %x, %x, %x, %x, %x, %x, %x\r"; /* 0x080123fc */
const char s_arrow_ok[]              = "\n----------> OK\r";               /* 0x08012420 */

/* ---- Command parser tokens (0x08012b9e .. 0x08012fab) ---------------- */

/* Command-table tokens. The OEM packs each as `{idx, name_len, name}`
 * inside a 47-byte slot starting at 0x08012b9c — what looked like
 * `\t`/`\n`/`\r` prefixes on a few entries during the initial scan
 * was actually the `name_len` byte (9 = `\t`, 10 = `\n`, 13 = `\r`).
 * Names below are the raw name bytes only; the address comment points
 * at the first character (i.e. entry_base + 2). */
const char cmd_who[]             = "Who?";             /* 0x08012b9e */
const char cmd_now[]             = "Now?";             /* 0x08012bcd */
const char cmd_pf[]              = "PF";               /* 0x08012bfc */
const char cmd_reset_bms[]       = "Reset BMS";        /* 0x08012c2b */
const char cmd_df[]              = "DF";               /* 0x08012c5a */
const char cmd_upgrade_ap[]      = "Upgrade AP";       /* 0x08012c89 */
const char cmd_upgrade_bl[]      = "Upgrade BL";       /* 0x08012cb8 */
const char cmd_into_bootloader[] = "Into BootLoader";  /* 0x08012ce7 */
const char cmd_chg_cal_set[]     = "CHG CAL";          /* 0x08012d16 */
const char cmd_chg_cal_get[]     = "CHG CAL?";         /* 0x08012d45 */
const char cmd_dsg_cal_set[]     = "DSG CAL";          /* 0x08012d74 */
const char cmd_dsg_cal_get[]     = "DSG CAL?";         /* 0x08012da3 */
const char cmd_reset_esn[]       = "Reset ESN";        /* 0x08012dd2 */
const char cmd_log_clear[]       = "Log Clear";        /* 0x08012e01 */
const char cmd_ts0_set[]        = "TS0";               /* 0x08012e30 */
const char cmd_ts1_set[]        = "TS1";               /* 0x08012e5f */
const char cmd_ts2_set[]        = "TS2";               /* 0x08012e8e */
const char cmd_ts0_get[]        = "TS0?";              /* 0x08012ebd */
const char cmd_ts1_get[]        = "TS1?";              /* 0x08012eec */
const char cmd_ts2_get[]        = "TS2?";              /* 0x08012f1b */
const char cmd_ts_reset[]       = "TS Reset";          /* 0x08012f4a */
const char cmd_fcc[]            = "FCC";               /* 0x08012f79 */
const char cmd_soc[]            = "SOC";               /* 0x08012fa8 */
