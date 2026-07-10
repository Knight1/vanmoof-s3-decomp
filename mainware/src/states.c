#include <stdint.h>

#include "states.h"
#include "app.h"        /* maybe_get/set_bike_state, channel_notify_*, announce_mark, aux/set mode, reset_dual_buffers_and_flags */
#include "log.h"        /* g_log_func, log_print_timestamp_prefix */
#include "scheduler.h"  /* scheduler_alloc/release/start/slot_is_idle */
#include "watchdog.h"   /* watchdog_kick */
#include "systick.h"    /* systick_delay */

/* ── recurring base pointers (all resolved from the OEM literal pool) ─────────
 * Every DAT_* alias the decompiler created for status_process collapses to one
 * of these. The state object + clocking block are read as signed bytes in the
 * OEM (the `== -6` / `== -0x12` sentinels), so they are `char *`. */
#define G_STATE  ((signed char *)0x20000029u)   /* bike-state object; [4] = state uint8_t */
#define G_CLK    ((signed char *)0x200001D8u)   /* clocking/aux counter block */
#define GPIOA_BASE ((void *)0x40020000u)
#define GPIOB_BASE ((void *)0x40020400u)
#define GPIOC_BASE ((void *)0x40020800u)
#define GPIOD_BASE ((void *)0x40020C00u)
#define GPIOE_BASE ((void *)0x40021000u)
#define GPIOF_BASE ((void *)0x40021400u)   /* used only by enter_stop_mode's pin sweep */
#define GPIOG_BASE ((void *)0x40021800u)
#define GPIOH_BASE ((void *)0x40021C00u)

/* ── named callees (decoded elsewhere; K&R prototypes — several are variadic-ish
 * in the OEM ABI, e.g. save_state_record_to_eeprom takes 15 stack args and
 * maybe_enqueue_tx_message is called with 3 or 4) ───────────────────────────*/
extern int HAL_GPIO_ReadPin();
extern int HAL_GPIO_WritePin();
extern int stc_gas_gauge_set_run();
extern int battery_on_detect_step();
extern int battery_substate_advance();
extern int batteryware_update_set_pending();
extern int bike_is_locked();
extern int bms_modbus_read();
extern int bms_write_reg8_and_poll();
extern int charge_level_adc_get();
extern int console_cmd_logout();
extern int ctx_flag_0x131_is_clear();
extern int diagnostics_run_step();
/* console-dump commands + bus scan invoked by the diagnostics sequence (console.c/i2c.c) */
extern void console_cmd_shifterstatus();
extern void console_cmd_battery();
extern void console_cmd_motorstatus();
extern void console_cmd_show();
extern void console_cmd_adc();
extern void i2c_bus_scan();
extern void app_log_sink_enable();
extern int display_send_init_cmd();
void enter_stop_mode(uint8_t reason);          /* sourced at end of file */
extern int gpio_pc1_is_low();
extern int internal_lipo_charge_step();
extern int is_display_bus_ready();
extern int display_aux_byte_get();
extern int is_user_reset_pending();
extern int led_channel3_set_brightness();
extern int led_driver_enter_shipping_mode();
extern int led_driver_set_shipping_mode();
extern int light_sensor_read_step();
extern int lis3dh_config_motion_int();
extern int lis3dh_int1_clear();
extern int lis3dh_powerdown();
extern int lock_blink_sequence_step();
void locked_state_step(char *state_tab, int active);
extern int maybe_enqueue_tx_message();
extern int maybe_ota_progress_all_low();
extern int modbus_shift_submit();
extern int modem_info_ready();
extern int obj_set_field34();
extern int obj_set_field38();
extern int power_assist_gear_step();
extern int power_state_get_clamped();
void reboot_restart_task(void);                /* sourced at end of file */
extern int save_state_record_to_eeprom();
void sched_timer_arm_or_alloc(uint32_t period);
void set_unlock_state_persist(void);
extern int shifter_firmware_update_step();
extern int shifter_get_active_flag();
extern int shifter_sm_get_step();
extern int shifter_sm_set_step_10();
extern int shifter_sm_set_step_13();
extern int shifter_sm_set_step_3();
extern int sms_track_state_get();
extern int ssp_ble_enqueue_tx_packet();
extern int state_check_and_clear_step5();
extern int state_flags_clear();
extern int state_flags_set();
extern int state_flags_test();
void state_table_ptr_get(char **out);
extern int system_reset();
void telemetry_datalog_emit(uint8_t *ctx);
extern int stc_read();                 /* 0x080396E4 — STC3115 read into buffer */
extern int update_sm_is_idle(void);    /* 0x08032980 */
void testmode_command_dispatch(int cmd);
extern int snprintf(char *s, unsigned int n, const char *fmt, ...);
extern unsigned int strlen(const char *s);
extern int hw_version_lookup();          /* 0x08032CE4 */
extern int testmode_continue_state_10();
extern int testmode_enter_state_16();
extern int display_request_clear();
extern int matrix_draw_icon();
extern int clear_flag_00e5();
extern int app_ctx_clear_field_328();
extern int battery_telemetry_state_get();
void announce_records_reset();   /* sourced at end of file; one call site (status_process)
                                  * relies on the OEM's K&R arg-less form, so keep it unspecified */
extern int gpio_pc0_is_low();

/* Map an alarm/bike state code (0..0x3D) to its name (OEM alarm_state_name,
 * 0x08032DF0). The OEM is a `tbh` jump table; this behaviour-equivalent switch
 * returns the same const char* (into flash rodata in the OEM, string literals
 * here). These are the bike's full alarm / power / lock / OTA(OAD) / PIN state
 * names — the codes status_process logs and that maybe_get_bike_state tracks.
 * Out-of-range returns "UNKNOWN". */
const char *alarm_state_name(uint32_t state)
{
    switch (state) {
    case 0x00: return "ALARM_PRE_M1";
    case 0x01: return "ALARM_ACTIVE_M1_CNT";
    case 0x02: return "ALARM_ACTIVE_M2";
    case 0x03: return "ALARM_TRACKING_UNCONFIRMED";
    case 0x04: return "ALARM_TRACKING_CONFIRMED";
    case 0x05: return "ALARM_BMS_REMOVED";
    case 0x06: return "SET_SHIPPING";
    case 0x07: return "SHIPPING";
    case 0x08: return "BIKE_SHIPPING_ACCIDENTAL_WAKE";
    case 0x09: return "BIKE_SHIPPING_LIPOCHARGE";
    case 0x0A: return "START_FROM_SHIPPING";
    case 0x0B: return "PLAY_FIRE";
    case 0x0C: return "RIDING_MODE";
    case 0x0D: return "INIT";
    case 0x0E: return "STANDBY";
    case 0x0F: return "CPU_STOP_MODE";
    case 0x10: return "CPU_STOPPED";
    case 0x11: return "SHOW_LOCK";
    case 0x12: return "AUTOWAKEUP";
    case 0x13: return "CARDRIDGE_REMOVED";
    case 0x14: return "LIPOCHARGE";
    case 0x15: return "CHARGING";
    case 0x16: return "RESET";
    case 0x17: return "DIAGNOSE";
    case 0x18: return "DIAG_RDY";
    case 0x19: return "OAD_UPDATE";
    case 0x1A: return "OAD_FILE_TRF";
    case 0x1B: return "OAD_FAILED";
    case 0x1C: return "OAD_RX_SOUND";
    case 0x1D: return "OAD_FINISH";
    case 0x1E: return "FACTORY_TEST";
    case 0x1F: return "PLAY_SHTDN";
    case 0x20: return "PLAY_LOCK_SHTDN";
    case 0x21: return "PLAY_LOCK_FROM_SLEEP";
    case 0x22: return "PLAY_SHTDN_RDY";
    case 0x23: return "ALARM_DELAY_ON";
    case 0x24: return "TURN_ON";
    case 0x25: return "LOW_SOC";
    case 0x26: return "PIN_START";
    case 0x27: return "PIN_STUCK";
    case 0x28: return "PIN_1ST";
    case 0x29: return "PIN_2ND";
    case 0x2A: return "PIN_3ND";
    case 0x2B: return "PIN_CHECK";
    case 0x2C: return "PIN_OK";
    case 0x2D: return "PIN_SHOW_OK";
    case 0x2E: return "PIN_NOK";
    case 0x2F: return "PIN_NOK_SHOW";
    case 0x30: return "UNLOCK";
    case 0x31: return "EXTRA_ALREADY_UNLOCKED";
    case 0x32: return "UNLOCK_COUNT";
    case 0x33: return "UNLOCK_COUNT_TIMEOUT";
    case 0x34: return "LOCK_PLAY_UNLOCK";
    case 0x35: return "LOCK_PLAY_START";
    case 0x36: return "LOCK_DIM_OFF";
    case 0x37: return "LOCK_CLEAR";
    case 0x38: return "LOCK_SETUP";
    case 0x39: return "LOCK_PIC";
    case 0x3A: return "LOCK_COUNT";
    case 0x3B: return "COUNT_OFF";
    case 0x3C: return "COUNT_CLEAR";
    case 0x3D: return "FIND_MY_PLAY";
    default:   return "UNKNOWN";
    }
}

/* status_process — the bike's per-state behaviour engine (OEM 0x0802AAF8, the
 * largest function in the image). Ticked every super-loop from main(); after a
 * common prologue (telemetry, error-flag report, shifter/battery housekeeping,
 * scheduler-driven blinkers) it dispatches on the bike-state uint8_t G_STATE[4]
 * (the alarm_state_name codes) and runs that state's logic: read sensors
 * (LIS3DH, wheel, buttons, BMS, charger), drive lights/sound/motor-shifter,
 * manage LiPo charge + CPU-stop/sleep, run the alarm / kick-lock / PIN flow +
 * GSM tracking, and finalize OTA/diag with NVICReset (system_reset). It moves
 * the state via maybe_set_state_if_unlocked. Behaviour-equivalent reconstruction
 * of the live disassembly (exact control flow, ctx offsets + widths, log
 * strings); see docs/status-process.md and docs/state-machine.md. */
