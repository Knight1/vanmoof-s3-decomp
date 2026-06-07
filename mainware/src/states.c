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
extern int display_send_init_cmd();
extern int enter_stop_mode();
extern int gpio_pc1_is_low();
extern int internal_lipo_charge_step();
extern int is_display_bus_ready();
extern int is_user_reset_pending();
extern int led_channel3_set_brightness();
extern int led_driver_enter_shipping_mode();
extern int led_driver_set_shipping_mode();
extern int light_sensor_read_step();
extern int lis3dh_config_motion_int();
extern int lis3dh_int1_clear();
extern int lis3dh_powerdown();
extern int lock_blink_sequence_step();
extern int locked_state_step();
extern int maybe_enqueue_tx_message();
extern int maybe_ota_progress_all_low();
extern int modbus_shift_submit();
extern int modem_info_ready();
extern int obj_set_field34();
extern int obj_set_field38();
extern int power_assist_gear_step();
extern int power_state_get_clamped();
extern int reboot_restart_task();
extern int save_state_record_to_eeprom();
extern int sched_timer_arm_or_alloc();
extern int set_unlock_state_persist();
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
extern int state_table_ptr_get();
extern int system_reset();
extern int telemetry_datalog_emit();
extern int testmode_command_dispatch();
extern int testmode_continue_state_10();
extern int testmode_enter_state_16();
extern int display_request_clear();
extern int matrix_draw_icon();
extern int clear_flag_00e5();
extern int app_ctx_clear_field_328();
extern int battery_telemetry_state_get();
extern int announce_records_reset();
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
        led_driver_set_shipping_mode();
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
