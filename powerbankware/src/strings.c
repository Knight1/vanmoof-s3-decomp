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

/* Extend_IO (LED-bar I/O expander) SPI error banners (src/extend_io.c). */
const char s_extio_busy[]    = "\nExtend_IO_Process()--> SPI Busy\r";    /* 0x0801E484 */
const char s_extio_retry[]   = "\nExtend_IO_Process()--> Retry Error\r"; /* 0x0801E4A8 */
const char s_extio_timeout[] = "\nExtend_IO_Process()--> SPI Timeout\r"; /* 0x0801E4D0 */