void status_process(uint8_t *ctx)
{
    uint16_t *puVar1;
    char cVar2;
    short sVar3;
    uint16_t uVar4;
    int iVar7, iVar8, iVar13, iVar17, iVar18;
    uint8_t uVar10, bVar11, uVar12;
    const char *pcVar14;
    uint32_t uVar15, uVar16, uVar19;
    void *pvVar20, *pvVar21;
    uint8_t local_138[16];
    char *local_128;
    uint16_t local_124[2];

  *(uint8_t **)0x20000944u = ctx;
  iVar13 = state_check_and_clear_step5();
  if (iVar13 != 0) {
    g_log_func("Double click\r\n");
  }
  if (3 < (uint8_t)(G_STATE[4] - 0xd)) {
    battery_on_detect_step(0);
  }
  if (0xfffd < (uint16_t)(*(int16_t *)(ctx + 0x342) - 1U)) {
    battery_on_detect_step(0);
  }
  if ((((25000 < *(uint16_t *)(ctx + 0x3b0)) && (*(int16_t *)(ctx + 0x402) == 1)) &&
      (G_STATE[4] == '\f')) && (G_CLK[0x7c] == '\0')) {
    G_CLK[0x7c] = 1;
    log_print_timestamp_prefix();
    g_log_func("SHifter on\r\n");
    shifter_sm_set_step_3();
  }
  if ((G_CLK[0x7d] != '\0') && (25000 < *(uint16_t *)(ctx + 0x3b0))) {
    G_CLK[0x7d] = 0;
    channel_notify_with_status(0x10);
  }
  if (G_STATE[0x14] != G_STATE[4]) {
    led_driver_set_shipping_mode(0xff);
    if (((uint8_t)(G_STATE[4] - 0xd) < 2) || (G_STATE[4] == '\0')) {
      announce_records_reset(6);
    }
    else {
      announce_records_reset(7);
    }
    log_print_timestamp_prefix();
    pcVar14 = alarm_state_name((uint32_t)(uint8_t)G_STATE[4]);
    g_log_func("BIKE_%s\r\n", pcVar14);
    G_STATE[0x14] = G_STATE[4];
  }
  iVar17 = *(int *)(ctx + 0x3b8);
  iVar18 = *(int *)(ctx + 0x3bc);
  if (iVar18 != *(int *)(G_CLK + 0x84) || iVar17 != *(int *)(G_CLK + 0x80)) {
    *(int *)(G_CLK + 0x80) = iVar17;
    *(int *)(G_CLK + 0x84) = iVar18;
    log_print_timestamp_prefix();
    if (*(int *)(ctx + 0x3bc) == 0 && *(int *)(ctx + 0x3b8) == 0) {
      g_log_func("Error Flags: None\r\n");
    }
    else {
      for (uVar19 = 0; uVar19 < 0x40; uVar19 = uVar19 + 1) {
        watchdog_kick();
        uVar15 = *(uint32_t *)(ctx + 0x3bc);
        if (((*(uint32_t *)(ctx + 0x3b8) >> (uVar19 & 0xff) | uVar15 << ((0x20 - uVar19) & 0xff)
             | uVar15 >> ((uVar19 - 0x20) & 0xff)) & 1) != 0) {
          g_log_func("Error Flags: %d\r\n", uVar19);
        }
      }
    }
  }
  if (G_STATE[0xb] == -0x12) {
    G_STATE[0xb] = *(uint8_t *)(ctx + 0x3c9);
    log_print_timestamp_prefix();
    g_log_func("Restore power level %d\r\n", *(uint8_t *)(ctx + 0x3c9));
    bVar11 = ssp_ble_enqueue_tx_packet(0x118, 0, (void *)0x0, '\x01');
    if (0x80 < bVar11) {
      g_log_func("  ERROR SSPB place\r\n");
    }
    bVar11 = ssp_ble_enqueue_tx_packet(0x11a, 0, (void *)0x0, '\x01');
    if (0x80 < bVar11) {
      g_log_func("  ERROR SSPB place\r\n");
    }
  }
  if (G_STATE[0x15] == -6) {
    uVar10 = scheduler_alloc();
    G_STATE[0x15] = uVar10;
    scheduler_start(uVar10, 500, (void *)0x0);
  }
  iVar13 = scheduler_slot_is_idle(G_STATE[0x15]);
  if (iVar13 != 0) {
    scheduler_start(G_STATE[0x15], 1000, (void *)0x0);
    G_CLK[0x88] = G_CLK[0x88] ^ 1;
    iVar13 = HAL_GPIO_ReadPin(GPIOA_BASE, 0x800);
    if ((iVar13 == 0) && (iVar13 = HAL_GPIO_ReadPin((void *)0x40020400, 8), iVar13 == 0)) {
      state_flags_clear(0, 0x400);
    }
  }
  telemetry_datalog_emit(ctx);
  iVar13 = scheduler_slot_is_idle(G_STATE[0x16]);
  if (iVar13 != 0) {
    scheduler_release((uint8_t *)0x2000003fu);
  }
  iVar13 = HAL_GPIO_ReadPin(GPIOC_BASE, 0x10);
  if ((iVar13 == 0) || (*(signed char *)(ctx + 0x3e1) == '\0')) {
    scheduler_release((uint8_t *)0x20000040u);
  }
  else if (G_STATE[0x17] == -6) {
    uVar10 = scheduler_alloc();
    G_STATE[0x17] = uVar10;
    scheduler_start(uVar10, 2000, (void *)0x0);
  }
  iVar13 = scheduler_slot_is_idle(G_STATE[0x17]);
  if (iVar13 != 0) {
    *(uint8_t *)(ctx + 0x3e1) = 0;
    *(uint8_t *)(ctx + 0x3e0) = 0;
    scheduler_release((uint8_t *)0x20000040u);
    log_print_timestamp_prefix();
    g_log_func("External battery removed\r\n");
    announce_mark(1);
  }
  iVar13 = scheduler_slot_is_idle(G_STATE[8]);
  if ((iVar13 != 0) && (G_STATE[4] != '\x1a')) {
    scheduler_release((uint8_t *)0x20000031u);
    HAL_GPIO_WritePin(GPIOD_BASE, 0x20, 1);
  }
  iVar7 = (int)(intptr_t)G_STATE;
  iVar8 = (int)(intptr_t)G_CLK;
  pvVar20 = GPIOD_BASE;
  switch (G_STATE[4]) {
  case 0:
    scheduler_release((uint8_t *)(G_STATE + 0x30));
    if (G_STATE[0x28] == -6) {
      G_CLK[0x89] = 0;
      iVar13 = HAL_GPIO_ReadPin(GPIOC_BASE,0x20);
      G_CLK[0xa1] = (iVar13 != 0);
      reset_dual_buffers_and_flags();
      uVar10 = scheduler_alloc();
      G_STATE[0x28] = uVar10;
      scheduler_start(uVar10,5000,0);
    }
    lis3dh_int1_clear();
    iVar17 = scheduler_slot_is_idle(G_STATE[0x28]);
    if (iVar17 != 0) {
      scheduler_release((uint8_t *)(G_STATE + 0x28));
      iVar18 = HAL_GPIO_ReadPin(GPIOC_BASE,0x20);
      G_CLK[0xa1] = (iVar18 != 0);
      G_CLK[0x89] = 0;
      G_CLK[0xa3] = 0;
      G_CLK[0xa4] = 0;
      G_STATE[4] = 1;
      *(uint8_t *)(ctx + 0x310) = 1;
      iVar13 = save_state_record_to_eeprom
                         (*(uint32_t *)(ctx + 0x310),*(uint32_t *)(ctx + 0x314),
                          *(uint32_t *)(ctx + 0x318),*(uint32_t *)(ctx + 0x31c),
                          *(uint32_t *)(ctx + 800),*(uint32_t *)(ctx + 0x324),
                          *(uint32_t *)(ctx + 0x328),*(uint32_t *)(ctx + 0x32c),
                          *(uint32_t *)(ctx + 0x330),*(uint32_t *)(ctx + 0x334),
                          *(uint32_t *)(ctx + 0x338),*(uint32_t *)(ctx + 0x33c),
                          *(uint32_t *)(ctx + 0x340),*(uint32_t *)(ctx + 0x344),
                          *(uint32_t *)(ctx + 0x348));
      if (iVar13 != 0) {
        g_log_func(" ERROR Save values\r\n");
      }
    }
    state_table_ptr_get(&local_128);
    if (*local_128 == '\x01') {
      announce_records_reset(1);
      iVar17 = HAL_GPIO_ReadPin(GPIOC_BASE,4);
      if (iVar17 == 0) {
        scheduler_release((uint8_t *)(G_STATE + 0xf));
        scheduler_release((uint8_t *)(G_STATE + 0x10));
        log_print_timestamp_prefix();
        g_log_func("Battery detected\r\n");
        G_STATE[4] = 0x11;
      }
      else {
        log_print_timestamp_prefix();
        g_log_func("Ask APP to unlock\r\n");
        local_138[0] = 1;
        bVar11 = ssp_ble_enqueue_tx_packet(0x5522,1,local_138,'\0');
        if (0x80 < bVar11) {
          g_log_func("  ERROR SSPB place\r\n");
        }
      }
    }
    if (*local_128 == '\x02') {
      scheduler_release((uint8_t *)(G_STATE + 0x30));
      announce_records_reset(1);
      G_STATE[4] = 0x26;
    }
    if ((local_128[1] == '\x01') || (local_128[2] == '\x06')) {
      announce_records_reset(6);
      log_print_timestamp_prefix();
      g_log_func("Locked\r\n");
      G_STATE[4] = 0x11;
    }
    break;
  case 1:
    scheduler_release((uint8_t *)(G_STATE + 0x20));
    if (*(signed char *)(iVar17 + 0x30) == -6) {
      uVar10 = scheduler_alloc();
      *(uint8_t *)(iVar17 + 0x30) = uVar10;
      scheduler_start(uVar10,0xe74,0);
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[0x31]);
    if (iVar13 != 0) {
      scheduler_release((uint8_t *)0x2000005Au);
      log_print_timestamp_prefix();
      g_log_func("LIS3DH_high sense\r\n");
      lis3dh_config_motion_int(0,6);
    }
    if ((G_CLK[0x89] != '\0') && (G_CLK[0xa3] == '\0')) {
      G_CLK[0xa3] = 1;
      log_print_timestamp_prefix();
      g_log_func("LIS3DH_low sense\r\n");
      lis3dh_config_motion_int(0,0x20);
      channel_notify_with_status(0xe);
      if (G_STATE[0x2f] == -6) {
        uVar10 = scheduler_alloc();
        G_STATE[0x2f] = uVar10;
        scheduler_start(uVar10,0xe74,0);
        G_CLK[0xa5] = 1;
      }
      if (G_STATE[0x31] == -6) {
        uVar10 = scheduler_alloc();
        G_STATE[0x31] = uVar10;
        scheduler_start(uVar10,4000,0);
      }
      scheduler_start(G_STATE[0x30],0xe74,0);
      log_print_timestamp_prefix();
      g_log_func("Lights P5\r\n");
      *(uint8_t *)(ctx + 0x351) = 8;
      *(uint8_t *)(ctx + 0x352) = 8;
    }
    iVar13 = HAL_GPIO_ReadPin(GPIOC_BASE,8);
    if ((iVar13 != 0) && (G_CLK[0x89] == '\0')) {
      lis3dh_int1_clear();
      log_print_timestamp_prefix();
      g_log_func("Mems trigger\r\n");
      G_CLK[0x89] = 1;
    }
    bVar11 = G_CLK[0xa1];
    uVar19 = HAL_GPIO_ReadPin(GPIOC_BASE,0x20);
    if ((bVar11 != uVar19) && (G_CLK[0x89] == '\0')) {
      iVar17 = HAL_GPIO_ReadPin(GPIOC_BASE,0x20);
      G_CLK[0xa1] = (iVar17 != 0);
      log_print_timestamp_prefix();
      g_log_func("Wheel trigger\r\n");
      G_CLK[0x89] = 1;
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[0x30]);
    if (iVar13 == 0) {
      if (((G_CLK[0xa4] != '\0') || (G_CLK[0x89] != '\0')) &&
         (G_CLK[0xa5] != '\0')) {
        set_mode_state_byte(0x12);
      }
    }
    else {
      scheduler_start(G_STATE[0x30],0xe74,0);
      G_CLK[0xa3] = 0;
      iVar17 = HAL_GPIO_ReadPin(GPIOC_BASE,0x20);
      G_CLK[0xa1] = (iVar17 != 0);
      if (G_CLK[0x89] == '\0') {
        scheduler_release((uint8_t *)0x20000059u);
        if (*(signed char *)(ctx + 0x310) != '\v') {
          *(uint8_t *)(ctx + 0x310) = 0xb;
          iVar13 = save_state_record_to_eeprom
                             (*(uint32_t *)(ctx + 0x310),*(uint32_t *)(ctx + 0x314),
                              *(uint32_t *)(ctx + 0x318),*(uint32_t *)(ctx + 0x31c),
                              *(uint32_t *)(ctx + 800),*(uint32_t *)(ctx + 0x324),
                              *(uint32_t *)(ctx + 0x328),*(uint32_t *)(ctx + 0x32c),
                              *(uint32_t *)(ctx + 0x330),*(uint32_t *)(ctx + 0x334),
                              *(uint32_t *)(ctx + 0x338),*(uint32_t *)(ctx + 0x33c),
                              *(uint32_t *)(ctx + 0x340),*(uint32_t *)(ctx + 0x344),
                              *(uint32_t *)(ctx + 0x348));
          if (iVar13 != 0) {
            g_log_func(" ERROR Save values\r\n");
          }
        }
        iVar13 = HAL_GPIO_ReadPin(GPIOC_BASE,0x10);
        if (iVar13 == 0) {
          set_mode_state_byte(8);
          G_STATE[4] = 0x15;
        }
        else {
          set_mode_state_byte(0xf);
          G_STATE[4] = 0xe;
        }
      }
      else {
        G_CLK[0x89] = 0;
        lis3dh_int1_clear();
        G_CLK[0xa4] = G_CLK[0xa4] + '\x01';
        log_print_timestamp_prefix();
        g_log_func("Alarm count %d\r\n",G_CLK[0xa4]);
      }
      if (G_CLK[0xa4] == '\x06') {
        scheduler_release((uint8_t *)(G_STATE + 0x30));
        G_STATE[4] = 2;
      }
      G_CLK[0x89] = 0;
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[0x2f]);
    if (iVar13 != 0) {
      scheduler_release((uint8_t *)0x20000058u);
      G_CLK[0xa5] = 0;
      set_mode_state_byte(0xf);
    }
    state_table_ptr_get(&local_128);
    if (*local_128 == '\x02') {
      scheduler_release((uint8_t *)(G_STATE + 0x30));
      announce_records_reset(1);
      G_STATE[4] = 0x26;
    }
    if ((*local_128 == '\x01') && (G_CLK[0xa4] == '\0')) {
      announce_records_reset(1);
      iVar13 = HAL_GPIO_ReadPin(GPIOC_BASE,4);
      if (iVar13 != 0) {
        log_print_timestamp_prefix();
        g_log_func("ASk APP to unlock\r\n");
        local_138[0] = 1;
        bVar11 = ssp_ble_enqueue_tx_packet(0x5522,1,local_138,'\0');
        if (0x80 < bVar11) {
          g_log_func("  ERROR SSPB place\r\n");
        }
      }
    }
    break;
  case 2:
    scheduler_release((uint8_t *)(G_STATE + 0x30));
    *(uint8_t *)(ctx + 0x310) = 2;
    if (*(signed char *)(iVar18 + 0x2e) == -6) {
      uVar10 = scheduler_alloc();
      *(uint8_t *)(iVar18 + 0x2e) = uVar10;
      scheduler_start(uVar10,5,0);
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[0x2e]);
    if (iVar13 != 0) {
      scheduler_start(G_STATE[0x2e],0xc3f,0);
      channel_notify_with_status(0xf);
      log_print_timestamp_prefix();
      g_log_func("Lights P6\r\n");
      *(uint8_t *)(ctx + 0x350) = 9;
      *(uint8_t *)(ctx + 0x352) = 9;
    }
    if (G_STATE[0x20] == -6) {
      uVar10 = scheduler_alloc();
      G_STATE[0x20] = uVar10;
      scheduler_start(uVar10,30000,0);
    }
    set_mode_state_byte(0x12);
    iVar13 = scheduler_slot_is_idle(G_STATE[0x20]);
    if (iVar13 != 0) {
      led_driver_set_shipping_mode(0);
      scheduler_release((uint8_t *)(G_STATE + 0x2e));
      scheduler_release((uint8_t *)(G_STATE + 0x20));
      set_mode_state_byte(0xf);
      G_STATE[4] = 0xe;
      log_print_timestamp_prefix();
      g_log_func("SMS: Tracking active\r\n");
      clear_flag_00e5();
      *(uint32_t *)(ctx + 0x33c) = 0;
      *(uint8_t *)(ctx + 0x3c8) = 1;
      *(uint8_t *)(ctx + 0x310) = 3;
      iVar13 = save_state_record_to_eeprom
                         (*(uint32_t *)(ctx + 0x310),*(uint32_t *)(ctx + 0x314),
                          *(uint32_t *)(ctx + 0x318),*(uint32_t *)(ctx + 0x31c),
                          *(uint32_t *)(ctx + 800),*(uint32_t *)(ctx + 0x324),
                          *(uint32_t *)(ctx + 0x328),*(uint32_t *)(ctx + 0x32c),
                          *(uint32_t *)(ctx + 0x330),*(uint32_t *)(ctx + 0x334),
                          *(uint32_t *)(ctx + 0x338),*(uint32_t *)(ctx + 0x33c),
                          *(uint32_t *)(ctx + 0x340),*(uint32_t *)(ctx + 0x344),
                          *(uint32_t *)(ctx + 0x348));
      if (iVar13 != 0) {
        g_log_func(" ERROR Save values\r\n");
      }
      G_STATE[0x11] = 1;
      app_ctx_clear_field_328();
    }
    state_table_ptr_get(&local_128);
    if ((*local_128 == '\x02') && (G_STATE[0x32] == -6)) {
      scheduler_release((uint8_t *)(G_STATE + 0x20));
      announce_records_reset(3);
      G_STATE[4] = 0x26;
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[0x32]);
    if (iVar13 != 0) {
      scheduler_release((uint8_t *)0x2000005Bu);
      G_CLK[0xa6] = 0;
    }
    break;
  case 3:
  case 4:
    scheduler_release((uint8_t *)(G_STATE + 0x30));
    scheduler_release((uint8_t *)(iVar7 + 0x20));
    if (*(signed char *)(iVar7 + 0x33) == -6) {
      uVar10 = scheduler_alloc();
      *(uint8_t *)(iVar7 + 0x33) = uVar10;
      scheduler_start(uVar10,8000,0);
      log_print_timestamp_prefix();
      g_log_func("Lights PC\r\n");
      *(uint8_t *)(ctx + 0x351) = 10;
      *(uint8_t *)(ctx + 0x352) = 10;
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[0x33]);
    if (iVar13 == 0) {
      set_mode_state_byte(0x12);
    }
    else {
      scheduler_release((uint8_t *)0x2000005Cu);
      iVar13 = HAL_GPIO_ReadPin(GPIOA_BASE,0x800);
      if (iVar13 == 0) {
        G_STATE[0x11] = 0;
      }
      else {
        G_STATE[0x11] = 3;
      }
      set_mode_state_byte(0xf);
      G_STATE[4] = 0xe;
    }
    uVar10 = maybe_get_bike_state();
    if ((uVar10 == '\x03') && (state_table_ptr_get(&local_128), *local_128 == '\x02')) {
      announce_records_reset(3);
      led_driver_set_shipping_mode(0xff);
      if (*(short *)(ctx + 0x100) == 0xff) {
        log_print_timestamp_prefix();
        g_log_func("No backupcode\r\n");
        G_STATE[4] = 0x11;
      }
      else {
        G_STATE[4] = 0x26;
      }
      *(uint8_t *)(ctx + 0x310) = 3;
      iVar13 = save_state_record_to_eeprom
                         (*(uint32_t *)(ctx + 0x310),*(uint32_t *)(ctx + 0x314),
                          *(uint32_t *)(ctx + 0x318),*(uint32_t *)(ctx + 0x31c),
                          *(uint32_t *)(ctx + 800),*(uint32_t *)(ctx + 0x324),
                          *(uint32_t *)(ctx + 0x328),*(uint32_t *)(ctx + 0x32c),
                          *(uint32_t *)(ctx + 0x330),*(uint32_t *)(ctx + 0x334),
                          *(uint32_t *)(ctx + 0x338),*(uint32_t *)(ctx + 0x33c),
                          *(uint32_t *)(ctx + 0x340),*(uint32_t *)(ctx + 0x344),
                          *(uint32_t *)(ctx + 0x348));
      if (iVar13 != 0) {
        g_log_func(" ERROR Save values\r\n");
      }
    }
    break;
  case 5:
    set_mode_state_byte(0x12);
    if (G_STATE[0x19] == -6) {
      iVar13 = HAL_GPIO_ReadPin(GPIOA_BASE,0x800);
      if (iVar13 == 0) {
        g_log_func("Battery is removed!\r\n");
      }
      else {
        g_log_func("Cardridge is removed!\r\n");
      }
      uVar10 = scheduler_alloc();
      G_STATE[0x19] = uVar10;
      scheduler_start(uVar10,120000,0);
    }
    iVar13 = HAL_GPIO_ReadPin(GPIOC_BASE,0x400);
    if (iVar13 != 0) {
      log_print_timestamp_prefix();
      g_log_func("Locked\r\n");
      scheduler_release((uint8_t *)(G_STATE + 0x19));
      scheduler_release((uint8_t *)(G_STATE + 0x2e));
      set_mode_state_byte(0xf);
      G_STATE[4] = 0xe;
    }
    if (G_STATE[0x2e] == -6) {
      uVar10 = scheduler_alloc();
      G_STATE[0x2e] = uVar10;
      scheduler_start(uVar10,5,0);
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[0x2e]);
    if (iVar13 != 0) {
      *(uint8_t *)(ctx + 0x350) = 9;
      *(uint8_t *)(ctx + 0x352) = 9;
      scheduler_start(G_STATE[0x2e],0xc3f,0);
      channel_notify_with_status(0xf);
    }
    iVar17 = scheduler_slot_is_idle(G_STATE[0x19]);
    if (iVar17 != 0) {
      scheduler_release((uint8_t *)(G_STATE + 0x19));
      scheduler_release((uint8_t *)(G_STATE + 0x2e));
      iVar17 = HAL_GPIO_ReadPin(GPIOA_BASE,0x800);
      if (iVar17 == 0) {
        G_STATE[0x11] = 7;
        G_STATE[4] = 0xf;
      }
      else {
        G_STATE[4] = 3;
      }
    }
    break;
  case 6:
    iVar17 = scheduler_slot_is_idle(G_STATE[0x2c]);
    if (iVar17 != 0) {
      scheduler_release((uint8_t *)(G_STATE + 0x2c));
      G_STATE[4] = 0xf;
    }
    break;
  case 7:
    if ((signed char)G_STATE[0x2c] == -6) {
      set_mode_state_byte(0x1c);
      channel_notify_emit(0x18,1);
      uVar10 = scheduler_alloc();
      G_STATE[0x2c] = uVar10;
      scheduler_start(uVar10,10000,0);
    }
    iVar17 = scheduler_slot_is_idle(G_STATE[0x2c]);
    if (iVar17 != 0) {
      scheduler_start(G_STATE[0x2c],100,0);
      *(uint8_t *)(ctx + 0x314) = 1;
      G_STATE[0x11] = 6;
      G_STATE[4] = 6;
    }
    break;
  case 8:
    HAL_GPIO_WritePin((void *)GPIOD_BASE,0x2000,0);
    HAL_GPIO_WritePin(pvVar20,0x8000,0);
    pvVar20 = (void *)((int)pvVar20 + 0x400);
    HAL_GPIO_WritePin(pvVar20,4,0);
    pvVar21 = (void *)GPIOA_BASE;
    HAL_GPIO_WritePin((void *)GPIOA_BASE,0x8000,0);
    HAL_GPIO_WritePin(pvVar21,0x1000,0);
    HAL_GPIO_WritePin(pvVar20,0x40,1);
    obj_set_field34(0);
    obj_set_field38(0);
    led_channel3_set_brightness(0);
    display_send_init_cmd();
    lis3dh_powerdown();
    HAL_GPIO_WritePin(pvVar20,0x20,1);
    pvVar21 = (void *)((int)pvVar21 + 0x400);
    HAL_GPIO_WritePin(pvVar21,0x200,1);
    systick_delay(10);
    HAL_GPIO_WritePin(pvVar20,0x20,0);
    HAL_GPIO_WritePin(pvVar21,0x200,0);
    local_138[0] = 1;
    ssp_ble_enqueue_tx_packet(0x112,1,local_138,'\0');
    *(uint16_t *)(G_CLK + 0x92) = 0;
    *(uint16_t *)(G_CLK + 0x90) = 0;
    maybe_enqueue_tx_message(0x15,4,(uint16_t *)(G_CLK + 0x90),0);
    local_124[0] = 1;
    maybe_enqueue_tx_message(0x14,2,local_124,0);
    maybe_set_state_if_unlocked('\t');
    break;
  case 9:
    iVar13 = HAL_GPIO_ReadPin((void *)GPIOC_BASE,0x10);
    if ((iVar13 == 0) || (iVar13 = HAL_GPIO_ReadPin((void *)GPIOD_BASE,4), iVar13 == 0)) {
      system_reset();
LAB_0802ca9a:
      uVar10 = scheduler_alloc();
      G_STATE[0x2d] = uVar10;
      scheduler_start(uVar10,0x1d4c0,0);
      log_print_timestamp_prefix();
      g_log_func("Wait 2 minutes before evaluating the LiPo SoC\r\n");
    }
    else {
      uVar12 = internal_lipo_charge_step(ctx + 0x3fc);
      G_STATE[0x18] = uVar12;
      if ((signed char)G_STATE[0x2d] == -6) goto LAB_0802ca9a;
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[0x2d]);
    if ((iVar13 != 0) &&
       ((G_STATE[0x18] < 2 || (uVar19 = charge_level_adc_get(), 0x4b < uVar19)))) {
      scheduler_release((uint8_t *)(G_STATE + 0x2d));
      log_print_timestamp_prefix();
      uVar16 = charge_level_adc_get();
      g_log_func("LiPo charged to at least %d%%. Going back to sleep. See ya :)\r\n",uVar16);
      G_STATE[0x11] = 6;
      maybe_set_state_if_unlocked('\x0f');
    }
    break;
  case 10:
    if ((signed char)G_STATE[0x2c] == -6) {
      set_mode_state_byte(0x1c);
      pvVar20 = (void *)GPIOE_BASE;
      HAL_GPIO_WritePin((void *)GPIOE_BASE,0x20,1);
      systick_delay(10);
      HAL_GPIO_WritePin(pvVar20,0x20,0);
      uVar10 = scheduler_alloc();
      G_STATE[0x2c] = uVar10;
      scheduler_start(uVar10,10000,0);
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[0x2c]);
    if ((iVar13 != 0) || (*(int *)(ctx + 0x38c) != 0)) {
      scheduler_release((uint8_t *)0x20000055u);
      set_mode_state_byte(0x1d);
      channel_notify_with_status(0x15);
      *(uint8_t *)(ctx + 0x314) = 0;
      iVar13 = save_state_record_to_eeprom
                         (*(uint32_t *)(ctx + 0x310),*(uint32_t *)(ctx + 0x314),
                          *(uint32_t *)(ctx + 0x318),*(uint32_t *)(ctx + 0x31c),
                          *(uint32_t *)(ctx + 800),*(uint32_t *)(ctx + 0x324),
                          *(uint32_t *)(ctx + 0x328),*(uint32_t *)(ctx + 0x32c),
                          *(uint32_t *)(ctx + 0x330),*(uint32_t *)(ctx + 0x334),
                          *(uint32_t *)(ctx + 0x338),*(uint32_t *)(ctx + 0x33c),
                          *(uint32_t *)(ctx + 0x340),*(uint32_t *)(ctx + 0x344),
                          *(uint32_t *)(ctx + 0x348));
      if (iVar13 != 0) {
        g_log_func(" ERROR Save values\r\n");
      }
      G_STATE[4] = 0xb;
    }
    break;
  case 0xb:
    iVar13 = is_display_bus_ready();
    if (iVar13 != 0) {
      iVar13 = HAL_GPIO_ReadPin((void *)GPIOC_BASE,0x10);
      if (iVar13 == 0) {
        set_mode_state_byte(8);
        G_STATE[4] = 0x15;
      }
      else {
        iVar13 = bike_is_locked();
        if (iVar13 == 0) {
          *(uint8_t *)(ctx + 0x351) = 3;
          *(uint8_t *)(ctx + 0x350) = 3;
          *(uint8_t *)(ctx + 0x352) = 3;
          set_mode_state_byte(6);
          G_STATE[4] = 0xc;
        }
        else if (*(short *)(ctx + 0x100) == 0xff) {
          G_STATE[4] = 0x11;
        }
        else {
          G_STATE[4] = 0x26;
        }
      }
    }
    break;
  case 0xc:
    state_table_ptr_get(&local_128);
    iVar13 = light_sensor_read_step();
    if (iVar13 != 0xfffe) {
      uVar19 = light_sensor_read_step();
      if (uVar19 < *(uint16_t *)(ctx + 0x102)) {
        if (G_CLK[0x8a] == '\0') {
          if (G_STATE[0x1e] == -6) {
            uVar10 = scheduler_alloc();
            G_STATE[0x1e] = uVar10;
          }
          scheduler_start(G_STATE[0x1e],5000,0);
        }
        G_CLK[0x8a] = 1;
      }
      else {
        if (G_CLK[0x8a] != '\0') {
          if (G_STATE[0x1e] == -6) {
            uVar10 = scheduler_alloc();
            G_STATE[0x1e] = uVar10;
          }
          scheduler_start(G_STATE[0x1e],5000,0);
        }
        G_CLK[0x8a] = 0;
      }
      iVar13 = scheduler_slot_is_idle(G_STATE[0x1e]);
      if (iVar13 != 0) {
        if (G_CLK[0x8a] == '\0') {
          uVar16 = 0xff;
        }
        else {
          uVar16 = 0x32;
        }
        led_driver_set_shipping_mode(uVar16);
        scheduler_release((uint8_t *)0x20000047u);
      }
    }
    if (G_STATE[0x1f] == -6) {
      uVar10 = scheduler_alloc();
      G_STATE[0x1f] = uVar10;
      scheduler_start(uVar10,10000,0);
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[0x1f]);
    if (iVar13 != 0) {
      log_print_timestamp_prefix();
      g_log_func("Start-GSM: riding\r\n");
      scheduler_start(G_STATE[0x1f],0x36ee80,0);
      clear_flag_00e5();
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[7]);
    if (iVar13 != 0) {
      scheduler_release((uint8_t *)0x20000030u);
    }
    if (G_STATE[7] == -6) {
      if (*(uint16_t *)(ctx + 0x3b0) < 25000) {
        battery_substate_advance();
        state_flags_set(0x100000,0);
      }
      else {
        state_flags_clear(0x100000,0);
      }
    }
    power_assist_gear_step(ctx);
    scheduler_release((uint8_t *)(G_STATE + 0x20));
    scheduler_release((uint8_t *)(G_STATE + 0xf));
    iVar13 = HAL_GPIO_ReadPin((void *)0x40020000u,0x800);
    if ((iVar13 != 0) && (iVar13 = HAL_GPIO_ReadPin((void *)0x40020400u,8), iVar13 == 0)) {
      state_flags_set(0,0x400);
    }
    if (*(uint16_t *)(ctx + 0x3c2) < 9) {
      if (G_CLK[0x8b] == '\0') {
        G_CLK[0x8b] = 1;
        announce_records_reset();
      }
      iVar13 = HAL_GPIO_ReadPin((void *)0x40020800u,0x10);
      if ((iVar13 == 0) && (*(signed char *)(ctx + 0x3e0) != '\x01')) {
        if (G_STATE[0x1b] == -6) {
          uVar10 = scheduler_alloc();
          G_STATE[0x1b] = uVar10;
          scheduler_start(uVar10,1000,0);
        }
      }
      else {
        scheduler_release((uint8_t *)0x20000044u);
      }
      iVar17 = scheduler_slot_is_idle(G_STATE[0x1b]);
      if (iVar17 != 0) {
        scheduler_release((uint8_t *)(G_STATE + 0x1b));
        log_print_timestamp_prefix();
        g_log_func("Lights P2a\r\n");
        *(uint8_t *)(ctx + 0x351) = 4;
        *(uint8_t *)(ctx + 0x350) = 4;
        *(uint8_t *)(ctx + 0x352) = 4;
        scheduler_release((uint8_t *)(G_STATE + 0xf));
        scheduler_release((uint8_t *)(G_STATE + 0x1f));
        iVar13 = bike_is_locked();
        if (iVar13 == 0) {
          *(uint8_t *)(ctx + 0x311) = 0;
        }
        *(uint32_t *)(ctx + 0x3b8) = 0;
        *(uint32_t *)(ctx + 0x3bc) = 0;
        scheduler_release((uint8_t *)(G_STATE + 0x1c));
        scheduler_release((uint8_t *)(G_STATE + 0x1d));
        set_mode_state_byte(8);
        G_STATE[4] = 0x15;
      }
      if (G_STATE[6] == -6) {
        uVar10 = scheduler_alloc();
        G_STATE[6] = uVar10;
        scheduler_start(uVar10,0x2bf20,0);
      }
      iVar13 = scheduler_slot_is_idle(G_STATE[6]);
      if (iVar13 != 0) {
        log_print_timestamp_prefix();
        g_log_func("Sleep timer elapsed\r\n");
        scheduler_release((uint8_t *)0x2000002fu);
        channel_notify_with_status(0x11);
        log_print_timestamp_prefix();
        g_log_func("BLE standby mode\r\n");
        *(uint8_t *)(ctx + 0x351) = 4;
        *(uint8_t *)(ctx + 0x352) = 4;
        announce_records_reset(1);
        iVar13 = HAL_GPIO_ReadPin((void *)0x40020800u,4);
        if (iVar13 == 0) {
          set_mode_state_byte(0x19);
          G_STATE[4] = 0x1f;
        }
        else {
          log_print_timestamp_prefix();
          g_log_func("Low BMS\r\n");
          set_mode_state_byte(0xf);
          G_STATE[4] = 0xe;
        }
      }
    }
    else {
      if (local_128[2] == '\x01') {
        announce_records_reset(4);
      }
      G_CLK[0x8b] = 0;
      iVar13 = HAL_GPIO_ReadPin((void *)0x40020800u,0x100);
      if (iVar13 == 0) {
        HAL_GPIO_WritePin((void *)0x40020400u,8,0);
      }
      else {
        HAL_GPIO_WritePin((void *)0x40020400u,8,1);
      }
      scheduler_start(G_STATE[6],0x2bf20,0);
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[0x21]);
    if (iVar13 != 0) {
      scheduler_release((uint8_t *)0x2000004au);
    }
    if (G_STATE[0x22] == -6) {
      uVar10 = scheduler_alloc();
      G_STATE[0x22] = uVar10;
      scheduler_start(uVar10,200,0);
    }
    iVar13 = power_state_get_clamped();
    if (iVar13 == 1) {
      log_print_timestamp_prefix();
      g_log_func("Lights P2a\r\n");
      shifter_sm_set_step_10();
      set_mode_state_byte(0xf);
      G_STATE[4] = 0xe;
    }
    uVar4 = *(uint16_t *)(ctx + 0x402);
    if ((G_CLK[0x8c] != uVar4) &&
       (G_CLK[0x8c] = (signed char)uVar4, uVar4 == 0)) {
      *(uint16_t *)(G_CLK + 0x92) = 0;
      puVar1 = (uint16_t *)(G_CLK + 0x90);
      *puVar1 = 0;
      uVar19 = maybe_enqueue_tx_message(0x15,4,puVar1);
      if (0x10 < uVar19) {
        g_log_func("  ERROR SSPM place\r\n");
      }
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[0x22]);
    if ((((iVar13 != 0) || (uVar19 = gpio_pc1_is_low(), uVar19 != (uint32_t)(uint8_t)G_CLK[0x94]))
        && (uVar19 = power_state_get_clamped(), 2 < uVar19)) &&
       ((*(short *)(ctx + 0x402) == 1 && (iVar13 = modem_info_ready(), iVar13 == 0)))) {
      uVar12 = gpio_pc1_is_low();
      G_CLK[0x94] = uVar12;
      if (*(signed char *)(ctx + 0x108) == '\0') {
        iVar13 = gpio_pc1_is_low();
      }
      else if ((local_128[1] == '\x02') || (local_128[1] == '\x04')) {
        iVar13 = gpio_pc1_is_low();
        if (iVar13 == 0) {
          iVar13 = 0;
        }
        else {
          iVar13 = 1;
        }
      }
      else {
        iVar13 = 0;
      }
      scheduler_start(G_STATE[0x22],200,0);
      if (*(signed char *)(ctx + 0x109) == '\x02') {
        if ((((iVar13 == 0) || (99 < *(uint16_t *)(ctx + 0x3c2))) ||
            (3 < *(uint8_t *)(ctx + 0x3c9))) ||
           ((G_CLK[0x18] != '\0' ||
            (iVar18 = state_flags_test(0,0x200), iVar18 != 0)))) {
          if (((iVar13 == 0) ||
              ((*(uint16_t *)(ctx + 0x3c2) < 0x65 || (3 < *(uint8_t *)(ctx + 0x3c9))))) ||
             (G_CLK[0x18] != '\0')) {
            iVar17 = (int)ctx + (*(uint8_t *)(ctx + 0x3c9) + 0x5b) * 8;
            uVar16 = *(uint32_t *)(iVar17 + 10);
            *(uint32_t *)0x20000270u = *(uint32_t *)(iVar17 + 6);
            *(uint32_t *)0x20000274u = uVar16;
          }
          else {
            uVar16 = *(uint32_t *)(ctx + 0x302);
            *(uint32_t *)0x20000270u = *(uint32_t *)(ctx + 0x2fe);
            *(uint32_t *)0x20000274u = uVar16;
          }
        }
        else {
          uVar16 = *(uint32_t *)(ctx + 0x30a);
          *(uint32_t *)0x20000270u = *(uint32_t *)(ctx + 0x306);
          *(uint32_t *)0x20000274u = uVar16;
        }
        uVar19 = maybe_enqueue_tx_message(0x17,8,(void *)0x20000270u,0);
        if (0x10 < uVar19) {
          g_log_func("  ERROR SSPM2 place\r\n");
        }
      }
      else {
        if (*(signed char *)(ctx + 0x3cb) != '\0') {
          if ((G_STATE[0x23] == -6) && (iVar17 = gpio_pc1_is_low(), iVar17 != 0)) {
            uVar10 = scheduler_alloc();
            G_STATE[0x23] = uVar10;
            scheduler_start(uVar10,0x96,0);
          }
          iVar17 = gpio_pc1_is_low();
          if (iVar17 == 0) {
            scheduler_release((uint8_t *)0x2000004cu);
          }
        }
        if (((iVar13 == 0) ||
            (((iVar13 = scheduler_slot_is_idle(G_STATE[0x23]), iVar13 == 0 &&
              (*(signed char *)(ctx + 0x3cb) != '\0')) ||
             (G_CLK[0x18] != '\0')))) ||
           (iVar17 = state_flags_test(0,0x200), iVar17 != 0)) {
          iVar17 = (int)ctx + (*(uint8_t *)(ctx + 0x3c9) + 0x70) * 4;
          *(uint16_t *)(G_CLK + 0x92) = *(uint16_t *)(iVar17 + 6);
          *(uint16_t *)(G_CLK + 0x90) = *(uint16_t *)(iVar17 + 4);
        }
        else {
          *(uint16_t *)(G_CLK + 0x92) = *(uint16_t *)(ctx + 0x1da);
          *(uint16_t *)(G_CLK + 0x90) = *(uint16_t *)(ctx + 0x1d8);
        }
        uVar19 = maybe_enqueue_tx_message(0x15,4,(void *)0x20000268u,0);
        if (0x10 < uVar19) {
          g_log_func("  ERROR SSPM place\r\n");
        }
      }
      uVar19 = maybe_enqueue_tx_message(0xd,0,0,1);
      if (0x10 < uVar19) {
        g_log_func("  ERROR SSPM2 place\r\n");
      }
    }
    iVar13 = gpio_pc0_is_low();
    if (((iVar13 != 0) && (iVar13 = state_flags_test(0,0x100), iVar13 == 0)) ||
       ((iVar13 = gpio_pc1_is_low(), iVar13 != 0 &&
        (iVar13 = state_flags_test(0,0x200), iVar13 == 0)))) {
      scheduler_start(G_STATE[6],0x2bf20,0);
    }
    bVar11 = aux_mode_byte_get();
    if (bVar11 == 0) {
      announce_records_reset(1);
    }
    if (*local_128 == '\x01') {
      announce_records_reset(1);
      channel_notify_with_status(*(uint8_t *)(ctx + 0x318));
    }
    if (*(signed char *)(ctx + 0x3cb) == '\0') {
      if (((*local_128 == '\x02') && (*(short *)(ctx + 0x3c2) != 0)) &&
         (*(signed char *)(ctx + 0x108) == '\0')) {
        announce_records_reset(1);
        G_CLK[0xa0] = 1;
      }
      iVar13 = gpio_pc0_is_low();
      if (iVar13 == 0) {
        G_CLK[0xa0] = 0;
      }
      if (G_CLK[0xa0] != '\0') {
        if (G_STATE[0x24] == -6) {
          uVar10 = scheduler_alloc();
          G_STATE[0x24] = uVar10;
          scheduler_start(uVar10,0x50,0);
        }
        iVar13 = scheduler_slot_is_idle(G_STATE[0x24]);
        if (iVar13 != 0) {
          scheduler_release((uint8_t *)0x2000004du);
          channel_notify_with_status(*(uint8_t *)(ctx + 0x318));
        }
      }
    }
    iVar13 = HAL_GPIO_ReadPin((void *)0x40020000u,0x800);
    if ((((iVar13 == 0) || (iVar13 = HAL_GPIO_ReadPin((void *)0x40020800u,0x400), iVar13 != 0)) ||
        (iVar13 = HAL_GPIO_ReadPin((void *)0x40020800,0x100), iVar13 == 0)) ||
       ((*(signed char *)(ctx + 0x340) != '\0' || (*(signed char *)(ctx + 0x312) != '\0')))) {
      iVar13 = bike_is_locked();
      if ((iVar13 != 0) &&
         ((*(short *)(ctx + 0x3c2) == 0 && (G_STATE[0x1a] == -6)))) {
        uVar10 = scheduler_alloc();
        G_STATE[0x1a] = uVar10;
        scheduler_start(uVar10,100,0);
      }
      iVar13 = scheduler_slot_is_idle(G_STATE[0x1a]);
      if (iVar13 != 0) {
        scheduler_release((uint8_t *)0x20000043u);
        iVar13 = bike_is_locked();
        if (iVar13 != 0) {
          scheduler_release((uint8_t *)0x2000002fu);
          maybe_set_state_if_unlocked(' ');
        }
      }
      if ((((*local_128 == '\x04') || (local_128[2] == '\x06')) &&
          (*(short *)(ctx + 0x3c2) == 0)) && (bVar11 = aux_mode_byte_get(), bVar11 != 0))
      {
        if ((*local_128 == '\x04') && (iVar13 = state_flags_test(0,0x100), iVar13 == 0)) {
          announce_records_reset(1);
          log_print_timestamp_prefix();
          g_log_func("Button horn: off\r\n");
        }
        if (local_128[2] == '\x03') {
          announce_records_reset(4);
          log_print_timestamp_prefix();
          g_log_func("USER Button reset: off\r\n");
        }
        announce_records_reset(5);
        channel_notify_with_status(0x11);
        log_print_timestamp_prefix();
        g_log_func("Charger detected Lights OFF\r\n");
        *(uint8_t *)(ctx + 0x350) = 4;
        *(uint8_t *)(ctx + 0x351) = 4;
        *(uint8_t *)(ctx + 0x352) = 4;
        display_request_clear();
        scheduler_release((uint8_t *)(G_STATE + 0x1f));
        G_STATE[4] = 0x1f;
      }
    }
    else {
      maybe_set_state_if_unlocked('\a');
    }
    break;
  case 0xd:
    cVar2 = *(signed char *)(ctx + 0x310);
    if (cVar2 == '\x01') {
      G_CLK[0x89] = 1;
      G_STATE[4] = 1;
    }
    else if (cVar2 == '\x02') {
      G_STATE[4] = 2;
    }
    else if ((cVar2 == '\x03') && (*(int *)0x20000004u != 1)) {
      G_STATE[4] = 3;
    }
    else if ((cVar2 == '\x04') && (*(int *)0x20000004u != 1)) {
      G_STATE[4] = 4;
    }
    else {
      G_STATE[4] = 0xe;
    }
    if ((uint8_t)(cVar2 - 3U) < 2) {
      iVar13 = HAL_GPIO_ReadPin((void *)0x40020000u, 0x800);
      if (iVar13 == 0) {
        G_STATE[0x11] = 0;
      }
      else {
        G_STATE[0x11] = 3;
      }
    }
    if (*(signed char *)(ctx + 0x314) != '\0') {
      log_print_timestamp_prefix();
      g_log_func("Wake from shipping\r\n");
      iVar13 = HAL_GPIO_ReadPin((void *)0x40020000u, 0x800);
      if (iVar13 == 0) {
        iVar13 = HAL_GPIO_ReadPin((void *)0x40020800u, 0x10);
        if ((iVar13 == 0) || (iVar13 = HAL_GPIO_ReadPin((void *)0x40020c00u, 4), iVar13 == 0)) {
          log_print_timestamp_prefix();
          g_log_func("First time use\r\n");
          *(uint8_t *)(ctx + 0x314) = 0;
          maybe_set_state_if_unlocked('\n');
        }
        else {
          maybe_set_state_if_unlocked('\b');
        }
      }
      else {
        iVar13 = HAL_GPIO_ReadPin((void *)0x40020800u, 0x10);
        if (iVar13 == 0) {
          maybe_set_state_if_unlocked('\x14');
        }
        else {
          maybe_set_state_if_unlocked('\a');
        }
      }
    }
    local_138[0] = 1;
    bVar11 = ssp_ble_enqueue_tx_packet(0x110, 1, local_138, '\0');
    if (0x80 < bVar11) {
      g_log_func("  ERROR SSPB place\r\n");
    }
    iVar13 = maybe_ota_progress_all_low();
    if ((iVar13 != 0) && (*(signed char *)(ctx + 0x32c) != '\0')) {
      for (uVar19 = 0; uVar19 < 6; uVar19 = (uVar19 + 1) & 0xff) {
        cVar2 = *(signed char *)(ctx + uVar19 + 0x32c);
        if (cVar2 == '\x06') {
          testmode_continue_state_10((uVar19 + 1) & 0xff);
          G_STATE[4] = 0x19;
          return;
        }
        if (cVar2 == '\x02') {
          testmode_enter_state_16((uVar19 + 1) & 0xff);
          G_STATE[4] = 0x19;
        }
      }
    }
    break;
  case 0xe:
    if (*(signed char *)(G_STATE + 0xf) == -6) {
      uVar10 = scheduler_alloc();
      *(uint8_t *)(G_STATE + 0xf) = uVar10;
      scheduler_start(uVar10, 10000, 0);
      *(uint32_t *)(ctx + 0x3b8) = 0;
      *(uint32_t *)(ctx + 0x3bc) = 0;
      iVar13 = HAL_GPIO_ReadPin((void *)0x40020000u, 0x800);
      if (iVar13 != 0) {
        log_print_timestamp_prefix();
        g_log_func("No kicklock coil\r\n");
      }
      log_print_timestamp_prefix();
      g_log_func("Lights P3\r\n");
      *(uint8_t *)(ctx + 0x350) = 4;
      *(uint8_t *)(ctx + 0x351) = 4;
      *(uint8_t *)(ctx + 0x352) = 4;
    }
    iVar13 = sms_track_state_get();
    if (iVar13 == 2) {
      scheduler_start(*(uint8_t *)(G_STATE + 0xf), 10000, 0);
    }
    iVar13 = HAL_GPIO_ReadPin((void *)0x40020800u, 4);
    if (iVar13 != 0) {
      scheduler_start(*(uint8_t *)(G_STATE + 0xf), 10000, 0);
    }
    iVar13 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0xf));
    if ((iVar13 != 0) && (*(signed char *)(G_STATE + 0x16) == -6)) {
      uVar19 = charge_level_adc_get();
      if ((uVar19 < 0x3d) ||
         (charge_level_adc_get(), *(signed char *)(G_STATE + 0x18) == '\x02'))
      {
        uVar19 = power_state_get_clamped();
        if (uVar19 < 2) {
          log_print_timestamp_prefix();
          uVar16 = charge_level_adc_get();
          g_log_func("Charging liPo %d%% not possible\r\n", uVar16);
          G_STATE[0x11] = 1;
          G_STATE[4] = 0xf;
        }
        else {
          battery_on_detect_step(0);
          log_print_timestamp_prefix();
          uVar16 = charge_level_adc_get();
          g_log_func("Charging liPo %d%%\r\n", uVar16);
          *(int *)0x20000004u = 1;
        }
      }
      else {
        scheduler_release((uint8_t *)(G_STATE + 0xf));
        if (*(signed char *)(G_STATE + 0x11) == '\b') {
          iVar17 = HAL_GPIO_ReadPin((void *)0x40020000u, 0x800);
          if (iVar17 == 0) {
            iVar17 = bike_is_locked();
            if ((iVar17 == 0) || (*(signed char *)(ctx + 0x317) == '\0')) {
              G_STATE[0x11] = 2;
            }
            else {
              G_STATE[0x11] = 1;
            }
          }
          else {
            G_STATE[0x11] = 2;
          }
        }
        G_STATE[4] = 0xf;
      }
    }
    if ((((*(signed char *)(G_STATE + 0x19) == -6) &&
         (iVar13 = HAL_GPIO_ReadPin((void *)0x40020800u, 0x400), iVar13 == 0)) &&
        (iVar13 = HAL_GPIO_ReadPin((void *)0x40020000u, 0x800), iVar13 == 0)) &&
       (iVar13 = is_user_reset_pending(), iVar13 == 0)) {
      uVar10 = scheduler_alloc();
      *(uint8_t *)(G_STATE + 0x19) = uVar10;
      scheduler_start(uVar10, 2000, 0);
    }
    iVar13 = HAL_GPIO_ReadPin((void *)0x40020800u, 0x400);
    if (iVar13 != 0) {
      scheduler_release((uint8_t *)0x20000042u);
    }
    iVar17 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0x19));
    if (iVar17 != 0) {
      scheduler_release((uint8_t *)(G_STATE + 0x19));
      log_print_timestamp_prefix();
      g_log_func("Battery removed\r\n");
      G_STATE[4] = 5;
    }
    iVar13 = HAL_GPIO_ReadPin((void *)0x40020800u, 4);
    if (iVar13 == 0) {
      scheduler_release((uint8_t *)0x20000039u);
    }
    else {
      if (*(signed char *)(G_STATE + 0x10) == -6) {
        uVar10 = scheduler_alloc();
        *(uint8_t *)(G_STATE + 0x10) = uVar10;
        scheduler_start(uVar10, 600000, 0);
      }
      iVar13 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0x10));
      if (iVar13 != 0) {
        scheduler_release((uint8_t *)0x20000039u);
        log_print_timestamp_prefix();
        g_log_func("Set BLE sleep\r\n");
        local_138[0] = 0;
        bVar11 = ssp_ble_enqueue_tx_packet(0x10f, 1, local_138, '\0');
        if (0x80 < bVar11) {
          g_log_func("  ERROR SSPB place\r\n");
        }
      }
    }
    iVar13 = *(int *)0x20000004u;
    if (((iVar13 == 1) || (iVar13 == 5)) || (iVar13 == 0)) {
      iVar13 = internal_lipo_charge_step(ctx + 0x3fc);
      *(signed char *)(G_STATE + 0x18) = (signed char)iVar13;
      if (iVar13 == 0) {
        log_print_timestamp_prefix();
        g_log_func("Int LiPo decide: CPU stop\r\n");
        *(int *)0x20000004u = 10;
        G_STATE[4] = 0xf;
        if ((uint8_t)(*(signed char *)(ctx + 0x310) - 3U) < 2) {
          iVar13 = HAL_GPIO_ReadPin((void *)0x40020000u, 0x800);
          if (iVar13 == 0) {
            G_STATE[0x11] = 0;
          }
          else {
            G_STATE[0x11] = 3;
          }
        }
        else {
          iVar13 = HAL_GPIO_ReadPin((void *)0x40020000u, 0x800);
          if (iVar13 == 0) {
            iVar13 = bike_is_locked();
            if ((iVar13 == 0) || (*(signed char *)(ctx + 0x317) == '\0')) {
              G_STATE[0x11] = 2;
            }
            else {
              G_STATE[0x11] = 1;
            }
          }
          else {
            iVar13 = HAL_GPIO_ReadPin((void *)0x40020800u, 0x400);
            if (iVar13 == 0) {
              G_STATE[0x11] = 1;
            }
            else {
              G_STATE[0x11] = 2;
            }
          }
        }
      }
      else if (iVar13 == 1) {
        log_print_timestamp_prefix();
        g_log_func("Int LiPo decide: CPU stop 24h\r\n");
        *(int *)0x20000004u = 10;
        G_STATE[4] = 0xf;
        iVar13 = HAL_GPIO_ReadPin((void *)0x40020000u, 0x800);
        if (iVar13 == 0) {
          G_STATE[0x11] = 5;
        }
        else {
          G_STATE[0x11] = 5;
        }
      }
      else {
        scheduler_start(*(uint8_t *)(G_STATE + 0xf), 10000, 0);
      }
    }
    state_table_ptr_get(&local_128);
    iVar13 = bike_is_locked();
    if ((iVar13 != 0) && (*(signed char *)(ctx + 0x311) != '\0')) {
      locked_state_step(local_128, 1);
    }
    iVar13 = bike_is_locked();
    if (iVar13 == 0) {
      *(uint8_t *)(ctx + 0x311) = 0;
    }
    iVar13 = HAL_GPIO_ReadPin((void *)0x40020000u, 0x800);
    if ((((iVar13 == 0) || (iVar13 = HAL_GPIO_ReadPin((void *)0x40020800u, 0x400), iVar13 != 0)) ||
        (iVar13 = HAL_GPIO_ReadPin((void *)0x40020800u, 0x100), iVar13 == 0)) ||
       ((*(signed char *)(ctx + 0x340) != '\0' || (*(signed char *)(ctx + 0x312) != '\0')))) {
      iVar13 = bike_is_locked();
      if (((iVar13 != 0) &&
          ((*(signed char *)(ctx + 0x311) == '\0' &&
           (iVar13 = HAL_GPIO_ReadPin((void *)0x40020000u, 0x800), iVar13 == 0)))) &&
         (*(signed char *)(G_STATE + 0x1a) == -6)) {
        uVar10 = scheduler_alloc();
        *(uint8_t *)(G_STATE + 0x1a) = uVar10;
        scheduler_start(uVar10, 100, 0);
      }
      iVar13 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0x1a));
      if (iVar13 != 0) {
        scheduler_release((uint8_t *)0x20000043u);
        iVar17 = bike_is_locked();
        if (iVar17 != 0) {
          scheduler_release((uint8_t *)(G_STATE + 6));
          scheduler_release((uint8_t *)(G_STATE + 0xf));
          scheduler_release((uint8_t *)(G_STATE + 0x10));
          log_print_timestamp_prefix();
          g_log_func("Kick lock in standby\r\n");
          *(int *)0x20000004u = 10;
          maybe_set_state_if_unlocked('!');
        }
      }
      iVar13 = HAL_GPIO_ReadPin((void *)0x40020800u, 0x10);
      if (iVar13 == 0) {
        if (*(signed char *)(G_STATE + 0x1b) == -6) {
          uVar10 = scheduler_alloc();
          *(uint8_t *)(G_STATE + 0x1b) = uVar10;
          scheduler_start(uVar10, 1000, 0);
        }
      }
      else {
        scheduler_release((uint8_t *)0x20000044u);
      }
      iVar17 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0x1b));
      if (iVar17 != 0) {
        scheduler_release((uint8_t *)(G_STATE + 0x1b));
        log_print_timestamp_prefix();
        g_log_func("Charger detected Lights Off\r\n");
        *(uint8_t *)(ctx + 0x351) = 4;
        *(uint8_t *)(ctx + 0x350) = 4;
        *(uint8_t *)(ctx + 0x352) = 4;
        scheduler_release((uint8_t *)(G_STATE + 0xf));
        scheduler_release((uint8_t *)(G_STATE + 0x10));
        scheduler_release((uint8_t *)(G_STATE + 0x1c));
        scheduler_release((uint8_t *)(G_STATE + 0x1d));
        set_mode_state_byte(8);
        G_STATE[4] = 0x15;
      }
      if ((((*local_128 == '\x01') || (local_128[1] == '\x01')) || (local_128[2] == '\x06')) &&
         ((iVar13 = bike_is_locked(), iVar13 == 0 && (*(signed char *)(ctx + 0x310) == '\v')))) {
        log_print_timestamp_prefix();
        g_log_func("USER bike on\r\n");
        announce_records_reset(7);
        scheduler_release((uint8_t *)(G_STATE + 0xf));
        scheduler_release((uint8_t *)(G_STATE + 0x10));
        iVar13 = shifter_sm_get_step();
        if ((iVar13 == 0x11) || (iVar13 = shifter_sm_get_step(), iVar13 == 5)) {
          log_print_timestamp_prefix();
          g_log_func("Shifter on\r\n");
          shifter_sm_set_step_3();
        }
        G_STATE[4] = 0x24;
      }
    }
    else {
      maybe_set_state_if_unlocked('\a');
    }
    break;
  case 0xf:
    if (*(signed char *)(G_STATE + 0x25) == -6) {
      HAL_GPIO_WritePin(GPIOE_BASE,4,0);
      console_cmd_logout((char *)0x0);
      iVar13 = (int)ctx;
      uVar12 = bike_is_locked();
      *(uint8_t *)(iVar13 + 0x311) = uVar12;
      log_print_timestamp_prefix();
      g_log_func("Force Lights Off\r\n");
      obj_set_field34(0);
      obj_set_field38(0);
      led_channel3_set_brightness(0);
      iVar13 = (int)ctx;
      *(uint8_t *)(iVar13 + 0x316) = *(uint8_t *)(iVar13 + 0x3c9);
      if (*(signed char *)(iVar13 + 0x3cb) != '\0') {
        *(uint8_t *)(iVar13 + 0x316) = *(uint8_t *)(iVar13 + 0x3c9) | 0x80;
      }
      iVar13 = save_state_record_to_eeprom
                         (*(uint32_t *)(iVar13 + 0x310),*(uint32_t *)(iVar13 + 0x314),
                          *(uint32_t *)(iVar13 + 0x318),*(uint32_t *)(iVar13 + 0x31c),
                          *(uint32_t *)(iVar13 + 800),*(uint32_t *)(iVar13 + 0x324),
                          *(uint32_t *)(iVar13 + 0x328),*(uint32_t *)(iVar13 + 0x32c),
                          *(uint32_t *)(iVar13 + 0x330),*(uint32_t *)(iVar13 + 0x334),
                          *(uint32_t *)(iVar13 + 0x338),*(uint32_t *)(iVar13 + 0x33c),
                          *(uint32_t *)(iVar13 + 0x340),*(uint32_t *)(iVar13 + 0x344),
                          *(uint32_t *)(iVar13 + 0x348));
      if (iVar13 != 0) {
        g_log_func(" ERROR Save values\r\n");
      }
      display_send_init_cmd();
      lis3dh_config_motion_int(0,6);
      if (*(signed char *)(ctx + 0x314) != '\0') {
        local_138[0] = 1;
        bVar11 = ssp_ble_enqueue_tx_packet(0x112,1,local_138,'\0');
        if (0x80 < bVar11) {
          g_log_func("  ERROR SSPB place\r\n");
        }
      }
      *(uint16_t *)(G_CLK + 0x92) = 0;
      *(uint16_t *)(G_CLK + 0x90) = 0;
      uVar19 = maybe_enqueue_tx_message(0x15,4,(uint16_t *)(G_CLK + 0x90));
      if (0x10 < uVar19) {
        g_log_func("  ERROR SSPM place\r\n");
      }
      local_124[0] = 1;
      uVar19 = maybe_enqueue_tx_message(0x14,2,local_124);
      if (0x10 < uVar19) {
        g_log_func("  ERROR SSPM place\r\n");
      }
      *(uint16_t *)(ctx + 0x364) = 0;
      uVar19 = maybe_enqueue_tx_message(0xc,0,0,1);
      if (0x10 < uVar19) {
        g_log_func("  ERROR SSPM4 place\r\n");
      }
      batteryware_update_set_pending();
      uVar10 = scheduler_alloc();
      *(uint8_t *)(G_STATE + 0x25) = uVar10;
      scheduler_start(uVar10,600,0);
      *(uint8_t *)(G_STATE + 4) = 0x10;
    }
    break;
  case 0x10:
    if ((*(int *)(ctx + 0x3bc) != 0 || *(int *)(ctx + 0x3b8) != 0) &&
       (*(signed char *)(G_STATE + 0x26) == -6)) goto LAB_0802c26e;
    while (1) {
      iVar13 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0x25));
      if (((iVar13 != 0) &&
          ((iVar13 = battery_telemetry_state_get(), iVar13 == 5 && (*(signed char *)(G_STATE + 0x26) == -6)))) ||
         (iVar13 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0x26)), iVar13 != 0)) {
        scheduler_release((uint8_t *)(G_STATE + 0x26));
        scheduler_release((uint8_t *)(G_STATE + 0x25));
        if (-1 < *(short *)(ctx + 0x364)) {
          g_log_func("No MOTOR_SLEEP_MODE from motor 0x%04X\r\n",*(uint16_t *)(ctx + 0x364));
        }
        lis3dh_int1_clear();
        iVar13 = led_driver_enter_shipping_mode();
        if (iVar13 != 0) {
          g_log_func(" ERROR Display off\r\n");
        }
        stc_gas_gauge_set_run();
        enter_stop_mode(*(uint8_t *)(G_STATE + 0x11));
      }
      iVar13 = HAL_GPIO_ReadPin(GPIOC_BASE,4);
      if ((iVar13 == 0) || (*(signed char *)(ctx + 0x314) != '\0')) break;
      scheduler_release((uint8_t*)0x2000004eu);
      g_log_func("Appcon NVICReset\r\n");
      systick_delay(10);
      system_reset();
