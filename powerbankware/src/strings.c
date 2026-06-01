#include "powerbankware.h"

/*
 * Flash log/banner strings, pinned by content from the OEM image. The linker
 * folds .rodata into .text (see linker_stm32f091.ld), so these live in flash
 * like the originals; code references them by symbol.
 *
 * Only the strings reached by already-decompiled code are materialised here;
 * more are added as functions land. OEM addresses are noted for traceability.
 */

/* main() — entry banner + the idblk[2] "Record <fault> Mode" factory entries */
const char s_msg_iam_ap[]      = "\nI am VM-BATT AP\r";        /* 0x0801E05C */
const char s_msg_rec_mosfail[] = "\nRecord MOS Failure Mode\r"; /* 0x0801E070 (sel 0x17) */
const char s_msg_rec_ov2[]     = "\nRecord OV 2nd Mode\r";     /* 0x0801E08C (sel 0x18) */
const char s_msg_rec_cotp[]    = "\nRecord COTP Mode\r";       /* 0x0801E0A4 (sel 0x12) */
const char s_msg_rec_cutp[]    = "\nRecord CUTP Mode\r";       /* 0x0801E0B8 (sel 0x13) */
const char s_msg_rec_dotp[]    = "\nRecord DOTP Mode\r";       /* 0x0801E0CC (sel 0x14) */
const char s_msg_rec_dutp[]    = "\nRecord DUTP Mode\r";       /* 0x0801E0E0 (sel 0x15) */

/* FEDL5236 SPI driver error banners (src/fedl5236.c). The "FFEDL5236"
 * double-F in the Command_Write strings is an OEM typo, reproduced verbatim. */
const char s_fedl_cmd_busy[]    = "\nFFEDL5236_Command_Write()--> SPI Busy\r";     /* 0x0801DD2C */
const char s_fedl_cmd_ng[]      = "\nFFEDL5236_Command_Write()--> HAL_NG Error\r"; /* 0x0801DD54 */
const char s_fedl_cmd_timeout[] = "\nFFEDL5236_Command_Write()--> SPI Timeout\r";  /* 0x0801DD80 */
const char s_fedl_rd_busy[]     = "\nFEDL5236_Read_Data()--> SPI Busy\r";          /* 0x0801DDAC */
const char s_fedl_rd_ng[]       = "\nFEDL5236_Read_Data()--> HAL_NG Error\r";      /* 0x0801DDD0 */
const char s_fedl_rd_retry[]    = "\nFEDL5236_Read_Data()--> Retry Error\r";       /* 0x0801DDF8 */

/* FEDL5236_Initialize stage banners (src/fedl5236.c). */
const char s_fedl_init[]        = "\nFEDL5236_Initialize()\r";          /* 0x0801DE20 */
const char s_fedl_default[]     = "\nFEDL5236_Default_Setting()\r";     /* 0x0801DE38 */
const char s_fedl_zero_offset[] = "\nFEDL5236_Zero_Current_Offset()\r"; /* 0x0801DE58 */
const char s_fedl_tv_check[]    = "\nFEDL5236_Total_Voltage_Check()\r"; /* 0x0801DE7C */
const char s_fedl_charger_v[]   = "\nFEDL5236_Charger_Voltage = %l\r";  /* 0x0801DEA0 */
const char s_fedl_total_v[]     = "\nFEDL5236_Total_Voltage = %i\r";    /* 0x0801DEC0 */
const char s_fedl_ts_error[]    = "\nFEDL5236 TS Error()\r";            /* 0x0801DEE0 */
const char s_fedl_pd_start[]    = "\nFEDL5236_PowerDown_Start()\r";     /* 0x0801DEF8 */
const char s_fedl_pd_finish[]   = "\nFEDL5236_PowerDown_Finish()\r";    /* 0x0801DF18 */

/* FEDL5236_PowerDown stage banners (src/fedl5236.c). */
const char s_check_charger_v[]       = "\nCheck_Charger_Voltage()\r";          /* 0x0801DF38 */
const char s_charger_v_fmt[]         = "\nCharger_Voltage = %l\r";             /* 0x0801DF54 */
const char s_check_charger_timeout[] = "\nCheck_Charger_Voltage_TimeOut()\r";  /* 0x0801DF6C */
const char s_check_pupin[]           = "\nCheck_PUPIN()\r";                    /* 0x0801DF90 */
const char s_check_pupin_timeout[]   = "\nCheck_PUPIN_TimeOut()\r";            /* 0x0801DFA0 */
const char s_pdown_set_command[]     = "\nFEDL5236_POWER_DOWN_Set_Command()\r"; /* 0x0801DFB8 */
const char s_pdown_over5[]           = "\nFEDL5236_PowerDown() over 5 times\r"; /* 0x0801DFDC */

/* BMS-record EEPROM persistence banners (src/fedl5236.c). */
const char s_write_bms_record[] = "\nWrite BMS Record!!\r"; /* 0x0801E35C */
const char s_check_bms_record[] = "\nCheck BMS Record!!\r"; /* 0x0801E374 */

/* OTA image-copy banners (src/ota.c). */
const char s_copy_shadow_to_ap[] = "\nCopy Shadow to AP";  /* 0x0801E120 */
const char s_copy_ap_to_shadow[] = "\nCopy AP to Shadow";  /* 0x0801E134 */
const char s_copy_shadow_to_bl[] = "\nCopy Shadow to BL";  /* 0x0801E148 */
const char s_copy_done[]         = " --> Done\r";          /* 0x0801E15C */