LAB_0802c26e:
      set_mode_state_byte(0x24);
      uVar10 = scheduler_alloc();
      *(uint8_t *)(G_STATE + 0x26) = uVar10;
      scheduler_start(uVar10,10000,0);
    }
    break;
  case 0x11:
    if (*(signed char *)(G_STATE + 0x3e) == -6) {
      uVar10 = scheduler_alloc();
      *(uint8_t *)(G_STATE + 0x3e) = uVar10;
      scheduler_start(uVar10,4000,0);
      set_mode_state_byte(0x1b);
      channel_notify_with_status(2);
    }
    iVar17 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0x3e));
    if (iVar17 != 0) {
      scheduler_release((uint8_t *)(G_STATE + 0x3e));
      set_mode_state_byte(0xf);
      *(uint8_t *)(G_STATE + 4) = 0xe;
    }
    state_table_ptr_get(&local_128);
    if ((*local_128 == '\x02') && (*(signed char *)(ctx + 0x310) != '\x04')) {
      announce_records_reset(3);
      scheduler_release((uint8_t *)(G_STATE + 0xf));
      scheduler_release((uint8_t *)(G_STATE + 0x10));
      if (*(short *)(ctx + 0x100) == 0xff) {
        log_print_timestamp_prefix();
        g_log_func("No backup code\r\n");
        *(uint8_t *)(G_STATE + 4) = 0x11;
      }
      else {
        *(uint8_t *)(G_STATE + 4) = 0x26;
      }
    }
    break;
  case 0x12:
    if (*(signed char *)(G_STATE + 0x3d) == -6) {
      uVar10 = scheduler_alloc();
      *(uint8_t *)(G_STATE + 0x3d) = uVar10;
      scheduler_start(uVar10,5000,0);
      g_log_func("Lights P1a\r\n");
      *(uint8_t *)(ctx + 0x351) = 3;
      *(uint8_t *)(ctx + 0x352) = 3;
      set_mode_state_byte(0x1a);
    }
    iVar17 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0x3d));
    if (iVar17 != 0) {
      scheduler_release((uint8_t *)(G_STATE + 0x3d));
      set_mode_state_byte(0xf);
      *(uint8_t *)(G_STATE + 4) = 0xe;
    }
    state_table_ptr_get(&local_128);
    if (*local_128 == '\x01') {
      announce_records_reset(3);
      log_print_timestamp_prefix();
      g_log_func("Auto wake request BLE unlock\r\n");
      local_138[0] = 1;
      bVar11 = ssp_ble_enqueue_tx_packet(0x5522,1,local_138,'\0');
      if (0x80 < bVar11) {
        g_log_func("  ERROR SSPB place\r\n");
      }
    }
    if (*local_128 == '\x02') {
      announce_records_reset(3);
      scheduler_release((uint8_t *)(G_STATE + 0x3d));
      log_print_timestamp_prefix();
      g_log_func("Auto wake do unlock\r\n");
      *(uint8_t *)(G_STATE + 4) = 0x26;
    }
    break;
  case 0x13:
    if (*(signed char *)(G_STATE + 0x2f) == -6) {
      channel_notify_with_status(0xe);
      set_mode_state_byte(0x12);
      uVar10 = scheduler_alloc();
      *(uint8_t *)(G_STATE + 0x2f) = uVar10;
      scheduler_start(uVar10,0xe74,0);
    }
    iVar13 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0x2f));
    if (iVar13 != 0) {
      bVar11 = *(signed char *)(G_CLK + 0x1a) + 1;
      *(uint8_t *)(G_CLK + 0x1a) = bVar11;
      if (bVar11 < 2) {
        scheduler_release((uint8_t*)0x20000058u);
      }
      else {
        set_mode_state_byte(0xf);
        *(uint8_t *)(G_STATE + 4) = 0xe;
      }
    }
    break;
  case 0x14:
    set_mode_state_byte(10);
    iVar13 = HAL_GPIO_ReadPin(GPIOC_BASE,0x10);
    if (iVar13 == 0) {
      scheduler_release((uint8_t*)0x20000052u);
    }
    else if (*(signed char *)(G_STATE + 0x29) == -6) {
      uVar10 = scheduler_alloc();
      *(uint8_t *)(G_STATE + 0x29) = uVar10;
      scheduler_start(uVar10,1000,0);
    }
    iVar17 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0x29));
    if (iVar17 != 0) {
      scheduler_release((uint8_t *)(G_STATE + 0x29));
      scheduler_release((uint8_t *)(G_STATE + 0x2a));
      log_print_timestamp_prefix();
      g_log_func("End intLiPo\r\n");
      set_mode_state_byte(0x1c);
      *(uint8_t *)(G_STATE + 4) = 7;
    }
    break;
  case 0x15:
    if (*(signed char *)(G_STATE + 0x1c) == -6) {
      if ((*(signed char *)(G_STATE + 0x13) != '\0') && (*(signed char *)(ctx + 0x3e0) != '\x01'))
      {
        channel_notify_with_status(0x12);
      }
      led_driver_set_shipping_mode(0xff);
      *(uint8_t *)(G_STATE + 0x27) = 0xff;
      uVar10 = scheduler_alloc();
      *(uint8_t *)(G_STATE + 0x1c) = uVar10;
      scheduler_start(uVar10,15000,0);
      iVar13 = HAL_GPIO_ReadPin(GPIOC_BASE,0x20);
      *(uint8_t *)(G_CLK + 0xa1) = (iVar13 != 0);
    }
    state_table_ptr_get(&local_128);
    if ((((*local_128 == '\x01') || (local_128[1] == '\x01')) || (local_128[2] == '\x01')) &&
       (((*(signed char *)(ctx + 0x3e0) == '\x01' && (iVar13 = bike_is_locked(), iVar13 == 0)) &&
        (*(signed char *)(ctx + 0x310) == '\v')))) {
      log_print_timestamp_prefix();
      g_log_func("USER bike on with powerbank in discharge mode\r\n");
      scheduler_release((uint8_t *)(G_STATE + 0xf));
      scheduler_release((uint8_t *)(G_STATE + 0x10));
      scheduler_release((uint8_t *)(G_STATE + 0x1d));
      *(uint16_t *)(ctx + 0x402) = 0;
      bms_write_reg8_and_poll();
      iVar13 = shifter_sm_get_step();
      if ((iVar13 == 0x11) || (iVar13 = shifter_sm_get_step(), iVar13 == 5)) {
        log_print_timestamp_prefix();
        g_log_func("re-Shifter on\r\n");
        *(uint8_t *)(G_CLK + 0x7c) = 0;
        shifter_sm_set_step_3();
      }
      *(uint8_t *)(G_STATE + 4) = 0x24;
    }
    iVar13 = bike_is_locked();
    if (iVar13 != 0) {
      scheduler_release((uint8_t*)0x20000051u);
      if (*(signed char *)(ctx + 0x3e0) == '\x01') {
        if (*(signed char *)(ctx + 0x311) == '\0') {
          uVar16 = 0;
        }
        else {
          uVar16 = 1;
        }
      }
      else {
        uVar16 = 0;
      }
      locked_state_step(local_128,uVar16);
    }
    if (*(signed char *)(ctx + 0x3e0) != '\x01') {
      announce_records_reset(7);
    }
    iVar13 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0x1c));
    if (iVar13 != 0) {
      if (0x14 < *(uint8_t *)(G_STATE + 0x27)) {
        systick_delay(4);
        *(signed char *)(G_STATE + 0x27) = *(signed char *)(G_STATE + 0x27) + -1;
        led_driver_set_shipping_mode(*(uint8_t *)(G_STATE + 0x27));  /* OEM passes the post-decrement value */
      }
      bVar11 = *(uint8_t *)(G_CLK + 0xa1);
      uVar19 = HAL_GPIO_ReadPin(GPIOC_BASE,0x20);
      if (bVar11 != uVar19) {
        iVar13 = HAL_GPIO_ReadPin(GPIOC_BASE,0x20);
        *(uint8_t *)(G_CLK + 0xa1) = (iVar13 != 0);
        log_print_timestamp_prefix();
        g_log_func("Wheel move\r\n");
        scheduler_release((uint8_t*)0x20000045u);
      }
      iVar13 = HAL_GPIO_ReadPin(GPIOC_BASE,8);
      if (iVar13 != 0) {
        lis3dh_int1_clear();
        scheduler_release((uint8_t*)0x20000045u);
      }
    }
    iVar13 = HAL_GPIO_ReadPin(GPIOA_BASE,0x800);
    if (((iVar13 == 0) || (iVar13 = HAL_GPIO_ReadPin(GPIOC_BASE,0x400), iVar13 != 0)) ||
       ((iVar13 = HAL_GPIO_ReadPin((void *)0x40020800,0x100), iVar13 == 0 ||
        ((*(signed char *)(ctx + 0x340) != '\0' || (*(signed char *)(ctx + 0x312) != '\0')))))
       ) {
      iVar13 = bike_is_locked();
      if ((iVar13 != 0) &&
         ((*(signed char *)(ctx + 0x311) == '\0' && (*(signed char *)(G_STATE + 0x1a) == -6)))) {
        uVar10 = scheduler_alloc();
        *(uint8_t *)(G_STATE + 0x1a) = uVar10;
        scheduler_start(uVar10,100,0);
      }
      iVar13 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0x1a));
      if (iVar13 != 0) {
        scheduler_release((uint8_t*)0x20000043u);
        iVar13 = bike_is_locked();
        if (iVar13 != 0) {
          scheduler_release((uint8_t*)0x2000002fu);
          maybe_set_state_if_unlocked(' ');
        }
      }
      if ((*(signed char *)(G_STATE + 0x1d) == -6) &&
         (*(uint16_t *)(ctx + 0x3fe) = 0x8300,
         *(short *)(ctx + 0x3f2) != 0)) {
        uVar10 = scheduler_alloc();
        *(uint8_t *)(G_STATE + 0x1d) = uVar10;
        scheduler_start(uVar10,10000,0);
        bms_modbus_read(6,1);
      }
      iVar13 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0x1d));
      if (iVar13 != 0) {
        scheduler_release((uint8_t*)0x20000046u);
        if ((*(short *)(ctx + 0x3fc) < 0x5a) &&
           (sVar3 = *(short *)(ctx + 0x3fe), sVar3 != -32000)) {
          if (sVar3 < 0xb) {
            if (*(uint8_t *)(G_CLK + 0xa2) < 2) {
              g_log_func("Possible error 21. Check again in 10 seconds\r\n");
              *(signed char *)(G_CLK + 0xa2) = *(signed char *)(G_CLK + 0xa2) + '\x01';
            }
            else {
              state_flags_set(0x200000,0);
            }
          }
          else {
            if (*(signed char *)(G_CLK + 0xa2) != '\0') {
              g_log_func("Possible error 21 recovered\r\n");
            }
            state_flags_clear(0x200000,0);
            *(uint8_t *)(G_CLK + 0xa2) = 0;
          }
        }
      }
      sVar3 = *(short *)(ctx + 0x3fe);
      if (sVar3 != -32000) {
        if ((*(short *)(ctx + 0x3fc) < 0x5a) || (10 < sVar3)) {
          *(uint8_t *)(G_STATE + 0x13) = 1;
        }
        else {
          *(uint8_t *)(G_STATE + 0x13) = 0;
        }
      }
      iVar13 = HAL_GPIO_ReadPin(GPIOC_BASE,0x10);
      if (iVar13 == 0) {
        scheduler_release((uint8_t*)0x20000052u);
      }
      else if (*(signed char *)(G_STATE + 0x29) == -6) {
        uVar10 = scheduler_alloc();
        *(uint8_t *)(G_STATE + 0x29) = uVar10;
        scheduler_start(uVar10,1000,0);
      }
      iVar13 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0x29));
      if (iVar13 != 0) {
        battery_on_detect_step(1);
        *(uint8_t *)(G_STATE + 0x13) = 1;
        scheduler_release((uint8_t *)(G_STATE + 0x1d));
        scheduler_release((uint8_t *)(G_STATE + 0x29));
        scheduler_release((uint8_t *)(G_STATE + 0x2a));
        log_print_timestamp_prefix();
        g_log_func("End of charge\r\n");
        iVar17 = ctx_flag_0x131_is_clear();
        if (iVar17 == 0) {
          if (*(signed char *)(G_STATE + 0x2b) == -6) {
            uVar10 = scheduler_alloc();
            *(uint8_t *)(G_STATE + 0x2b) = uVar10;
            scheduler_start(uVar10,0x5dc,0);
          }
        }
        else {
          announce_records_reset(7);
          set_mode_state_byte(0xf);
          *(uint8_t *)(G_STATE + 4) = 0xe;
        }
      }
      iVar13 = scheduler_slot_is_idle(*(uint8_t *)(G_STATE + 0x2b));
      if ((iVar13 != 0) && (iVar17 = ctx_flag_0x131_is_clear(), iVar17 != 0))
      {
        scheduler_release((uint8_t *)(G_STATE + 0x2b));
        set_mode_state_byte(0xf);
        *(uint8_t *)(G_STATE + 4) = 0xe;
      }
    }
    else {
      maybe_set_state_if_unlocked('\a');
    }
    break;
  case 0x17:
    iVar13 = diagnostics_run_step(ctx);
    if (iVar13 != 0) {
      log_print_timestamp_prefix();
      if (*(int *)(ctx + 0x3bc) == 0 && *(int *)(ctx + 0x3b8) == 0) {
        g_log_func("Diag ok\r\n");
        set_mode_state_byte(0x23);
        channel_notify_with_status(3);
      }
      else {
        g_log_func("Diag fail\r\n");
        set_mode_state_byte(0x22);
        channel_notify_with_status(2);
      }
      G_STATE[4] = 0x18;
    }
    break;
  case 0x18:
    if ((signed char)G_STATE[0x36] == -6) {
      uVar10 = scheduler_alloc();
      G_STATE[0x36] = uVar10;
      scheduler_start(uVar10,15000,0);
      announce_records_reset(7);
    }
    state_table_ptr_get(&local_128);
    iVar17 = scheduler_slot_is_idle(G_STATE[0x36]);
    if ((((iVar17 != 0) || (*local_128 == '\x01')) || (local_128[1] == '\x01')) ||
       (local_128[2] == '\x01')) {
      scheduler_release((uint8_t *)(G_STATE + 0x36));
      set_mode_state_byte(0xf);
      G_STATE[4] = 0xe;
    }
    break;
  case 0x19:
    set_mode_state_byte(0x14);
    scheduler_release((uint8_t *)(G_STATE + 0x34));
    scheduler_release((uint8_t *)(G_STATE + 0x35));
    break;
  case 0x1a:
    if ((signed char)G_STATE[0x34] == -6) {
      uVar10 = scheduler_alloc();
      G_STATE[0x34] = uVar10;
      scheduler_start(uVar10,0x2932e0,0);
      g_log_func("Lights P2c\r\n");
      *(uint8_t *)(ctx + 0x351) = 4;
      *(uint8_t *)(ctx + 0x352) = 4;
      *(uint8_t *)(ctx + 0x350) = 4;
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[0x34]);
    if (iVar13 != 0) {
      log_print_timestamp_prefix();
      g_log_func("OAD took too long\r\n");
      testmode_command_dispatch(0xc);
    }
    if ((signed char)G_STATE[0x35] == -6) {
      uVar10 = scheduler_alloc();
      G_STATE[0x35] = uVar10;
      scheduler_start(uVar10,500,0);
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[0x35]);
    if (iVar13 != 0) {
      scheduler_start(G_STATE[0x35],0x5dc,0);
      channel_notify_with_status(0x14);
    }
    set_mode_state_byte(0x13);
    break;
  case 0x1b:
    if ((signed char)G_STATE[0x37] == -6) {
      uVar10 = scheduler_alloc();
      G_STATE[0x37] = uVar10;
      scheduler_start(uVar10,5000,(sched_cb_t)reboot_restart_task);
      g_log_func("Update fail, Reboot on key\r\n");
      channel_notify_with_status(0x19);
    }
    state_table_ptr_get(&local_128);
    if (((*local_128 == '\x01') || (local_128[1] == '\x01')) || (local_128[2] == '\x01')) {
      reboot_restart_task();
    }
    break;
  case 0x1c:
    if ((signed char)G_STATE[0x38] == -6) {
      uVar10 = scheduler_alloc();
      G_STATE[0x38] = uVar10;
      scheduler_start(uVar10,5000,0);
    }
    iVar17 = scheduler_slot_is_idle(G_STATE[0x38]);
    if (iVar17 != 0) {
      scheduler_release((uint8_t *)(G_STATE + 0x38));
      G_STATE[4] = G_STATE[5];
    }
    set_mode_state_byte(0x17);
    break;
  case 0x1d:
    if ((signed char)G_STATE[0x37] == -6) goto LAB_0802d668;
    while (iVar13 = scheduler_slot_is_idle(G_STATE[0x37]), iVar13 != 0) {
      scheduler_release((uint8_t *)0x20000060u);
      g_log_func("NVICReset\r\n");
      systick_delay(10);
      system_reset();
LAB_0802d668:
      uVar10 = scheduler_alloc();
      G_STATE[0x37] = uVar10;
      scheduler_start(uVar10,5000,0);
      *(uint16_t *)(ctx + 0x342) = 0;
      iVar13 = save_state_record_to_eeprom
                         (*(uint32_t *)(ctx + 0x310),*(uint32_t *)(ctx + 0x314),
                          *(uint32_t *)(ctx + 0x318),*(uint32_t *)(ctx + 0x31c),
                          *(uint32_t *)(ctx + 800),*(uint32_t *)(ctx + 0x324),
                          *(uint32_t *)(ctx + 0x328),*(uint32_t *)(ctx + 0x32c),
                          *(uint32_t *)(ctx + 0x330),*(uint32_t *)(ctx + 0x334),
                          *(uint32_t *)(ctx + 0x338),*(uint32_t *)(ctx + 0x33c),
                          *(uint32_t *)(ctx + 0x340),*(uint32_t *)(ctx + 0x344),
                          *(uint32_t *)(ctx + 0x348));
      if (iVar13 != 0) {
        g_log_func(" ERROR Save values\r\n");
      }
      g_log_func("Update ok, Reboot\r\n");
      set_mode_state_byte(0x17);
      channel_notify_with_status(0x15);
    }
    break;
  case 0x1f:
    iVar13 = shifter_get_active_flag();
    if (iVar13 != 0) {
      if ((*(short *)(ctx + 0x520) == 0x201) &&
         (0x502 < *(short *)(ctx + 0x52a))) {
        log_print_timestamp_prefix();
        g_log_func("MT Shifter calibrate\r\n");
        shifter_sm_set_step_13();
      }
      else {
        log_print_timestamp_prefix();
        g_log_func("Shifter calibrate\r\n");
        {
            uint8_t mb_cmd[6] = { 0x20, 6, 0x14, 0, 1, 0 };
            iVar13 = modbus_shift_submit(mb_cmd);
        }
        if (iVar13 != 0) {
          log_print_timestamp_prefix();
          g_log_func("  ERR MS overflow\r\n");
        }
      }
    }
    set_mode_state_byte(0x19);
    G_STATE[4] = 0x22;
    break;
  case 0x20:
  case 0x21:
    if ((signed char)G_STATE[6] == -6) {
      g_log_func("Kick lock locked\r\n");
      uVar10 = scheduler_alloc();
      G_STATE[6] = uVar10;
      scheduler_start(uVar10,1000,0);
      scheduler_release((uint8_t *)(G_STATE + 0x1f));
      channel_notify_with_status(0xc);
      set_mode_state_byte(0x1b);
      *(uint8_t *)(ctx + 0x311) = 1;
      *(uint8_t *)(ctx + 0x340) = 1;
      iVar13 = save_state_record_to_eeprom
                         (*(uint32_t *)(ctx + 0x310),*(uint32_t *)(ctx + 0x314),
                          *(uint32_t *)(ctx + 0x318),*(uint32_t *)(ctx + 0x31c),
                          *(uint32_t *)(ctx + 800),*(uint32_t *)(ctx + 0x324),
                          *(uint32_t *)(ctx + 0x328),*(uint32_t *)(ctx + 0x32c),
                          *(uint32_t *)(ctx + 0x330),*(uint32_t *)(ctx + 0x334),
                          *(uint32_t *)(ctx + 0x338),*(uint32_t *)(ctx + 0x33c),
                          *(uint32_t *)(ctx + 0x340),*(uint32_t *)(ctx + 0x344),
                          *(uint32_t *)(ctx + 0x348));
      if (iVar13 != 0) {
        g_log_func(" ERROR Save values\r\n");
      }
      if ((signed char)G_STATE[4] != '!') {
        log_print_timestamp_prefix();
        g_log_func("Lights P3\r\n");
        *(uint8_t *)(ctx + 0x350) = 4;
        *(uint8_t *)(ctx + 0x351) = 4;
        *(uint8_t *)(ctx + 0x352) = 4;
      }
    }
    iVar13 = scheduler_slot_is_idle(G_STATE[6]);
    if (iVar13 != 0) {
      scheduler_release((uint8_t *)0x2000002fu);
      iVar17 = HAL_GPIO_ReadPin(GPIOC_BASE,0x10);
      if (iVar17 == 0) {
        scheduler_release((uint8_t *)(G_STATE + 0x1d));
        set_mode_state_byte(8);
        G_STATE[4] = 0x15;
      }
      else if ((signed char)G_STATE[4] == '!') {
        set_mode_state_byte(0xf);
        G_STATE[4] = 0xe;
      }
      else {
        channel_notify_with_status(0x11);
        G_STATE[4] = 0x1f;
      }
    }
    break;
  case 0x22:
    iVar13 = is_display_bus_ready();
    if (iVar13 != 0) {
      announce_records_reset(1);
      set_mode_state_byte(0xf);
      iVar13 = bike_is_locked();
      if (iVar13 == 0) {
        G_STATE[4] = 0xe;
      }
      else {
        G_STATE[4] = 0x23;
        shifter_firmware_update_step(*(uint8_t **)0x20000944u);
      }
    }
    break;
  case 0x23:
    if ((signed char)G_STATE[0x12] == -6) {
      uVar10 = scheduler_alloc();
      G_STATE[0x12] = uVar10;
      scheduler_start(uVar10,10000,0);
    }
    G_STATE[4] = 0xe;
    break;
  case 0x24:
    uVar19 = power_state_get_clamped();
    if (uVar19 < 2) {
      log_print_timestamp_prefix();
      g_log_func("SOC %d saved %d\r\n",
                 (int)*(short *)(*(uint8_t **)0x20000944u + 0x3fc),
                 (int)*(signed char *)(*(uint8_t **)0x20000944u + 0x315));
      G_STATE[4] = 0x25;
    }
    else {
      scheduler_release((uint8_t *)(G_STATE + 6));
      sched_timer_arm_or_alloc(5000);
      G_CLK[0x7d] = 1;
      log_print_timestamp_prefix();
      g_log_func("Lights P1\r\n");
      iVar17 = *(int *)0x20000944u;
      *(uint8_t *)(iVar17 + 0x351) = 3;
      *(uint8_t *)(iVar17 + 0x350) = 3;
      *(uint8_t *)(iVar17 + 0x352) = 3;
      set_mode_state_byte(5);
      G_STATE[4] = 0xc;
    }
    break;
  case 0x25:
    if ((signed char)G_STATE[0x2d] == -6) {
      uVar10 = scheduler_alloc();
      G_STATE[0x2d] = uVar10;
      scheduler_start(uVar10,5000,0);
    }
    set_mode_state_byte(0x11);
    iVar17 = scheduler_slot_is_idle(G_STATE[0x2d]);
    if (iVar17 != 0) {
      scheduler_release((uint8_t *)(G_STATE + 0x2d));
      G_CLK[0x7c] = 0;
      set_mode_state_byte(0xf);
      G_STATE[4] = 0xe;
    }
    break;
  case 0x26:
    announce_records_reset(3);
    G_CLK[0xaa] = (signed char)0xff;
    G_CLK[0xa9] = (signed char)0xff;
    G_CLK[0xa8] = (signed char)0xff;
    set_mode_state_byte(0x1e);
    if ((signed char)G_CLK[0xab] == '\0') {
      log_print_timestamp_prefix();
      g_log_func("Start PIN state machine at %d attempts\r\n",G_CLK[0xa6]);
      channel_notify_with_status(6);
    }
    G_CLK[0xab] = 1;
    iVar13 = gpio_pc0_is_low();
    if (iVar13 == 0) {
      G_CLK[0xa6] = (signed char)G_CLK[0xa6] + '\x01';
      announce_records_reset(1);
      G_STATE[4] = 0x28;
    }
    iVar13 = state_flags_test(0,0x100);
    if (iVar13 != 0) {
      log_print_timestamp_prefix();
      g_log_func("Horn stuck cannot use backup code\r\n");
      G_STATE[4] = 0x27;
    }
    break;
  case 0x27:
    if ((signed char)G_STATE[0x39] == -6) {
      set_mode_state_byte(0x24);
      uVar10 = scheduler_alloc();
      G_STATE[0x39] = uVar10;
    }
    iVar17 = scheduler_slot_is_idle(G_STATE[0x39]);
    if (iVar17 != 0) {
      scheduler_release((uint8_t *)(G_STATE + 0x39));
      G_STATE[4] = 0xe;
    }
    break;
  case 0x28:
    G_CLK[0xab] = 0;
    set_mode_state_byte(0x1e);
    iVar13 = lock_blink_sequence_step(iVar8 + 0xa8);
    if (iVar13 == 1) {
      G_STATE[4] = 0x29;
      log_print_timestamp_prefix();
      g_log_func("Got %d\r\n",*(uint8_t *)(iVar8 + 0xa8));
      channel_notify_with_status(3);
    }
    iVar13 = lock_blink_sequence_step(0x20000280);
    if (iVar13 == 2) {
      G_STATE[4] = 0x2e;
    }
    break;
  case 0x29:
    set_mode_state_byte(0x1f);
    iVar13 = lock_blink_sequence_step(0x20000281);
    if (iVar13 == 1) {
      G_STATE[4] = 0x2a;
      log_print_timestamp_prefix();
      g_log_func("Got %d\r\n",G_CLK[0xa9]);
      channel_notify_with_status(3);
    }
    iVar13 = lock_blink_sequence_step(0x20000281);
    if (iVar13 == 2) {
      G_STATE[4] = 0x2e;
    }
    break;
  case 0x2a:
    set_mode_state_byte(0x20);
    iVar13 = lock_blink_sequence_step(0x20000282);
    if (iVar13 == 1) {
      G_STATE[4] = 0x2b;
      log_print_timestamp_prefix();
      g_log_func("Got %d\r\n",G_CLK[0xaa]);
      channel_notify_with_status(3);
    }
    iVar13 = lock_blink_sequence_step(0x20000282);
    if (iVar13 == 2) {
      G_STATE[4] = 0x2e;
    }
    break;
  case 0x2b:
    uVar19 = (uint32_t)*(uint16_t *)(*(uint8_t **)0x20000944u + 0x100);
    if ((((uint32_t)(uint8_t)G_CLK[0xa8] ==
          (uint32_t)((uint64_t)0x51eb851f * (uint64_t)uVar19 >> 0x25)) &&
        ((uint32_t)(uint8_t)G_CLK[0xa9] ==
         (uint32_t)((uint64_t)0xcccccccd *
                (uint64_t)
                ((uVar19 + (uint32_t)((uint64_t)0x51eb851f * (uint64_t)uVar19 >> 0x25) * -100) &
                0xffff) >> 0x23))) &&
       ((uint32_t)(uint8_t)G_CLK[0xaa] ==
        ((uVar19 + (uint32_t)((uint64_t)0xcccccccd * (uint64_t)uVar19 >> 0x23) * -10) & 0xffff))) {
      G_STATE[4] = 0x2c;
    }
    else {
      G_STATE[4] = 0x2e;
    }
    break;
  case 0x2c:
    iVar13 = is_display_bus_ready();
    if (iVar13 != 0) {
      set_mode_state_byte(0x21);
      G_STATE[4] = 0x2d;
    }
    break;
  case 0x2d:
    iVar13 = is_display_bus_ready();
    if (iVar13 != 0) {
      G_STATE[4] = 0x30;
    }
    break;
  case 0x2e:
    iVar13 = is_display_bus_ready();
    if (iVar13 != 0) {
      channel_notify_with_status(2);
      set_mode_state_byte(0x22);
      if ((signed char)G_STATE[0x3a] == -6) {
        uVar10 = scheduler_alloc();
        G_STATE[0x3a] = uVar10;
      }
      scheduler_start(G_STATE[0x3a],2000,0);
      G_STATE[4] = 0x2f;
    }
    break;
  case 0x2f:
    iVar17 = scheduler_slot_is_idle(G_STATE[0x3a]);
    if (iVar17 != 0) {
      scheduler_start(G_STATE[0x3a],0x14,0);
      if ((signed char)G_STATE[0x3b] == -6) {
        uVar10 = scheduler_alloc();
        G_STATE[0x3b] = uVar10;
      }
      scheduler_start(G_STATE[0x3b],5,0);
      G_STATE[4] = 0x36;
    }
    break;
  case 0x30:
    HAL_GPIO_WritePin(GPIOB_BASE,8,1);
    G_CLK[0xa6] = 0;
    G_CLK[0xac] = 5;
    *(uint8_t *)(ctx + 0x310) = 0xb;
    iVar13 = HAL_GPIO_ReadPin(GPIOC_BASE,0x100);
    if (iVar13 != 0) {
      if ((signed char)G_STATE[0x3a] == -6) {
        uVar10 = scheduler_alloc();
        G_STATE[0x3a] = uVar10;
      }
      scheduler_start(G_STATE[0x3a],1000,0);
      set_mode_state_byte(0x21);
      G_STATE[4] = 0x32;
      return;
    }
    G_STATE[4] = 0x31;
    /* fall through */
  case 0x31:
    if ((signed char)G_STATE[0x3c] == -6) {
      uVar10 = scheduler_alloc();
      G_STATE[0x3c] = uVar10;
      scheduler_start(uVar10,2000,0);
      channel_notify_with_status(0xd);
      set_mode_state_byte(0x21);
    }
    iVar17 = scheduler_slot_is_idle(G_STATE[0x3c]);
    if (iVar17 != 0) {
      if ((signed char)G_STATE[6] == -6) {
        G_STATE[4] = 0x34;
      }
      else {
        scheduler_release((uint8_t *)(G_STATE + 0x3c));
        set_mode_state_byte(6);
        G_STATE[4] = 0xc;
      }
      set_unlock_state_persist();
      HAL_GPIO_WritePin(GPIOB_BASE,8,0);
      systick_delay(0x14);
    }
    break;
  case 0x32:
    iVar13 = scheduler_slot_is_idle(G_STATE[0x3a]);
    if (iVar13 != 0) {
      if ((signed char)G_CLK[0xac] == '\x05') {
        channel_notify_with_status(4);
      }
      cVar2 = (signed char)G_CLK[0xac];
      G_CLK[0xac] = cVar2 + -1;
      matrix_draw_icon(cVar2,0xd);
      scheduler_start(G_STATE[0x3a],1000,0);
    }
    iVar13 = HAL_GPIO_ReadPin(GPIOC_BASE,0x100);
    if (iVar13 == 0) {
      set_mode_state_byte(0x21);
      if ((uint8_t)G_STATE[0x3c] == 0xfa) {
        uVar10 = scheduler_alloc();
        G_STATE[0x3c] = uVar10;
      }
      scheduler_start(G_STATE[0x3c],0x5dc,0);
      if ((uint8_t)G_STATE[0] == 0xfa) {
        uVar10 = scheduler_alloc();
        G_STATE[0] = uVar10;
      }
      scheduler_start(G_STATE[0],1000,(sched_cb_t)0x0802990dU);
      set_unlock_state_persist();
      channel_notify_with_status(0xd);
      uVar19 = power_state_get_clamped();
      if (uVar19 < 2) {
        G_STATE[4] = '%';
      }
      else {
        log_print_timestamp_prefix();
        g_log_func("Light P03\r\n");
        *(uint8_t *)(ctx + 0x351) = 7;
        *(uint8_t *)(ctx + 0x352) = 7;
        G_STATE[4] = '4';
      }
    }
    if ((signed char)G_CLK[0xac] == -1) {
      set_mode_state_byte(0x1b);
      if ((uint8_t)G_STATE[0x3a] == 0xfa) {
        uVar10 = scheduler_alloc();
        G_STATE[0x3a] = uVar10;
      }
      scheduler_start(G_STATE[0x3a],1000,0);
      log_print_timestamp_prefix();
      g_log_func("Kick lock timeout\r\n");
      HAL_GPIO_WritePin(GPIOB_BASE,8,0);
      channel_notify_with_status(0xc);
      *(uint8_t *)(ctx + 0x311) = 1;
      log_print_timestamp_prefix();
      g_log_func("Light P04\r\n");
      *(uint8_t *)(ctx + 0x351) = 4;
      *(uint8_t *)(ctx + 0x352) = 4;
      G_STATE[4] = '3';
    }
    break;
  case 0x33:
    iVar13 = scheduler_slot_is_idle(G_STATE[0x3a]);
    if (iVar13 != 0) {
      scheduler_release((uint8_t *)(G_STATE + 0x3a));
      set_mode_state_byte(0xf);
      G_STATE[4] = '\x0e';
    }
    break;
  case 0x34:
    iVar13 = scheduler_slot_is_idle(G_STATE[0x3c]);
    if (iVar13 != 0) {
      scheduler_release((uint8_t *)0x20000065u);
      set_mode_state_byte(5);
      channel_notify_with_status(0x10);
      uVar19 = power_state_get_clamped();
      if (uVar19 < 2) {
        G_STATE[4] = '%';
      }
      else {
        G_STATE[4] = '5';
      }
    }
    break;
  case 0x35:
    iVar13 = is_display_bus_ready();
    if (iVar13 != 0) {
      log_print_timestamp_prefix();
      g_log_func("Lights P1\r\n");
      *(uint8_t *)(ctx + 0x350) = 3;
      *(uint8_t *)(ctx + 0x351) = 3;
      *(uint8_t *)(ctx + 0x352) = 3;
      G_STATE[4] = '\f';
    }
    break;
  case 0x36:
    iVar13 = scheduler_slot_is_idle(G_STATE[0x3b]);
    if (iVar13 != 0) {
      scheduler_start(G_STATE[0x3b],5,0);
      led_driver_set_shipping_mode(G_CLK[0xae]);
      sVar3 = *(short *)(G_CLK + 0xae);
      if (sVar3 != 0) {
        *(short *)(G_CLK + 0xae) = sVar3 + -1;
      }
    }
    if (*(short *)(G_CLK + 0xae) == 0) {
      if ((signed char)G_CLK[0xa6] == '\x03') {
        if ((uint8_t)G_STATE[0x32] == 0xfa) {
          log_print_timestamp_prefix();
          g_log_func("3 times failed now wait %d seconds\r\n",10);
          uVar10 = scheduler_alloc();
          G_STATE[0x32] = uVar10;
          scheduler_start(uVar10,10000,0);
          if (*(uint8_t *)(ctx + 0x310) < 5) {
            switch(*(uint8_t *)(ctx + 0x310)) {
            case 0:
              G_STATE[4] = '\0';
              break;
            case 1:
              G_STATE[4] = '\x01';
              break;
            case 2:
              G_STATE[4] = '\x02';
              break;
            case 3:
              G_STATE[4] = '\x03';
              break;
            case 4:
              G_STATE[4] = '\x04';
            }
            g_log_func("Go back to alarm %d\r\n",G_STATE[4]);
            led_driver_set_shipping_mode(0xff);
          }
        }
        iVar13 = scheduler_slot_is_idle(G_STATE[0x32]);
        if (iVar13 != 0) {
          scheduler_release((uint8_t *)0x2000005bu);
          G_CLK[0xa6] = 0;
          announce_records_reset(3);
        }
      }
      else {
        G_STATE[4] = '7';
        scheduler_start(G_STATE[0x3a],0x14,0);
        reset_dual_buffers_and_flags();
      }
    }
    break;
  case 0x37:
    iVar13 = scheduler_slot_is_idle(G_STATE[0x3a]);
    if (iVar13 != 0) {
      if (*(uint8_t *)(ctx + 0x310) < 5) {
        G_STATE[4] = *(uint8_t *)(ctx + 0x310);
        g_log_func("Go back to alarm %d\r\n");
      }
      else {
        if ((signed char)G_STATE[0xf] == -6) {
          uVar10 = scheduler_alloc();
          G_STATE[0xf] = uVar10;
        }
        scheduler_start(G_STATE[0xf],10000,0);
        set_mode_state_byte(0xf);
        G_STATE[4] = 0xe;
      }
      led_driver_set_shipping_mode(0xff);
    }
    break;
  case 0x38:
    if ((signed char)G_STATE[0x3a] == -6) {
      uVar10 = scheduler_alloc();
      G_STATE[0x3a] = uVar10;
    }
    scheduler_start(G_STATE[0x3a],0x14,0);
    if ((signed char)G_STATE[0x3b] == -6) {
      uVar10 = scheduler_alloc();
      G_STATE[0x3b] = uVar10;
    }
    scheduler_start(G_STATE[0x3b],0x19,0);
    led_driver_set_shipping_mode(0xff);
    set_mode_state_byte(0x21);
    G_STATE[4] = 0x39;
    break;
  case 0x39:
    set_mode_state_byte(0x1b);
    G_STATE[4] = 0x3a;
    scheduler_start(G_STATE[0x3a],1000,0);
    break;
  case 0x3a:
    iVar13 = scheduler_slot_is_idle(G_STATE[0x3a]);
    if (iVar13 != 0) {
      G_STATE[4] = 0x3b;
    }
    break;
  case 0x3b:
    iVar13 = scheduler_slot_is_idle(G_STATE[0x3b]);
    if (iVar13 != 0) {
      scheduler_start(G_STATE[0x3b],5,0);
      led_driver_set_shipping_mode(G_CLK[0xae]);
      *(short *)(G_CLK + 0xae) = *(short *)(G_CLK + 0xae) + -1;
    }
    if (*(short *)(G_CLK + 0xae) == 0) {
      set_mode_state_byte(0xf);
      G_STATE[4] = 0x3c;
      scheduler_start(G_STATE[0x3a],0x14,0);
    }
    break;
  case 0x3c:
    iVar13 = scheduler_slot_is_idle(G_STATE[0x3a]);
    if (iVar13 != 0) {
      G_STATE[4] = 0xe;
      led_driver_set_shipping_mode(0xff);
    }
    break;
  case 0x3d:
    iVar17 = scheduler_slot_is_idle(G_STATE[10]);
    if (iVar17 != 0) {
      scheduler_release((uint8_t *)(G_STATE + 10));
      G_STATE[4] = G_STATE[5];
      set_mode_state_byte(G_STATE[9]);
    }
  }
  return;
}

/* power_assist_gear_step (OEM 0x0802A304) — the boost-button (PC1) power-assist
 * "gear"/level selector and its on-screen indicator. Holding boost cycles the
 * power-assist level at ctx+0x3CA (1..4, wrapping past 4 back to 0); each level
 * maps to an assist parameter written to ctx+0x354 (180/120/60/30). The selected
 * level is drawn on the LED matrix for ~2 s by a small display state machine in
 * G_CLK[0x14]. VanMoof labels this "gear"/"power level" interchangeably ("Start
 * gear", "Power level %d"). State bytes:
 *   G_STATE[0xB] last level shown, [0xC] display timer, [0xD] boost-hold timer,
 *                [0xE] auto-advance timer  (0xFA/-6 = no slot)
 *   G_CLK[0x14]  display SM (0=idle, 1..4), [0x15] last PC1 level, [0x16] fast-repeat,
 *                [0x17] change-allowed (speed < 0.9 km/h), [0x18] advancing
 *   ctx+0x3C2 speed (0.1 km/h), +0x3C9 saved level, +0x3CB decel flag, +0x372 wheel word.
 * Returns non-zero while the on-screen display SM is active. */
int power_assist_gear_step(uint8_t *ctx)
{
    /* Level changed elsewhere while the indicator is idle -> pop it up for 2 s. */
    if ((uint8_t)G_STATE[0xb] != ctx[0x3ca] && G_CLK[0x14] == 0) {
        G_STATE[0xb] = (signed char)ctx[0x3ca];
        if (G_STATE[0xc] == -6) {
            G_STATE[0xc] = scheduler_alloc();
        }
        scheduler_start(G_STATE[0xc], 2000, 0);
        G_CLK[0x14] = 1;
        g_log_func("Start gear\r\n");
    }

    /* Boost held at standstill (ctx+0x372 == 0) with the decel flag set -> fast repeat. */
    if (ctx[0x3cb] != 0 && gpio_pc1_is_low() != 0 &&
        G_CLK[0x15] == 0 && *(int16_t *)(ctx + 0x372) == 0) {
        G_CLK[0x16] = 1;
    }

    /* Boost pressed below 0.9 km/h -> allow a level change this cycle. */
    if (gpio_pc1_is_low() != 0 && G_CLK[0x15] == 0 && *(uint16_t *)(ctx + 0x3c2) < 9) {
        G_CLK[0x17] = 1;
    }
    if (*(uint16_t *)(ctx + 0x3c2) > 9) {
        G_CLK[0x17] = 0;
    }

    G_CLK[0x15] = (signed char)gpio_pc1_is_low();

    if ((G_CLK[0x17] == 0 && G_CLK[0x16] == 0) ||
        gpio_pc1_is_low() == 0 || state_flags_test(0, 0x200) != 0) {
        /* Release / commit: if a level was being shown, latch + announce it. */
        if (G_STATE[0xe] != -6) {
            ctx[0x3c9] = ctx[0x3ca];
            log_print_timestamp_prefix();
            g_log_func("Power level %d\r\n", ctx[0x3c9]);
            channel_notify_with_status(3);
        }
        G_CLK[0x18] = 0;
        G_CLK[0x16] = 0;
        scheduler_release((uint8_t *)(G_STATE + 0xe));
        scheduler_release((uint8_t *)(G_STATE + 0xd));
    } else {
        /* Boost held: after an initial delay, auto-advance the level. */
        if (G_STATE[0xd] == -6) {
            G_STATE[0xd] = scheduler_alloc();
            scheduler_start(G_STATE[0xd], G_CLK[0x16] == 0 ? 3000 : 1000, 0);
        }
        if (scheduler_slot_is_idle(G_STATE[0xd]) != 0 && G_STATE[0xe] == -6) {
            ctx[0x3ca] = ctx[0x3c9];
            G_STATE[0xb] = (signed char)0xfe;
            G_STATE[0xe] = scheduler_alloc();
            G_CLK[0x18] = 1;
            scheduler_start(G_STATE[0xe], G_CLK[0x16] == 0 ? 0x5dc : 0x514, 0);
        }
        if (*(int16_t *)(ctx + 0x372) != 0) {
            scheduler_release((uint8_t *)(G_STATE + 0xd));
        }
        if (scheduler_slot_is_idle(G_STATE[0xe]) != 0) {
            uint8_t lvl = (uint8_t)(ctx[0x3ca] + 1);
            ctx[0x3ca] = lvl;
            if (lvl > 4) {
                ctx[0x3ca] = 0;
            }
            scheduler_start(G_STATE[0xe], G_CLK[0x16] == 0 ? 1000 : 800, 0);
            channel_notify_with_status(1);
        }
    }

    /* Assist parameter for the selected level. */
    switch (ctx[0x3ca]) {
    case 1:  *(uint16_t *)(ctx + 0x354) = 0xb4; break;
    case 2:  *(uint16_t *)(ctx + 0x354) = 0x78; break;
    case 3:  *(uint16_t *)(ctx + 0x354) = 0x3c; break;
    case 4:  *(uint16_t *)(ctx + 0x354) = 0x1e; break;
    default: *(uint16_t *)(ctx + 0x354) = 0;
    }

    /* On-screen level-indicator display state machine. */
    switch (G_CLK[0x14]) {
    case 1:
        G_CLK[0x14] = 2;
        announce_records_reset(2);
        break;
    case 2:
        set_mode_state_byte(0xd);
        G_CLK[0x14] = 3;
        break;
    case 3:
        if (is_display_bus_ready() != 0) {
            G_CLK[0x14] = 4;
        }
        break;
    case 4:
        if ((uint8_t)G_STATE[0xb] != ctx[0x3ca] && display_aux_byte_get() != 0) {
            if (ctx[0x3ca] == 0) {
                display_request_clear();
                reset_dual_buffers_and_flags();
            }
            G_STATE[0xb] = (signed char)ctx[0x3ca];
            matrix_draw_icon(ctx[0x3ca], 0xb);
            scheduler_start(G_STATE[0xc], 2000, 0);
        }
        set_mode_state_byte(0xe);
        if (scheduler_slot_is_idle(G_STATE[0xc]) != 0) {
            scheduler_release((uint8_t *)(G_STATE + 0xc));
            G_CLK[0x14] = 0;
            set_mode_state_byte(7);
        }
        break;
    }

    return G_CLK[0x14] != 0;
}