/* BMS coulomb-counter calibration events (src/state_handlers.c). */
const char s_chg_cal_ok[] = "\nCHG CAL OK\r"; /* 0x0801DD0C */
const char s_dsg_cal_ok[] = "\nDSG CAL OK\r"; /* 0x0801DD1C */

/* BMS state/config (src/bms.c). */
const char s_preset_bms[]       = "\nPreset_BMS_System_Value()\r"; /* 0x0801E330 */
const char s_real_soc[]         = "\nReal SOC = %d\r";            /* 0x0801E34C */
const char s_shipping_mode2[]   = "\nShipping Mode\r";           /* 0x0801E200 */
const char s_output_dischg_on[] = "\nOutput_Discharge_On()\r";   /* 0x0801E210 */
const char s_write_errlog[]     = "\nWrite BMS ErrorLog -> %i!!\r"; /* 0x0801E3E0 */
const char s_check_errlog[]     = "\nCheck BMS ErrorLog!!\r";    /* 0x0801E400 */

/* Console command responses (src/cmd.c). */
const char s_iam_ap2[]      = "\nI am VM-BATT AP\r";              /* 0x0801E4F8 */
const char s_done[]         = "\nDone\r";                        /* 0x0801E50C */
const char s_ok[]           = "\nOK\r";                          /* 0x0801E514 */
const char s_chg_cal_q[]    = "\nCHG CAL=%i\r";                  /* 0x0801E51C */
const char s_dsg_cal_q[]    = "\nDSG CAL=%i\r";                  /* 0x0801E52C */
const char s_write_rtc[]    = "\nWrite RTC = %d-%d-%d %d:%d:%d\r"; /* 0x0801E53C */
const char s_charger_v_mv[] = "\nCharger Voltage = %i mV\r";     /* 0x0801E55C */

/* Extend_IO (LED-bar I/O expander) SPI error banners (src/extend_io.c). */
const char s_extio_busy[]    = "\nExtend_IO_Process()--> SPI Busy\r";    /* 0x0801E484 */
const char s_extio_retry[]   = "\nExtend_IO_Process()--> Retry Error\r"; /* 0x0801E4A8 */
const char s_extio_timeout[] = "\nExtend_IO_Process()--> SPI Timeout\r"; /* 0x0801E4D0 */

/* Per-state super-loop handler log strings (src/states.c). */
const char s_no_load_d2[]            = "\nNo Load -> %d\r";                    /* 0x0801DB58 */
const char s_no_load_bypass_off[]    = "\nNo Load -> ByPass Off\r";           /* 0x0801DB68 */
const char s_charger_absent[]        = "\nCharger Absent\r";                  /* 0x0801DB80 */
const char s_charger_voltage_l[]     = "\nCharger_Voltage = %l\r";            /* 0x0801DBF4 */
const char s_charger_load_exist[]    = "\nCharger In & Load Exist -> ByPass On\r";   /* 0x0801DC0C */
const char s_charger_load_absent[]   = "\nCharger In & Load Absent -> ByPass Off\r"; /* 0x0801DC34 */
const char s_no_load_d[]             = "\nNo Load -> %d\r";                   /* 0x0801DC60 */
const char s_dd_on[]                 = "\nDD On\r";                           /* 0x0801DC70 */
const char s_vout_under_20v_3s[]     = "\nVout <20V over 3Sec\r";             /* 0x0801DC78 */
const char s_dac_over_range[]        = "\nDAC Over Range!!\r";                /* 0x0801DC90 */
const char s_vout_under_30v_30min[]  = "\nVout <30V over 30min.\r";           /* 0x0801DCA4 */
const char s_state6_charge_mode[]    = "\nShipping_Mode_Process() --> KeyPressed\r"; /* 0x0801E168 */
const char s_state6_charger_present[]= "\nCharger Plugin\r";                  /* 0x0801E194 */
const char s_state6_powerdown_a[]    = "\nFEDL5236_PowerDown_Start()\r";      /* 0x0801E1A8 */
const char s_state6_powerdown_b[]    = "\nFEDL5236_PowerDown_Finish()\r";     /* 0x0801E1C8 */
const char s_state6_progress[]       = "\nShipping Count -> %d\r";            /* 0x0801E1E8 */

/* Power-path output stage (src/vout.c). */
const char s_dac_stop[]  = "\nDAC_Stop()\r";       /* 0x0801DBD0 */
const char s_dac_value[] = "\nDAC_Value= %l mV\r"; /* 0x0801DBE0 */

/* Boot operating-entry bypass states (src/transitions.c). */
const char s_bypass_on[]           = "\nByPass On\r";                  /* 0x0801DB94 */
const char s_bypass_off_batt_low[] = "\nByPass Off --> Battery Low\r"; /* 0x0801DBA0 */
const char s_bypass_off[]          = "\nByPass Off\r";                 /* 0x0801DBC0 */

/* Charger-attach AFE refresh (src/charger.c). */
const char s_fedl_proc_busy[]    = "\nFEDL5236_Process()--> SPI Busy\r";          /* 0x0801DCBC */
const char s_fedl_proc_fet_err[] = "\nFEDL5236_Process()--> FET Control Error\r"; /* 0x0801DCE0 */

/* Periodic status frame (src/telemetry.c). */
const char s_send_sn[] = "\nSend SN = %x %x %x %x %x, Version = %w, SOC = %d, SOH = %d, CycleCount = %i, State = %x\r"; /* 0x0801E000 */

/* State-machine transition trace (src/dispatch.c). */
const char s_new_state[]  = "\nNew State = %d\r";      /* 0x0801E0F4 */
const char s_prev_state[] = "\nPrevious State = %d\r"; /* 0x0801E108 */