/* locked_state_step (OEM 0x0802A5C4) — the "locked" behaviour engine, run each
 * pass while the bike is in a locked state. It reacts to the lock-command table
 * (`state_tab` from state_table_ptr_get: [0] lock(1)/unlock(2) request, [1]
 * kick-lock, [2] remote-lock=6), the front lock sensor (PC2), the kickstand
 * (PA11) / charger (PC10) / plug (PC2) inputs and the motion sensor, deciding
 * whether to (re)assert the lock, ask the app to unlock over BLE (SSP 0x5522),
 * fall back to backup-code entry when no code is set, release on cartridge/kick
 * removal, or arm the alarm on a mems/wheel wake. `active` gates the
 * command-driven transitions. State: G_STATE[4]=bike state, [0xF]/[0x10]=lock
 * timers, [0x11]=release latch, [0x12]=alarm-wake slot; G_CLK[0x19]=last lock-
 * sensor level, [0x1A]=kick latch; the mode word at 0x20000004 is the wake mode. */
void locked_state_step(char *state_tab, int active)
{
    uint8_t *sctx = *(uint8_t **)0x20000944u;   /* session_ctx (cached by status_process) */
    uint8_t  buf[8];

    /* Remote/kick-lock request, or the lock sensor (PC2) engaged -> (re)lock. */
    if ((state_tab[2] == 6 || state_tab[1] == 1 ||
         (state_tab[0] == 1 && HAL_GPIO_ReadPin(GPIOC_BASE, 4) == 0)) &&
        sctx[0x310] != 4 && active != 0) {
        announce_records_reset(7);
        scheduler_release((uint8_t *)(G_STATE + 0xf));
        scheduler_release((uint8_t *)(G_STATE + 0x10));
        log_print_timestamp_prefix();
        g_log_func("Locked\r\n");
        G_STATE[4] = 0x11;
    }

    /* Lock-sensor (PC2) edge while a lock command is pending -> clear horn queue. */
    if ((uint8_t)G_CLK[0x19] != HAL_GPIO_ReadPin(GPIOC_BASE, 4) && state_tab[0] == 1) {
        log_print_timestamp_prefix();
        g_log_func("Clear horn queue\r\n");
        announce_records_reset(3);
    }
    G_CLK[0x19] = (signed char)(HAL_GPIO_ReadPin(GPIOC_BASE, 4) != 0);

    if (state_tab[0] == 1) {
        /* Lock command with the sensor released -> ask the app to unlock over BLE. */
        if (HAL_GPIO_ReadPin(GPIOC_BASE, 4) != 0 && active != 0) {
            announce_records_reset(3);
            log_print_timestamp_prefix();
            g_log_func("ASK APP to unlock\r\n");
            buf[0] = 1;
            if (ssp_ble_enqueue_tx_packet(0x5522, 1, buf, 0) > 0x80) {
                g_log_func("  ERROR SSPB place\r\n");
            }
        }
    } else if (state_tab[0] == 2 && active != 0) {
        /* Unlock request. */
        if (sctx[0x310] == 4) {
            G_STATE[4] = 4;
        } else {
            announce_records_reset(3);
            scheduler_release((uint8_t *)(G_STATE + 0xf));
            scheduler_release((uint8_t *)(G_STATE + 0x10));
            if (*(uint16_t *)(sctx + 0x100) == 0xff) {
                log_print_timestamp_prefix();
                g_log_func("No backupcode\r\n");
                G_STATE[4] = 0x11;
            } else {
                G_STATE[4] = 0x26;
            }
        }
    }

    /* Kickstand down / charging / plugged / latched -> stay locked (and maybe arm
     * the alarm); else the cartridge/kickstand was removed -> release the lock. */
    if (HAL_GPIO_ReadPin(GPIOA_BASE, 0x800) == 0 ||
        HAL_GPIO_ReadPin(GPIOC_BASE, 0x400) != 0 ||
        G_CLK[0x1a] != 0 ||
        HAL_GPIO_ReadPin(GPIOC_BASE, 4) != 0) {
        /* Arm the alarm on a motion/mems trigger while the alarm is enabled. */
        if ((HAL_GPIO_ReadPin(GPIOC_BASE, 8) != 0 ||
             *(int *)0x20000004u == 5 || *(int *)0x20000004u == 7) &&
            sctx[0x317] != 0 && G_STATE[0x12] == -6) {
            *(int *)0x20000004u = 10;
            log_print_timestamp_prefix();
            g_log_func("Locked wake by mems\r\n");
            if (maybe_get_bike_state() == 0x15 &&
                HAL_GPIO_ReadPin(GPIOD_BASE, 0x20) != 0 && sctx[0x3e0] != 1) {
                channel_notify_with_status(0x12);
            }
            scheduler_release((uint8_t *)(G_STATE + 0xf));
            scheduler_release((uint8_t *)(G_STATE + 0x10));
            if (sctx[0x312] != 0 || sctx[0x340] == 1) {
                switch (sctx[0x310]) {
                case 2:  G_STATE[4] = 2; break;
                case 3:  G_STATE[4] = 3; break;
                case 4:  G_STATE[4] = 4; break;
                default: G_STATE[4] = 0;
                }
            }
            lis3dh_int1_clear();
        }
    } else {
        announce_records_reset(4);
        log_print_timestamp_prefix();
        g_log_func("Cartridge removed\r\n");
        G_CLK[0x1a] = 0;
        G_STATE[4] = 0x13;
        G_STATE[0x11] = 1;
    }

    if (scheduler_slot_is_idle(G_STATE[0x12]) != 0) {
        lis3dh_int1_clear();
        scheduler_release((uint8_t *)(G_STATE + 0x12));
    }
}

/* set_unlock_state_persist (OEM 0x0802A95C) — commit the "unlocked" state to the
 * EEPROM state record. If the record already reads unlocked (session_ctx+0x310 ==
 * 0x0B) with the remote-lock (+0x312) and kick-lock (+0x340) flags clear, it is a
 * no-op; otherwise it logs "SET UNLOCK", forces state 0x0B and clears those two
 * flags, then writes the 15-word state record to both EEPROM copies (logs
 * " ERROR Save values" on failure). See docs/hardware.md "EEPROM map". */
void set_unlock_state_persist(void)
{
    uint8_t *sctx = *(uint8_t **)0x20000944u;   /* session_ctx (cached by status_process) */

    if (sctx[0x310] == 0xb && sctx[0x340] == 0 && sctx[0x312] == 0) {
        return;
    }
    g_log_func("SET UNLOCK\r\n");
    sctx[0x312] = 0;
    sctx[0x340] = 0;
    sctx[0x310] = 0xb;
    if (save_state_record_to_eeprom(
            *(uint32_t *)(sctx + 0x310), *(uint32_t *)(sctx + 0x314),
            *(uint32_t *)(sctx + 0x318), *(uint32_t *)(sctx + 0x31c),
            *(uint32_t *)(sctx + 0x320), *(uint32_t *)(sctx + 0x324),
            *(uint32_t *)(sctx + 0x328), *(uint32_t *)(sctx + 0x32c),
            *(uint32_t *)(sctx + 0x330), *(uint32_t *)(sctx + 0x334),
            *(uint32_t *)(sctx + 0x338), *(uint32_t *)(sctx + 0x33c),
            *(uint32_t *)(sctx + 0x340), *(uint32_t *)(sctx + 0x344),
            *(uint32_t *)(sctx + 0x348)) != 0) {
        g_log_func(" ERROR Save values\r\n");
    }
}

/* lock_blink_sequence_step (OEM 0x08029960) — drives the lock-indicator blink
 * sequence (up to 10 blinks) off the lock-command table. `counter` is the caller's
 * blink-count byte (0xFF = (re)start). Returns the sequence state via G_CLK[0]
 * (0 = arming, 1 = blink gap, 2 = done/settled). G_CLK[4] caches the state-table
 * pointer (state_table_ptr_get); G_STATE[1]/[2] are the gap/timeout timer slots. */
int lock_blink_sequence_step(uint8_t *counter)
{
    if (*counter == 0xff) {
        G_CLK[0] = 0;
        if (G_STATE[1] == -6) {
            G_STATE[1] = scheduler_alloc();
        }
        scheduler_start(G_STATE[1], 3000, 0);
        *counter = 0;
    }

    state_table_ptr_get((char **)(G_CLK + 4));
    if (**(char **)(G_CLK + 4) == 2) {
        **(char **)(G_CLK + 4) = 0;
    }
    if (**(char **)(G_CLK + 4) == 1) {
        **(char **)(G_CLK + 4) = 0;
        uint8_t n = *counter;
        *counter = (uint8_t)(n + 1);
        if ((uint8_t)(n + 1) > 9) {
            **(char **)(G_CLK + 4) = 0;
            scheduler_release((uint8_t *)(G_STATE + 2));
            scheduler_release((uint8_t *)(G_STATE + 1));
            G_CLK[0] = 2;
        }
        if (G_STATE[2] == -6) {
            G_STATE[2] = scheduler_alloc();
        }
        scheduler_start(G_STATE[2], 1000, 0);
        scheduler_start(G_STATE[1], 3000, 0);
    }

    if (scheduler_slot_is_idle(G_STATE[2]) != 0) {
        **(char **)(G_CLK + 4) = 0;
        scheduler_release((uint8_t *)(G_STATE + 2));
        scheduler_release((uint8_t *)(G_STATE + 1));
        G_CLK[0] = 1;
    }
    if (scheduler_slot_is_idle(G_STATE[1]) != 0) {
        **(char **)(G_CLK + 4) = 0;
        scheduler_release((uint8_t *)(G_STATE + 2));
        scheduler_release((uint8_t *)(G_STATE + 1));
        G_CLK[0] = 2;
    }
    return (uint8_t)G_CLK[0];
}

/* diagnostics_run_step (OEM 0x08030870) — the on-demand self-test sequence
 * (bike state 0x15). A 6-step machine (state byte at SRAM 0x20000650) that dumps
 * shifter/battery/motor status, then (after a 4 s settle) the full `show`, then an
 * I2C bus scan + ADC dump with the app-log sink enabled, and finally pushes the
 * logging-target byte (ctx+0x313) to the app over BLE char 0x55C1 ("Start Diag";
 * "diag_tmr"; "ERROR SSPB place2a" on TX-queue overflow). Returns non-zero on the
 * final step. Slot at SRAM 0x20000074. */
int diagnostics_run_step(uint8_t *ctx)
{
    uint8_t *state = (uint8_t *)0x20000650u;
    uint8_t *slot  = (uint8_t *)0x20000074u;
    uint8_t  buf[4];

    switch (*state) {
    case 0:
        g_log_func("Start Diag\r\n");
        set_mode_state_byte(0x15);
        channel_notify_with_status(0x13);
        *state = 1;
        break;
    case 1:
        console_cmd_shifterstatus((char *)0);
        console_cmd_battery((char *)0);
        console_cmd_motorstatus((char *)0);
        if (*slot == SCHED_SLOT_NONE) {
            *slot = scheduler_alloc();
            scheduler_set_timer_name(*slot, 4000, "diag_tmr");
        }
        scheduler_start(*slot, 4000, 0);
        *state = 2;
        break;
    case 2:
        if (scheduler_slot_is_idle(*slot) != 0) {
            console_cmd_show();
            *state = 3;
        }
        break;
    case 3:
        i2c_bus_scan(0);
        console_cmd_adc((char *)0);
        scheduler_start(*slot, 2000, 0);
        app_log_sink_enable();
        *state = 4;
        break;
    case 4:
        if (scheduler_slot_is_idle(*slot) != 0) {
            scheduler_release(slot);
            buf[0] = ctx[0x313];
            if (ssp_ble_enqueue_tx_packet(0x55c1, 1, buf, 0) > 0x80) {
                g_log_func("  ERROR SSPB place2a\r\n");
            }
            *state = 5;
        }
        break;
    case 5:
        *state = 0;
        break;
    }
    return *state == 5;
}

/* telemetry_datalog_emit (OEM 0x08036DB8) — the CSV datalog emitter. On a motor-
 * error change (while riding) it logs "Motor error %04X" and mirrors bits into the
 * fault flags. Every 30 s (when the updater is idle) it reads the STC3115 fuel
 * gauge and requests fresh module telemetry, then 100 ms later emits one CSV row
 * matching the header
 *   TIME;LiPOSOC;BMSSOC;BATTEMP;BATVOLTAGE;BATCURRENT;MOTORCURRENT;MOTORTMP;
 *   DRIVERTMP;SPEED;ODO;BOOST;LUX;BATDSG
 * (printed once). The OEM's reciprocal-multiply scalings are exact divisions by
 * 10/100/1000 — written as such here (behaviour-equivalent). Shared STC/telemetry
 * state block at SRAM 0x20005DB4; timer slots at 0x20000084 +5/+6. */
void telemetry_datalog_emit(uint8_t *ctx)
{
    uint8_t *mon  = (uint8_t *)0x20005db4u;
    uint8_t *slot = (uint8_t *)0x20000084u;

    if (*(int16_t *)(ctx + 0x364) != *(int16_t *)(mon + 0x34) &&
        maybe_get_bike_state() == 0xc) {
        log_print_timestamp_prefix();
        g_log_func("Motor error %04X\r\n", *(uint16_t *)(ctx + 0x364));
        if ((*(uint16_t *)(ctx + 0x364) & 4) == 0) {
            state_flags_clear(0, 0x2000);
        } else {
            state_flags_set(0, 0x2000);
        }
        if ((*(uint16_t *)(ctx + 0x364) & 0x2100) == 0x2100) {
            state_flags_set(0, 0x200000);
        } else {
            state_flags_clear(0, 0x200000);
        }
    }
    *(int16_t *)(mon + 0x34) = *(int16_t *)(ctx + 0x364);

    if (slot[5] == SCHED_SLOT_NONE) {
        slot[5] = scheduler_alloc();
        scheduler_start(slot[5], 6000, 0);
        stc_read(mon + 0x38, mon + 8);
        log_print_timestamp_prefix();
        g_log_func("LiPo SoC %d%% (first read)\r\n", *(int *)(mon + 0x10) / 10);
    }

    if (scheduler_slot_is_idle(slot[5]) != 0 && update_sm_is_idle() != 0) {
        scheduler_start(slot[5], 30000, 0);
        if (slot[6] == SCHED_SLOT_NONE) {
            slot[6] = scheduler_alloc();
            scheduler_set_timer_name(slot[6], 100, "result_tmr");
            scheduler_start(slot[6], 100, 0);
        }
        if (*(int16_t *)(ctx + 0x3fc) != -1 &&
            maybe_enqueue_tx_message(0xc, 0, 0, 1) > 0x10) {
            g_log_func("  ERROR SSPM place\r\n");
        }
        if (stc_read(mon + 0x38, mon + 8) == 1) {
            *(int16_t *)(ctx + 0x3d2) = (int16_t)*(int *)(mon + 0x10);
            *(int16_t *)(mon + 0x68) = 0;
        } else {
            *(int16_t *)(mon + 0x68) = (int16_t)(*(int16_t *)(mon + 0x68) + 1);
            g_log_func(" ERR Read STC\r\n");
        }
        if (*(uint16_t *)(mon + 0x68) < 5) {
            state_flags_clear(0, 0x40);
        } else {
            state_flags_set(0, 0x40);
        }
        if (mon[0x6a] == 0) {
            mon[0x6a] = 1;
            g_log_func("TIME;LiPOSOC;BMSSOC;BATTEMP;BATVOLTAGE;BATCURRENT;MOTORCURRENT;"
                       "MOTORTMP;DRIVERTMP;SPEED;ODO;BOOST;LUX;BATDSG\r\n");
        }
    }

    if (scheduler_slot_is_idle(slot[6]) != 0) {
        scheduler_release(&slot[6]);
        log_print_timestamp_prefix();

        int      lipo   = *(int *)(mon + 0x10);
        int      lipo_w = lipo / 10;
        int      temp   = *(int16_t *)(ctx + 0x3f8) - 0xaab;   /* 0.1 K -> 0.1 °C */
        int      temp_w = temp / 10;
        uint16_t vbat   = *(uint16_t *)(ctx + 0x3fa);
        unsigned vbat_w = vbat / 1000;

        int16_t  cur  = *(int16_t *)(ctx + 0x3fe);
        const char *sign = (((cur + 0x62) & 0xffff) < 0x62) ? "-" : "";
        int16_t  cur_w = (int16_t)(cur / 100);
        int16_t  cur_f = (int16_t)(cur - cur_w * 100);
        if (cur_f < 0) {
            cur_f = (int16_t)-cur_f;
        }

        uint16_t mcur   = *(uint16_t *)(ctx + 0x368);
        unsigned mcur_w = mcur / 10;
        int      mtmp   = *(int16_t *)(ctx + 0x36a);
        if (mtmp < -0x10d) {
            mtmp = 0;
        }
        int16_t  dtmp   = *(int16_t *)(ctx + 0x36c);
        uint16_t spd    = *(uint16_t *)(ctx + 0x3c2);
        unsigned spd_w  = spd / 10;
        uint32_t odo    = *(uint32_t *)(ctx + 0x31c);
        uint32_t odo_w  = odo / 10;

        int boost = gpio_pc1_is_low() ? 100 : 0;
        int lux   = light_sensor_read_step();
        int lux_v = (lux == 0xfffe) ? 0 : light_sensor_read_step();

        /* NB: the LiPOSOC field ends in a literal '%' (OEM format byte, single '%'
           passed to the non-printf-attributed g_log_func) — kept byte-faithful. */
        g_log_func(";%d.%d%;%d;%d.%d;%d.%02d;%s%d.%02d;%d.%d;%d;%d;%d.%d;%d.%d;%d;%d;%d\r\n",
                   lipo_w, lipo - lipo_w * 10,
                   (int)*(int16_t *)(ctx + 0x3fc),
                   temp_w, temp - temp_w * 10,
                   (int)vbat_w, (int)((vbat - vbat_w * 1000) / 10),
                   sign, (int)cur_w, (int)cur_f,
                   (int)mcur_w, (int)(mcur - mcur_w * 10),
                   mtmp, (int)dtmp,
                   (int)spd_w, (int)(spd - spd_w * 10),
                   (int)odo_w, (int)(odo - odo_w * 10),
                   boost, lux_v,
                   (int)*(int16_t *)(ctx + 0x402));
    }
}

/* testmode_command_dispatch (OEM 0x08029CA0) — the factory/service test-mode
 * command handler. Persists the state record to EEPROM first (clearing the
 * fw-update-order fields for any non-zero command), then: cmd 0 -> idle (bike
 * state 0x19); cmd 1 -> stream the identity blobs to the app (fw build "%d.%02d.%02d",
 * serial "%c.%d.%02d.%02d", "HW:%d", the batch string, shifter fw "%d.%d", BMS fw
 * "%X.%02X" over BLE chars 0x554A..0x5550) then state 0x1D; cmd 2..0xC -> inject a
 * single test fault bit into the 64-bit flag pair (ctx+0x3B8 low / +0x3BC high),
 * enter test drive-mode 0x18, state 0x1B. Session ctx via 0x20000944. */
void testmode_command_dispatch(int cmd)
{
    uint8_t *ctx = *(uint8_t **)0x20000944u;
    char buf96[96];
    char buf20[20];
    uint8_t hwver;

    if (cmd != 0) {
        *(uint32_t *)(ctx + 0x32c) = 0;
        *(uint16_t *)(ctx + 0x330) = 0;
    }
    if (save_state_record_to_eeprom(
            *(uint32_t *)(ctx + 0x310), *(uint32_t *)(ctx + 0x314),
            *(uint32_t *)(ctx + 0x318), *(uint32_t *)(ctx + 0x31c),
            *(uint32_t *)(ctx + 0x320), *(uint32_t *)(ctx + 0x324),
            *(uint32_t *)(ctx + 0x328), *(uint32_t *)(ctx + 0x32c),
            *(uint32_t *)(ctx + 0x330), *(uint32_t *)(ctx + 0x334),
            *(uint32_t *)(ctx + 0x338), *(uint32_t *)(ctx + 0x33c),
            *(uint32_t *)(ctx + 0x340), *(uint32_t *)(ctx + 0x344),
            *(uint32_t *)(ctx + 0x348)) != 0) {
        g_log_func(" ERROR Save values\r\n");
    }

    switch (cmd) {
    case 0:
        G_STATE[4] = 0x19;
        break;
    case 1: {
        uint32_t v = *(uint32_t *)0x08020004u;   /* fw build word from the image vector table */
        snprintf(buf20, 0x10, "%d.%02d.%02d", v >> 0x18, (v & 0xffffff) >> 0x10, (v & 0xffff) >> 8);
        if (ssp_ble_enqueue_tx_packet(0x554a, (uint16_t)strlen(buf20), buf20, 0) > 0x80) {
            g_log_func("  ERROR SSP place\r\n");
        }
        uint32_t sn = *(uint32_t *)(ctx + 0x388);
        snprintf(buf96, 0x60, "%c.%d.%02d.%02d",
                 sn >> 0x18, (sn & 0xffffff) >> 0x10, (sn & 0xffff) >> 8, sn & 0xff);
        if (ssp_ble_enqueue_tx_packet(0x554c, (uint16_t)strlen(buf96), buf96, 0) > 0x80) {
            g_log_func("  ERROR SSP place\r\n");
        }
        if (hw_version_lookup(&hwver) == 0) {
            g_log_func("  ERR HWversion\r\n");
        }
        snprintf(buf96, 0x60, "HW:%d", hwver);
        if (ssp_ble_enqueue_tx_packet(0x554d, (uint16_t)strlen(buf96), buf96, 0) > 0x80) {
            g_log_func("  ERROR SSP place\r\n");
        }
        snprintf(buf96, 0x60, "%s", (char *)(*(int *)(ctx + 1000) + 0x20));
        if (ssp_ble_enqueue_tx_packet(0x554e, (uint16_t)strlen(buf96), buf96, 0) > 0x80) {
            g_log_func("  ERROR SSP place\r\n");
        }
        snprintf(buf96, 0x60, "%d.%d",
                 *(uint16_t *)(ctx + 0x336) >> 8, (char)*(uint16_t *)(ctx + 0x336));
        if (ssp_ble_enqueue_tx_packet(0x554f, (uint16_t)strlen(buf96), buf96, 0) > 0x80) {
            g_log_func("  ERROR SSP place\r\n");
        }
        snprintf(buf96, 0x60, "%X.%02X",
                 *(uint16_t *)(ctx + 0x408) >> 8, (char)*(uint16_t *)(ctx + 0x408));
        if (ssp_ble_enqueue_tx_packet(0x5550, (uint16_t)strlen(buf96), buf96, 0) > 0x80) {
            g_log_func("  ERROR SSP place\r\n");
        }
        G_STATE[4] = 0x1d;
        break;
    }
    case 2:  *(uint32_t *)(ctx + 0x3b8) = 0x10000000; *(uint32_t *)(ctx + 0x3bc) = 0;
             set_mode_state_byte(0x18); G_STATE[4] = 0x1b; break;
    case 3:  *(uint32_t *)(ctx + 0x3b8) = 0x20000000; *(uint32_t *)(ctx + 0x3bc) = 0;
             set_mode_state_byte(0x18); G_STATE[4] = 0x1b; break;
    case 4:  *(uint32_t *)(ctx + 0x3b8) = 0x40000000; *(uint32_t *)(ctx + 0x3bc) = 0;
             set_mode_state_byte(0x18); G_STATE[4] = 0x1b; break;
    case 5:  *(uint32_t *)(ctx + 0x3b8) = 0x80000000; *(uint32_t *)(ctx + 0x3bc) = 0;
             set_mode_state_byte(0x18); G_STATE[4] = 0x1b; break;
    case 6:  *(uint32_t *)(ctx + 0x3b8) = 0; *(uint32_t *)(ctx + 0x3bc) = 1;
             set_mode_state_byte(0x18); G_STATE[4] = 0x1b; break;
    case 7:  *(uint32_t *)(ctx + 0x3b8) = 0; *(uint32_t *)(ctx + 0x3bc) = 2;
             set_mode_state_byte(0x18); G_STATE[4] = 0x1b; break;
    case 8:  *(uint32_t *)(ctx + 0x3b8) = 0; *(uint32_t *)(ctx + 0x3bc) = 4;
             set_mode_state_byte(0x18); G_STATE[4] = 0x1b; break;
    case 9:  *(uint32_t *)(ctx + 0x3b8) = 0; *(uint32_t *)(ctx + 0x3bc) = 8;
             set_mode_state_byte(0x18); G_STATE[4] = 0x1b; break;
    case 10: *(uint32_t *)(ctx + 0x3b8) = 0; *(uint32_t *)(ctx + 0x3bc) = 0x10;
             set_mode_state_byte(0x18); G_STATE[4] = 0x1b; break;
    case 11: *(uint32_t *)(ctx + 0x3b8) = 0; *(uint32_t *)(ctx + 0x3bc) = 0x20;
             set_mode_state_byte(0x18); G_STATE[4] = 0x1b; break;
    case 12: *(uint32_t *)(ctx + 0x3b8) = 0x4000000; *(uint32_t *)(ctx + 0x3bc) = 0;
             set_mode_state_byte(0x18); G_STATE[4] = 0x1b; break;
    }
}

/* is_user_reset_pending (OEM 0x08038EB0) — 1 when a user-requested reset is queued
 * (the flag byte at SRAM 0x20006E44 == 1). */
int is_user_reset_pending(void)
{
    return *(uint8_t *)0x20006e44u == 1;
}

/* set_wakeup_done_flag (OEM 0x080279C8) — bit-band set of bit 1 at SRAM 0x20007001
 * (the "wake handled" latch), written through its bit-band alias. */
void set_wakeup_done_flag(void)
{
    *(volatile uint32_t *)0x420e0024u = 1;
}

/* state_table_ptr_get (OEM 0x08040310) — hand back the pointer to the shared
 * lock/announce state-table block at SRAM 0x20009360. */
void state_table_ptr_get(char **out)
{
    *out = (char *)0x20009360u;
}

/* bike_is_locked (OEM 0x0802A8B0) — 1 when the bike is locked: the kickstand/lock
 * sensor on PC8 reads high, OR the remote-lock (session_ctx+0x312) or kick-lock
 * (session_ctx+0x340 == 1) flags are set. */
int bike_is_locked(void)
{
    uint8_t *ctx = *(uint8_t **)0x20000944u;

    if (HAL_GPIO_ReadPin(GPIOC_BASE, 0x100) == 0) {
        if (ctx[0x312] != 0) {
            return 1;
        }
        return (ctx[0x340] == 1) ? 1 : 0;
    }
    return 1;
}

/* sched_timer_arm_or_alloc (OEM 0x0802A014) — (alloc if needed and) arm the shared
 * G_STATE[7] scheduler slot for `period` ticks. */
void sched_timer_arm_or_alloc(uint32_t period)
{
    if (G_STATE[7] == -6) {
        G_STATE[7] = scheduler_alloc();
    }
    scheduler_start(G_STATE[7], period, 0);
}

/* ── scheduler-arming helpers for the status / display state machines ────────
 * announce_records_reset is called throughout status_process; the other three
 * are driven from the BLE command surface (ble_cmd_dispatch). */

/* Selectively reset the three button/announce state machines sharing the 6-byte
 * record block at SRAM 0x20009360 (OEM announce_records_reset, 0x0804031c). The
 * arg is a 3-bit mask: machine A (bit 0) resets to its re-arm state 5, B/C to 0;
 * dispatch-state bytes [3..5], phase bytes [0..2]. */
void announce_records_reset(int flags)
{
    volatile uint8_t *rec = (volatile uint8_t *)0x20009360u;

    if (flags & 1) { rec[3] = 5; rec[0] = 0; }   /* machine A -> idle/re-arm */
    if (flags & 2) { rec[4] = 0; rec[1] = 0; }   /* machine B */
    if (flags & 4) { rec[5] = 0; rec[2] = 0; }   /* machine C */
}

/* Enter the display "announce" mode 4 (OEM display_announce_enter, 0x0802f16c) —
 * the sibling of app.c's enter_mode3_arm_show_timer (same 0x20000068 state block
 * and "ssp_show_tmr" 4000-tick timer). Save the current mode as "previous"
 * (unless already 3/4), lazily allocate+name the timer, (re)start it, force the
 * display to mode 4, then clear the dual display buffers. 0xFA = unallocated slot
 * (ARM char is unsigned). */
void display_announce_enter(void)
{
    volatile uint8_t *st   = (volatile uint8_t *)0x20000068u;   /* [0]=mode, [7]=slot */
    volatile uint8_t *prev = (volatile uint8_t *)0x20000288u;   /* [4]=previous mode */
    uint8_t mode = st[0];

    if ((uint8_t)(mode - 3u) > 1u) {        /* mode not in {3,4} */
        prev[4] = mode;
    }
    if (st[7] == 0xFAu) {                    /* no slot allocated yet */
        uint8_t slot = scheduler_alloc();
        st[7] = slot;
        scheduler_set_timer_name(slot, 4000u, (const char *)0x08050820u);  /* "ssp_show_tmr" */
    }
    scheduler_start(st[7], 4000u, 0);
    st[0] = 4;                              /* announce mode */
    reset_dual_buffers_and_flags();
}

/* Arm/disarm the display-timeout scheduler task (OEM display_timeout_timer_set,
 * 0x0802e3d0). ticks==0 releases the slot; otherwise lazily allocate it and arm
 * for ticks*1000 scheduler ticks (seconds -> ms) with no callback. Slot byte at
 * the status ctx 0x20000029 + 0x16. */
void display_timeout_timer_set(uint16_t ticks)
{
    volatile uint8_t *slot = (volatile uint8_t *)(0x20000029u + 0x16u);

    if (ticks == 0) {
        scheduler_release((uint8_t *)slot);
        return;
    }
    if (*slot == 0xFAu) {
        *slot = scheduler_alloc();
    }
    scheduler_start(*slot, (uint32_t)ticks * 1000u, 0);
}

/* (Re)arm the lock-poll scheduler task (OEM lock_poll_timer_arm, 0x0802a03c).
 * Slot byte at the status ctx 0x20000029 + 8; lazily allocated, then started to
 * fire 100 ticks later (status_process polls the slot itself). */
void lock_poll_timer_arm(void)
{
    volatile uint8_t *slot = (volatile uint8_t *)(0x20000029u + 8u);

    if (*slot == 0xFAu) {
        *slot = scheduler_alloc();
    }
    scheduler_start(*slot, 100u, 0);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Low-power sleep + reset / factory-reset
 *
 *  Three OEM functions sourced together because they form the bike's
 *  power-down / restart surface:
 *    enter_stop_mode        0x080382D0 — arm wake sources, sleep (STOP/WFI),
 *                                        re-init the clock tree on wake, reboot
 *    reboot_restart_task    0x08038A68 — commit a reboot (clear marker, NVIC reset)
 *    factory_reset_sm_step  0x08038A90 — the 6-state USER/factory-reset machine
 * ════════════════════════════════════════════════════════════════════════ */

/* RCC (RM0430 §6) — only the AHB clock-enable registers we touch here. */
#define RCC_BASE     0x40023800u
#define RCC_AHB1ENR  0x30u   /* bits 0..7 = GPIOA..GPIOH clock enable */
#define RCC_AHB2ENR  0x34u   /* bit 7 = OTGFSEN */

/* STM32 HAL GPIO_InitTypeDef, as laid out on the OEM stack (4 words — these
 * calls never use the Alternate-function field). */
typedef struct {
    uint32_t Pin;
    uint32_t Mode;
    uint32_t Pull;
    uint32_t Speed;
} gpio_init_t;

/* HAL GPIO mode encodings (ST HAL values). The per-reason wake pins are armed
 * as EXTI interrupt sources; analog mode parks every other pin for low power. */
#define GPIO_MODE_ANALOG       3u
#define GPIO_MODE_IT_FALLING   0x10210000u
#define GPIO_MODE_IT_RISING    0x10110000u   /* == GPIO_MODE_IT_FALLING - 0x100000 */

/* Per-reason sleep/wake parameters in one runtime-populated RAM struct
 * @0x20000094: slot[reason] (u32) is the sleep duration in ms (its low u16 is
 * reused as the RTC wake period in seconds); p_clear at +0x24 is a
 * write-through pointer the OEM zeroes before sleeping. */
typedef struct { uint32_t slot[9]; uint32_t *p_clear; } sleep_ctx_t;
#define g_sleep_ctx        ((sleep_ctx_t *)0x20000094u)
#define g_stop_mode_rtc    ((void *)0x200099E4u)        /* RTC handle for the disable call */
#define K_MS_TO_MIN_MAGIC  0x88888889ull                /* reciprocal /60000 (×, then >>37) */
#define K_WARM_BOOT_MARKER 0x55AA55CFu                  /* RAM marker @0x20000000 */

/* The logger object at SRAM 0x20009D98 is a 3-slot fn-ptr table; log.h exposes
 * slot [0] as g_log_func (printf sink). Slot [1] is a single-byte sink and
 * slot [2] an alternate formatter, used on the reset SM's error paths. The
 * casts go through the table-pointer type (not object->function) to stay
 * -Wpedantic-clean. */
typedef void (*log_putc_t)(uint8_t c);
#define g_log_putc(c)  (((log_putc_t *)&g_log_func)[1])(c)
#define g_log_alt      (((log_func_t *)&g_log_func)[2])

/* reset-SM control + scheduler timer-slot blocks (SRAM). */
static uint8_t * const g_reset_sm    = (uint8_t *)0x20006E44u;  /* [0]=state(0..5) [1]=substep(1..6) */
static uint8_t * const g_timer_slots = (uint8_t *)0x200000BCu;  /* slot handles, 0xFA = free */

/* ── callees not already declared via the included headers ─────────────────*/
extern int  HAL_GPIO_Init();                       /* 0x080267D0 */
extern int  gas_gauge_reset(void);                 /* 0x080396C4 */
extern void settings_factory_reset(void *ctx, int mode); /* 0x0803FAD8 (app.c) */
extern void bus_tx_enqueue_byte(uint8_t b);        /* 0x0803639C */
extern int  bus_rx_byte_locked(uint8_t *out);      /* 0x080363EC */
extern void NVIC_SystemReset(void);                /* 0x08038A14 (noreturn) */
extern void reset_ble_timeout_cb(void);            /* 0x08038A38 — 2 s BLE-reset timer cb */
/* enter_stop_mode pre-sleep de-init cascade (HAL_*_DeInit veneers) */
extern void uart_handle_deinit_0(void);            /* 0x08033894 */
extern void uart_handle_deinit_1(void);            /* 0x080338A4 */
extern void shifter_usart3_reinit(void);           /* 0x080338B4 — 3rd UART veneer (shifter.c) */
extern void uart_handle_deinit_3(void);            /* 0x080338C4 */
extern void uart_handle_deinit_4(void);            /* 0x080338D4 */
extern void uart_handle_deinit_5(void);            /* 0x080338E4 */
extern void uart_handle_deinit_6(void);            /* 0x080338F4 */
extern void uart_handle_deinit_7(void);            /* 0x08033904 */
extern void i2c_handle_deinit(void);               /* 0x0803C8D4 */
extern void i2c3_handle_deinit(void);              /* 0x0803C8E4 */
extern void spi_handle_deinit(void);               /* 0x0803C614 */
extern void adc_handle_deinit(void);               /* 0x08032C94 */
extern void ahb1_periph_handle_deinit(void);       /* 0x080402D8 */
/* CMSIS NVIC + low-power + RTC-wakeup helpers */
extern void nvic_set_priority(int32_t irq_n, uint32_t preempt, uint32_t sub); /* 0x08027078 */
extern void nvic_enable_irq(int32_t irq_n);        /* 0x080270E0 */
extern void nvic_clear_pending_irq(int32_t irq_n); /* 0x0802714C */
extern void enter_low_power_wait(uint32_t pwr_cr_mode, int use_wfi); /* 0x08022DC4 (PWR STOP + WFI) */
extern void systick_irq_disable(void);             /* 0x0802332C */
extern void systick_irq_enable(void);              /* 0x0802333C */
extern void set_wakeup_done_flag(void);            /* 0x080279C8 */
extern void rtc_set_wakeup_seconds(uint16_t seconds); /* 0x08038088 */
extern int  rtc_wakeup_timer_disable(void *hrtc);  /* 0x08026FB0 */
extern void boot_init_warm(void);                  /* 0x0803DADC (static in main.c) */
extern void boot_init_cold(void);                  /* 0x0803DDE0 (static in main.c) */
extern void Error_Handler(void);                   /* 0x0803DDCC (panic.c) */
extern void system_software_reset(void) __attribute__((noreturn)); /* 0x080382AC (inlined NVIC reset) */

/* enter_stop_mode — low-power "STOP" entry / wake-arm sequencer (OEM 0x080382D0).
 * `reason` (0..8, else default) selects the wake-source map. After the entry
 * banner + per-reason wake string it: drives PB8 high / PB15 low, de-inits every
 * UART + I2C + SPI + ADC + the AHB1 peripheral, enables all 8 GPIO port clocks,
 * re-Inits every pin of GPIOA..H to analog low-power, then disables the clocks.
 * Per reason it arms a small EXTI wake-pin map + the RTC wakeup timer (skipped
 * for reason 6 "Shipping"), enters STOP via WFI (enter_low_power_wait), and on
 * wake re-initialises the clock tree (warm vs cold from the marker @0x20000000),
 * disables the RTC wakeup timer (Error_Handler on failure) and performs a clean
 * software reset (system_software_reset, noreturn). The enclosing do/while and
 * the default tail are structurally faithful but never iterate — the reset ends
 * the function. Behaviour-equivalent reconstruction; exact order of every
 * RCC/GPIO write preserved. */
void enter_stop_mode(uint8_t reason)
{
    volatile uint32_t *rcc   = (volatile uint32_t *)RCC_BASE;
    void              *GPIOx = GPIOB_BASE;     /* cached, reused across the switch */
    gpio_init_t        init;
    uint32_t           seed;                   /* per-pin packed EXTI mode seed */
    int                rc;

    log_print_timestamp_prefix();
    /* "EnterSTOPMode %d min ": minutes = duration_ms * 0x88888889 >> 37
     * (umull hi-word then >>5). The low 32 bits are a dead second %d the OEM
     * also passes. */
    {
        uint64_t prod = K_MS_TO_MIN_MAGIC * g_sleep_ctx->slot[reason];
        g_log_func("EnterSTOPMode %d min ",
                   (uint32_t)(prod >> 0x25), (uint32_t)prod);
    }

    switch (reason) {
    case 0:
    case 1:
        g_log_func("Wake:All\r\n");
        break;
    case 2:
        g_log_func("Wake:NoMems\r\n");
        break;
    case 3:
    case 4:
    case 5:
        if (lis3dh_powerdown() != 0)
            g_log_func("  ERR2 LIS3DH\r\n");
        g_log_func("Wake:RST\r\n");
        break;
    case 6:
        if (lis3dh_powerdown() != 0)
            g_log_func("  ERR2 LIS3DH\r\n");
        g_log_func("Wake:Shipping\r\n");
        break;
    case 7:
        g_log_func("Wake:No bat\r\n");
        break;
    case 8:
        if (lis3dh_powerdown() != 0)
            g_log_func("  ERR2 LIS3DH\r\n");
        g_log_func("Wake:ERROR\r\n");
        break;
    }

    /* common pre-sleep: drive PB8 high, PB15 low, settle, de-init peripherals */
    HAL_GPIO_WritePin(GPIOB_BASE, 0x100, 1);   /* PB8  */
    HAL_GPIO_WritePin(GPIOx, 0x8000, 0);       /* PB15 */
    systick_delay(0x14);
    uart_handle_deinit_0();
    uart_handle_deinit_1();
    shifter_usart3_reinit();                    /* 3rd UART de-init veneer */
    uart_handle_deinit_3();
    uart_handle_deinit_4();
    uart_handle_deinit_5();
    uart_handle_deinit_6();
    uart_handle_deinit_7();
    i2c_handle_deinit();
    i2c3_handle_deinit();
    spi_handle_deinit();
    adc_handle_deinit();
    ahb1_periph_handle_deinit();

    *g_sleep_ctx->p_clear = 0;                  /* clear the context pointer field (+0x24) */

    /* RCC: AHB2ENR bit7 off (OTGFS), enable AHB1ENR GPIOA..H clocks (bits 0..7).
     * Each enable is read-modify-write then read back (HAL clock-enable settle). */
    rcc[RCC_AHB2ENR / 4] &= 0xffffff7fu;
    rcc[RCC_AHB1ENR / 4] |= 0x01; (void)rcc[RCC_AHB1ENR / 4];
    rcc[RCC_AHB1ENR / 4] |= 0x02; (void)rcc[RCC_AHB1ENR / 4];
    rcc[RCC_AHB1ENR / 4] |= 0x04; (void)rcc[RCC_AHB1ENR / 4];
    rcc[RCC_AHB1ENR / 4] |= 0x08; (void)rcc[RCC_AHB1ENR / 4];
    rcc[RCC_AHB1ENR / 4] |= 0x10; (void)rcc[RCC_AHB1ENR / 4];
    rcc[RCC_AHB1ENR / 4] |= 0x20; (void)rcc[RCC_AHB1ENR / 4];
    rcc[RCC_AHB1ENR / 4] |= 0x40; (void)rcc[RCC_AHB1ENR / 4];
    rcc[RCC_AHB1ENR / 4] |= 0x80; (void)rcc[RCC_AHB1ENR / 4];

    /* park every pin of GPIOA..H: analog, no pull, Pin=0xFFFF (order A..H) */
    init.Mode  = GPIO_MODE_ANALOG;
    init.Speed = 3;
    init.Pull  = 0;
    init.Pin   = 0xffff;
    HAL_GPIO_Init(GPIOA_BASE, &init);
    HAL_GPIO_Init(GPIOx, &init);               /* GPIOB (cached) */
    HAL_GPIO_Init(GPIOC_BASE, &init);
    HAL_GPIO_Init(GPIOD_BASE, &init);
    HAL_GPIO_Init(GPIOE_BASE, &init);
    HAL_GPIO_Init(GPIOF_BASE, &init);
    HAL_GPIO_Init(GPIOG_BASE, &init);
    HAL_GPIO_Init(GPIOH_BASE, &init);

    /* disable the GPIO clocks again (GPIOC/bit2 is left enabled for the per-reason
     * re-Init that follows). Read-back on the bit2 set preserved as in the OEM. */
    rcc[RCC_AHB1ENR / 4] &= 0xfffffffeu;
    rcc[RCC_AHB1ENR / 4] &= 0xfffffffdu;
    rcc[RCC_AHB1ENR / 4] |= 0x04; (void)rcc[RCC_AHB1ENR / 4];
    rcc[RCC_AHB1ENR / 4] &= 0xfffffff7u;
    rcc[RCC_AHB1ENR / 4] &= 0xffffffefu;
    rcc[RCC_AHB1ENR / 4] &= 0xffffffdfu;
    rcc[RCC_AHB1ENR / 4] &= 0xffffffbfu;
    rcc[RCC_AHB1ENR / 4] &= 0xffffff7fu;

    /* per-reason EXTI wake-pin map on GPIOC (+GPIOD for 3/4/5/6/8), then the
     * shared arm/sleep/reboot tail at 'rearm'. */
    switch (reason) {
    case 0:
    case 1:
        do {
            seed = GPIO_MODE_IT_FALLING;
            init.Pin = 0x13;  init.Pull = 0; init.Speed = 2; init.Mode = seed;
            HAL_GPIO_Init(GPIOC_BASE, &init);
            init.Pin = 0x400; init.Pull = 2; init.Speed = 2; init.Mode = seed;
            HAL_GPIO_Init(GPIOC_BASE, &init);
            seed -= 0x100000;                  /* -> GPIO_MODE_IT_RISING */
            init.Pin = 0x100; init.Pull = 1; init.Speed = 2; init.Mode = seed;
            HAL_GPIO_Init(GPIOC_BASE, &init);
            init.Pin = 0xc;   init.Pull = 0; init.Speed = 2; init.Mode = seed;
            HAL_GPIO_Init(GPIOC_BASE, &init);
            nvic_set_priority(6, 3, 0);    nvic_enable_irq(6);
            nvic_set_priority(7, 3, 0);    nvic_enable_irq(7);
            nvic_set_priority(8, 3, 0);    nvic_enable_irq(8);
            nvic_set_priority(9, 3, 0);    nvic_enable_irq(9);
            nvic_set_priority(10, 3, 0);   nvic_enable_irq(10);
            nvic_set_priority(0x17, 3, 0); nvic_enable_irq(0x17);
            nvic_set_priority(0x28, 3, 0); nvic_enable_irq(0x28);
        rearm:
            systick_delay(0x32);
            nvic_clear_pending_irq(6);
            nvic_clear_pending_irq(7);
            nvic_clear_pending_irq(8);
            nvic_clear_pending_irq(9);
            nvic_clear_pending_irq(10);
            nvic_clear_pending_irq(0x17);
            nvic_clear_pending_irq(0x28);
            nvic_clear_pending_irq(0x29);
            wwdg_apb_clk_disable();
            systick_delay(100);
            if (reason != 6) {                 /* reason 6 "Shipping" parks with no RTC auto-wake */
                /* OEM reads the low u16 of slot[reason] (stride 4) as wake seconds */
                rtc_set_wakeup_seconds((uint16_t)g_sleep_ctx->slot[reason]);
            }
            set_wakeup_done_flag();
            systick_irq_disable();
            enter_low_power_wait(1, 1);        /* STOP mode, regulator low-power, WFI */
            systick_irq_enable();
            if (*(volatile uint32_t *)0x20000000u == K_WARM_BOOT_MARKER)
                boot_init_warm();
            else
                boot_init_cold();
            *(volatile uint32_t *)0x20000000u = 0;
            rc = rtc_wakeup_timer_disable(g_stop_mode_rtc);
            if (rc != 0)
                Error_Handler();
            system_software_reset();           /* noreturn (AIRCR system reset) */
        } while (1);

    case 2:
        seed = GPIO_MODE_IT_FALLING;
        init.Pin = 0x13;  init.Pull = 0; init.Speed = 2; init.Mode = seed;
        HAL_GPIO_Init(GPIOC_BASE, &init);
        init.Pin = 0x400; init.Pull = 2; init.Speed = 2; init.Mode = seed;
        HAL_GPIO_Init(GPIOC_BASE, &init);
        seed -= 0x100000;                      /* -> GPIO_MODE_IT_RISING */
        init.Pin = 0x100; init.Pull = 1; init.Speed = 2; init.Mode = seed;
        HAL_GPIO_Init(GPIOC_BASE, &init);
        init.Pin = 0x4;   init.Pull = 0; init.Speed = 2; init.Mode = seed;
        HAL_GPIO_Init(GPIOC_BASE, &init);
        nvic_set_priority(6, 3, 0);    nvic_enable_irq(6);
        nvic_set_priority(7, 3, 0);    nvic_enable_irq(7);
        nvic_set_priority(8, 3, 0);    nvic_enable_irq(8);
        nvic_set_priority(9, 3, 0);    nvic_enable_irq(9);
        nvic_set_priority(10, 3, 0);   nvic_enable_irq(10);
        nvic_set_priority(0x17, 3, 0); nvic_enable_irq(0x17);
        nvic_set_priority(0x28, 3, 0); nvic_enable_irq(0x28);
        goto rearm;

    case 3:
    case 4:
    case 5:
    case 6:
    case 8:
        seed = GPIO_MODE_IT_FALLING;
        init.Pin = 0x10; init.Pull = 0; init.Speed = 2; init.Mode = seed;
        HAL_GPIO_Init(GPIOC_BASE, &init);
        rcc[RCC_AHB1ENR / 4] |= 0x08; (void)rcc[RCC_AHB1ENR / 4];  /* re-enable GPIOD clk for PD2 */
        init.Pin = 0x4;  init.Pull = 0; init.Speed = 2; init.Mode = seed;
        HAL_GPIO_Init(GPIOD_BASE, &init);
        nvic_set_priority(6, 3, 0);    nvic_enable_irq(6);
        nvic_set_priority(7, 3, 0);    nvic_enable_irq(7);
        nvic_set_priority(8, 3, 0);    nvic_enable_irq(8);
        nvic_set_priority(9, 3, 0);    nvic_enable_irq(9);
        nvic_set_priority(10, 3, 0);   nvic_enable_irq(10);
        nvic_set_priority(0x17, 3, 0); nvic_enable_irq(0x17);
        nvic_set_priority(0x28, 3, 0); nvic_enable_irq(0x28);
        nvic_set_priority(0x17, 3, 0); nvic_enable_irq(0x17);   /* OEM arms EXTI9_5 twice */
        goto rearm;

    case 7:
        seed = GPIO_MODE_IT_FALLING;
        init.Pin = 0x13;  init.Pull = 0; init.Speed = 2; init.Mode = seed;
        HAL_GPIO_Init(GPIOC_BASE, &init);
        seed = GPIO_MODE_IT_RISING;
        init.Pin = 0x100; init.Pull = 1; init.Speed = 2; init.Mode = seed;
        HAL_GPIO_Init(GPIOC_BASE, &init);
        init.Pin = 0xc;   init.Pull = 0; init.Speed = 2; init.Mode = seed;
        HAL_GPIO_Init(GPIOC_BASE, &init);
        nvic_set_priority(6, 3, 0);    nvic_enable_irq(6);
        nvic_set_priority(7, 3, 0);    nvic_enable_irq(7);
        nvic_set_priority(8, 3, 0);    nvic_enable_irq(8);
        nvic_set_priority(9, 3, 0);    nvic_enable_irq(9);
        nvic_set_priority(10, 3, 0);   nvic_enable_irq(10);
        nvic_set_priority(0x17, 3, 0); nvic_enable_irq(0x17);
        goto rearm;

    default:
        systick_delay(0x14);
        system_software_reset();               /* noreturn */
    }
}

/* reboot_restart_task — commit a pending reboot (OEM 0x08038A68). Clears the
 * warm-boot marker so the bootloader does a cold init, logs "NVICReset", waits
 * 20 ms for the line to drain, then triggers a CPU reset via NVIC_SystemReset
 * (never returns). Armed as a scheduler callback by console_cmd_reboot (+600
 * ticks), the OAD-failed path in status_process, and the reset SM (state 5). */
void reboot_restart_task(void)
{
    *(volatile uint32_t *)0x20000000u = 0;     /* clear warm-boot marker -> cold boot */
    log_print_timestamp_prefix();
    g_log_func("NVICReset\r\n");
    systick_delay(0x14);                        /* 20 ms */
    NVIC_SystemReset();                         /* noreturn */
}

/* factory_reset_sm_step — FACTORY/USER-reset & power-cycle state machine
 * (OEM 0x08038A90). Ticked every super-loop from main() with `ctx` = the app
 * context base. g_reset_sm[0] is the main state (0->1->2->3->(4)->5);
 * g_reset_sm[1] is the sub-step (1..6) of the state-5 hardware-reset cascade.
 *
 *   0: arm the machine.
 *   1: when PD2 and PB5 both read low, arm the reset timer, log "USER Reset",
 *      move to state RESET(0x16)/mode LOW_SOC(0x25), enqueue BLE notify 0x11D.
 *   2: once the slot is idle (or a forced BLEWare version is pending), arm a
 *      2 s BLE-reset countdown or push the forced version; then gas_gauge_reset,
 *      settings_factory_reset, optional "Force end update", stamp the state
 *      record and persist its 15 words to EEPROM.
 *   3: PB9 high, "NVICReset", NVIC_SystemReset() (noreturn; the OEM physically
 *      falls through into state 4, which is therefore dead).
 *   4: arm/await the 6 s "reset_tmr" slot, then advance to 5.
 *   5: sub_step 1..6 = reboot-timer arm / BLE reset / reboot-timer re-arm /
 *      eShifter reset / BMS parameter reset ("\nReset BMS\r" + 2 s RX drain) /
 *      Motor reset; afterwards the machine returns to state 0.
 *
 * Verified against the OEM disassembly: control flow, ctx offsets, log strings,
 * GPIO writes, timer slots, and the 15-arg EEPROM save. */
void factory_reset_sm_step(uint8_t *ctx)
{
    void   *gpiob = GPIOB_BASE;       /* cached at top of the OEM body */
    uint8_t slot;
    int     i;
    uint8_t rx_byte;
    uint8_t ble_payload[1];

    switch (g_reset_sm[0]) {
    case 0:
        g_reset_sm[0] = 1;
        break;

    case 1:
        /* both wake pins low (PD2 == 0 && PB5 == 0) -> user-reset path */
        if (HAL_GPIO_ReadPin(GPIOD_BASE, 0x4) == 0 &&
            HAL_GPIO_ReadPin(GPIOB_BASE, 0x20) == 0) {
            if (g_timer_slots[1] == 0xFA) {
                slot = scheduler_alloc();
                g_timer_slots[1] = slot;
                scheduler_start(slot, 2000, 0);
            }
            if (scheduler_slot_is_idle(g_timer_slots[1]) != 0) {
                scheduler_start(g_timer_slots[1], 200, 0);
                log_print_timestamp_prefix();
                g_log_func("USER Reset\r\n");
                maybe_set_state_if_unlocked(0x16);   /* -> RESET   */
                set_mode_state_byte(0x25);           /* -> LOW_SOC */
                *(uint32_t *)(ctx + 0x38c) = 0;
                ble_payload[0] = 1;
                if (ssp_ble_enqueue_tx_packet(0x11d, 1, ble_payload, 0) > 0x80)
                    g_log_alt("  ERROR SSP place\r\n");
                g_reset_sm[0] = 2;
            }
        } else {
            scheduler_release(&g_timer_slots[1]);
        }
        break;

    case 2:
        if (scheduler_slot_is_idle(g_timer_slots[1]) != 0 ||
            *(int *)(ctx + 0x38c) != 0) {
            if (*(int *)(ctx + 0x38c) == 0) {
                g_log_func("Resetting the BLE in 2 seconds\r\n");
                if (g_timer_slots[0] == 0xFA)
                    g_timer_slots[0] = scheduler_alloc();
                scheduler_start(g_timer_slots[0], 2000, reset_ble_timeout_cb);
            } else {
                uint32_t v = *(uint32_t *)(ctx + 0x38c);
                channel_notify_with_status(7);
                HAL_GPIO_WritePin(GPIOD_BASE, 0x2000, 1);   /* PD13 high */
                sched_timer_arm_or_alloc(15000);
                g_log_func("BLEWare %d.%d.%02d\r\n",
                           (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
            }
            scheduler_start(g_timer_slots[1], 7000, 0);

            if (gas_gauge_reset() == 0)
                g_log_func("GasGauge_Reset\r\n");
            else
                g_log_func("ERROR GasGauge_Reset\r\n");

            settings_factory_reset(ctx, 0);

            if (*(char *)(ctx + 0x32c) != 0) {
                g_log_func("Force end update\r\n");
                *(uint32_t *)(ctx + 0x32c) = 0;
                *(uint16_t *)(ctx + 0x330) = 0;
            }

            *(uint8_t *)(ctx + 0x318) = 10;
            *(uint8_t *)(ctx + 0x316) = 4;

            if (save_state_record_to_eeprom(
                    *(uint32_t *)(ctx + 0x310), *(uint32_t *)(ctx + 0x314),
                    *(uint32_t *)(ctx + 0x318), *(uint32_t *)(ctx + 0x31c),
                    *(uint32_t *)(ctx + 0x320), *(uint32_t *)(ctx + 0x324),
                    *(uint32_t *)(ctx + 0x328), *(uint32_t *)(ctx + 0x32c),
                    *(uint32_t *)(ctx + 0x330), *(uint32_t *)(ctx + 0x334),
                    *(uint32_t *)(ctx + 0x338), *(uint32_t *)(ctx + 0x33c),
                    *(uint32_t *)(ctx + 0x340), *(uint32_t *)(ctx + 0x344),
                    *(uint32_t *)(ctx + 0x348)) != 0)
                g_log_func(" ERROR Save values\r\n");

            g_reset_sm[0] = 3;
        }
        break;

    case 3:
        if (scheduler_slot_is_idle(g_timer_slots[1]) != 0) {
            HAL_GPIO_WritePin(GPIOB_BASE, 0x200, 1);   /* PB9 high */
            log_print_timestamp_prefix();
            g_log_func("NVICReset\r\n");
            systick_delay(10);
            NVIC_SystemReset();                        /* noreturn */
        }
        /* OEM physically falls through into state 4 here; unreachable because
         * NVIC_SystemReset() never returns. When the slot is NOT idle the OEM
         * branches straight to the function exit (this break). */
        break;

    case 4:
        if (g_timer_slots[2] == 0xFA) {
            slot = scheduler_alloc();
            g_timer_slots[2] = slot;
            scheduler_start(slot, 6000, 0);
            scheduler_set_timer_name(g_timer_slots[2], 6000, "reset_tmr");
        }
        if (scheduler_slot_is_idle(g_timer_slots[2]) != 0) {
            scheduler_release(&g_timer_slots[2]);
            g_reset_sm[0] = 5;
        }
        break;

    case 5:
        switch (g_reset_sm[1]) {
        case 1:
            HAL_GPIO_WritePin(GPIOB_BASE, 0x4000, 0);  /* PB14 low  */
            HAL_GPIO_WritePin(GPIOE_BASE, 0x20, 1);    /* PE5  high */
            HAL_GPIO_WritePin(gpiob, 0x200, 1);        /* PB9  high */
            HAL_GPIO_WritePin(gpiob, 0x20, 1);         /* PB5  high */
            slot = scheduler_alloc();
            g_timer_slots[3] = slot;
            scheduler_set_timer_name(slot, 4000, "reboot_tmr");
            scheduler_start(g_timer_slots[3], 4000, reboot_restart_task);
            break;
        case 2:
            g_log_func("BLE reset\r\n");
            HAL_GPIO_WritePin(GPIOE_BASE, 0x20, 1);    /* PE5 pulse */
            systick_delay(10);
            HAL_GPIO_WritePin(GPIOE_BASE, 0x20, 0);
            break;
        case 3:
            slot = scheduler_alloc();
            g_timer_slots[3] = slot;
            scheduler_set_timer_name(slot, 500, "reboot_tmr");
            scheduler_start(g_timer_slots[3], 500, reboot_restart_task);
            break;
        case 4:
            g_log_func("Eshifter reset\r\n");
            HAL_GPIO_WritePin(GPIOB_BASE, 0x4000, 0);  /* PB14 low */
            systick_delay(10);
            shifter_sm_set_step_3();
            break;
        case 5:
            g_log_func("BMS parameter reset\r\n");
            bus_tx_enqueue_byte(0x0A);                 /* "\nReset BMS\r" */
            bus_tx_enqueue_byte('R');
            bus_tx_enqueue_byte('e');
            bus_tx_enqueue_byte('s');
            bus_tx_enqueue_byte('e');
            bus_tx_enqueue_byte('t');
            bus_tx_enqueue_byte(' ');
            bus_tx_enqueue_byte('B');
            bus_tx_enqueue_byte('M');
            bus_tx_enqueue_byte('S');
            bus_tx_enqueue_byte(0x0D);
            for (i = 0; i < 2000; i++) {
                watchdog_kick();
                systick_delay(1);
                if (bus_rx_byte_locked(&rx_byte) != 0)
                    g_log_putc(rx_byte);
            }
            break;
        case 6:
            g_log_func("Motor reset\r\n");
            HAL_GPIO_WritePin(GPIOB_BASE, 0x200, 1);   /* PB9 pulse */
            systick_delay(10);
            HAL_GPIO_WritePin(GPIOB_BASE, 0x200, 0);
            break;
        }
        g_reset_sm[0] = 0;
        break;
    }
}

/* Invalidate the cached battery charge-time/range estimate and re-arm the 10 s
 * BMS charge poll (OEM charge_time_estimate_reset, 0x0802E40C). The standalone
 * form of the idiom inlined in status_process: arm the G_STATE[0x1d] poll slot
 * for 10000 ticks (10 s) and force the estimate field at app-ctx+0x3FE to its
 * "invalid / not yet measured" sentinel (0x8300 = -32000). Called from the
 * staged-message / announce-mark dispatcher after a fresh announce lands, so
 * the next status_process pass recomputes the estimate. */
void charge_time_estimate_reset(void)
{
    scheduler_start((uint8_t)G_STATE[0x1d], 10000, (void *)0x0);
    *(uint16_t *)(*(uint8_t **)0x20000944u + 0x3FE) = 0x8300;
}
