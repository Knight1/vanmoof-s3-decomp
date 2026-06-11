/*
 * main.c — mainware application entry, boot/clock bring-up, and the super-loop.
 *
 * Reconstructed from the OEM (assert filename "src/main.c", confirmed in the
 * rodata literal pool):
 *
 *   main                       0x0803DEA8  — entry + infinite super-loop
 *   boot_init_cold             0x0803DDE0  — power-on clock tree (HSE+PLL, LSI)
 *   boot_init_warm             0x0803DADC  — soft-reset clock tree (HSE+PLL, LSE)
 *   mainware_boot_init_sequence 0x0803FC94 — board/subsystem bring-up + defaults
 *
 * Faithfulness: behaviour-equivalent to the live disassembly — exact control
 * flow, exact ctx field offsets + access widths, exact rodata log strings
 * (resolved from the literal pools), MMIO/GPIO writes verbatim, and the warm/
 * cold clock-config struct values reproduced field-for-field.
 *
 * LINK NOTE — `main` is defined __attribute__((weak)) here. Reset_Handler
 * (startup_stm32f413.S) still binds `bl main` to the *strong* spin-stub in
 * startup, so this real super-loop is compiled + warning-checked but link-
 * discarded, exactly like every other sourced-but-not-yet-rooted module
 * (update.c, ble.c, modem.c, status_process). The whole call-graph closure of
 * main (~70 callees) is not yet sourced; once it is, flip this `main` to strong
 * and drop the startup stub to root the graph (see docs/progress.md).
 */

#include <stdint.h>
#include <string.h>     /* memset, memcpy */
#include <stdio.h>      /* snprintf */

#include "main.h"
#include "app.h"        /* clock_pulse_gpioa8_until_pc9 */
#include "flash.h"      /* struct boot_cfg_block, config_persist_dual_bank */
#include "gpio.h"       /* gpio_init */
#include "i2c.h"        /* i2c3_handle_init */
#include "log.h"        /* g_log_func, log_print_timestamp_prefix, log_buffer_crc_check */
#include "panic.h"      /* Error_Handler, muco_assert_fail */
#include "scheduler.h"  /* scheduler_init */
#include "watchdog.h"   /* watchdog_init, watchdog_kick */

/* ── Cortex-M system control ────────────────────────────────────────────────*/
#define SCB_VTOR        (*(volatile uint32_t *)0xE000ED08u)

/* ── STM32F413 GPIO port bases (AHB1) ───────────────────────────────────────*/
#define GPIOA_BASE      ((void *)0x40020000u)
#define GPIOB_BASE      ((void *)0x40020400u)
#define GPIOC_BASE      ((void *)0x40020800u)
#define GPIOD_BASE      ((void *)0x40020C00u)
#define GPIOE_BASE      ((void *)0x40021000u)

/* ── RCC / PWR (clock bring-up preamble) ────────────────────────────────────*/
#define RCC_APB1ENR     (*(volatile uint32_t *)(0x40023800u + 0x40))   /* +0x40 */
#define RCC_APB1ENR_PWREN   0x10000000u                                /* bit 28 */
#define PWR_CR          (*(volatile uint32_t *)0x40007000u)
#define PWR_CR_VOS      0x0000C000u                                    /* VOS = scale 1 */

/* Image header (app slot) — version word at +4, logged as the boot banner. */
#define IMG_HEADER_BASE 0x08020000u

/* Boot-time persisted config defaults: the schema id stamped into ctx+0x140 and
 * the default region/speed preset table the loop seeds from. */
#define CFG_SCHEMA_ID_MASK   0x000706F4u    /* compare: (ctx[0x140] & 0xFFFFFF) */
#define CFG_SCHEMA_ID_STAMP  0x010706F4u    /* stored when the schema migrates  */
#define DEFAULT_CFG_TABLE    0x0804F54Cu    /* +0x48 → ctx+0x2DE (0x30 bytes)   */

/* g_app_ctx field access (the flat struct at G_APP_CTX_ADDR; see main.h). */
#define U8(off)   (*(volatile uint8_t  *)(ctx + (off)))
#define U16(off)  (*(volatile uint16_t *)(ctx + (off)))
#define U32(off)  (*(volatile uint32_t *)(ctx + (off)))

/* ── CubeF4 HAL clock-config structs (boot only) ────────────────────────────*/
typedef struct {
    uint32_t OscillatorType, HSEState, LSEState, HSIState,
             HSICalibrationValue, LSIState;
    struct { uint32_t PLLState, PLLSource, PLLM, PLLN, PLLP, PLLQ, PLLR; } PLL;
} rcc_osc_init_t;            /* 52 bytes */

typedef struct {
    uint32_t ClockType, SYSCLKSource, AHBCLKDivider, APB1CLKDivider, APB2CLKDivider;
} rcc_clk_init_t;           /* 20 bytes */

extern int rcc_oscillator_config(rcc_osc_init_t *osc);                    /* HAL_RCC_OscConfig      0x0802643C */
extern int rcc_clock_config(rcc_clk_init_t *clk, uint32_t flash_latency); /* HAL_RCC_ClockConfig    0x08027208 */
extern int rcc_periph_clock_config(const uint32_t *periph);               /* HAL_RCCEx_PeriphCLKConfig 0x08026064 */

/* ── HAL / CRT primitives (vendored at link) ────────────────────────────────*/
extern void HAL_GPIO_WritePin(void *GPIOx, uint16_t pin_mask, int state);
extern int  HAL_GPIO_ReadPin(void *GPIOx, uint16_t pin_mask);
extern void enableIRQinterrupts(void);

/* ── peripheral init (main's bring-up sequence) ─────────────────────────────*/
extern int  hal_mcu_init(void);                       /* 0x080232AC */
extern void dma_controller_init(void);                /* 0x0803C218 */
extern void usart1_init(void);                        /* 0x080332F0 */
extern void usart2_init(void);                        /* 0x08033324 */
extern void usart3_init(void);                        /* 0x08033358 */
extern void usart6_init(void);                        /* 0x0803338C */
extern void uart4_init(void);                         /* 0x08033254 */
extern void uart5_init(void);                         /* 0x08033220 */
extern void uart7_init(void);                         /* 0x080332BC */
extern void uart8_init(void);                         /* 0x08033288 */
extern void i2c2_init(void);                          /* 0x0803C624 */
extern void tim1_pwm_init(void);                      /* 0x0803C4F4 */
extern void crc_init(void);                           /* 0x08040268 */
extern void adc1_init(void);                          /* 0x08032AF8 */
extern void tim6_init(void);                          /* 0x0803C2E0 */
extern void tim7_init(void);                          /* 0x0803C32C */
extern void tim10_init(void);                         /* 0x0803C37C */
extern void rtc_init(void);                           /* 0x0803802C */
extern void comm_buffers_register_all(void);          /* 0x08035D0C */
extern void button_press_state_machines_step(void);   /* 0x08040380 */
extern void log_console_subsystem_init(uint32_t magic, void *app_ctx); /* 0x08043114 */
extern void log_wake_reason(void);                    /* 0x0803DA3C */
extern void dma_peripheral_transfer_4word_step(void); /* 0x08032CA4 */
extern int  uart_rx_ringbuf_get_byte(uint8_t *out);   /* 0x080367B8 (console.c) */
extern void app_ctx_ptr_set(void *app_ctx);           /* 0x08033964 → *0x20000944 */
extern void clear_buffer_0x180(void);                 /* 0x0803A4FC */
extern void clear_buffer_0x600(void);                 /* 0x0803FA84 */
extern void smodbus_queue_timer_init(void);           /* 0x08037980 */
extern void bmodbus_queue_timer_init(void);           /* 0x08039F2C */
extern void reset_reason_log_and_clear(void);         /* 0x0803D978 */
extern void speed_capture_init(void *cfg_a, void *cfg_b); /* 0x08038F30 */
extern void flash_program_rdp_level_once(void);       /* 0x0803DA78 */

/* ── per-loop services ──────────────────────────────────────────────────────*/
extern void     light_tick_update(void *ctx);                 /* 0x080371E8 */
extern void     modem_sim_state_machine(void);                /* 0x0803D284 */
extern void     light_pattern_step(void *trigger, int channel,
                                   uint16_t threshold, uint8_t bright_mode); /* 0x08037B64 */
extern void     sms_info_tracking_state_machine(void *ctx);   /* 0x0803CC6C */
extern void     modbus_shifter_link_monitor(void *ctx);       /* 0x0802945C */
extern void     modbus_bat_service_step(void *ctx);           /* 0x0803F338 */
extern void     lipo_charge_state_monitor(void *ctx);         /* 0x08036B98 */
extern int      ble_ssp_dispatch(void);                       /* 0x0803F8FC */
extern void     state_flags_clear(uint32_t mask, uint32_t lo);/* 0x0802A240 */
extern void     state_flags_set(uint32_t lo, uint32_t mask);  /* 0x0802A268 */
extern int      ssp_ble_tx_queue_pump(void);                  /* 0x0803F6B4 */
extern unsigned motor_fw_update_fsm_step(void);               /* 0x08030FF4 */
extern int      sspm_rx_reply_handler(void);                  /* 0x0803A42C */
extern int      sspm_tx_queue_pump(void);                     /* 0x0803A278 */
extern void     led_matrix_render_frame_region(uint16_t frame_ticks); /* 0x0803B7E0 */
extern void     led_matrix_overlay_frame_region(void);        /* 0x0803B988 */
extern uint32_t led_matrix_transmit_step(void);              /* 0x0803BB40 (I2C-DMA frame pump) */
extern void     charger_and_pc1_sense_debounce(void *ctx_state); /* 0x08040788 */
extern void     status_process(void *ctx);                    /* 0x0802AAF8 */
extern uint16_t supply_voltage_sample_step(void);             /* 0x08029B24 */
extern uint32_t output_value_filter_step(void);               /* 0x08038F78 */
extern void     ble_telemetry_change_broadcast(void *ctx);    /* 0x0803A5B0 */
extern void     subsystem_update_sm(void *ctx);               /* 0x08031900 */
extern int      update_sm_is_idle(void);                      /* 0x08032980 */
extern void     log_upload_sm_step(int force_start);          /* 0x08029774 */
extern void     display_mode_sm_step(void *ctx);              /* 0x0802E800 */
extern void     factory_reset_sm_step(uint8_t *ctx);          /* 0x08038A90 (states.c) */
extern void     staged_msg_validate_and_dispatch(void *staged); /* 0x08043C74 */

/* ── mainware_boot_init_sequence callees ────────────────────────────────────*/
extern int  display_module_init(void);                /* 0x0803B700 */
extern void flash_unlock_and_clear_status(void);
extern void module_ctx_init(void *owner);             /* 0x0803D624 */
extern void obj_set_field34(uint32_t v);              /* 0x0803C5F0 */
extern void obj_set_field38(uint32_t v);              /* 0x0803C5FC */
extern void led_channel3_set_brightness(uint32_t v);  /* 0x0803C608 */
extern int  tim_channel_enable_output(void *htim, uint32_t channel); /* 0x08027988 */
extern void peripheral_irq10_init_and_start(void);    /* 0x08043CB4 */
extern void peripheral_disable_handle(void);          /* 0x08037A98 */
extern int  hdc1080_write_config_reg(void *hi2c, int mode_a, int mode_b); /* 0x08033118 */
extern int  eeprom_read_id_block(uint8_t *out6);      /* 0x0803E138 */
extern int  eeprom_read_config_with_crc_fallback(void *out); /* 0x0803E1A8 */
extern void settings_factory_reset(void *ctx, int mode);
extern int  flash_read_config_with_crc_restore(void *out);   /* 0x08031784 */
extern void sound_groups_init_default(void *cfg);     /* 0x0803FAC0 (seeds sound-group masks ctx+0xF4/F8/FC) */
extern void region_speed_preset_table_load(void *out, int region_index); /* 0x0803FC24 */
extern int  display_write_reg20_init(void);           /* 0x0803B248 (light-sensor bring-up) */
extern int  stc3115_wake(void);                       /* 0x080398CE */
extern void stc3115_fuel_gauge_init(void);            /* 0x08037130 */
extern void lis3dh_config_motion_int(int a, int b);   /* 0x0803D120 */
extern int  lis3dh_accel_init(void);                  /* 0x0803D0BC */
extern int  audio_amp_init(void);                     /* 0x08039174 (MAX9768) */
extern void amp_volume_brownout_apply(const uint8_t *p);
extern int  hw_version_lookup(char *out);

/* Stack-passed EEPROM record writer: 4 register args + a trailing by-value block
 * on the stack (matching the OEM's memcpy-then-call). config_persist_dual_bank +
 * struct boot_cfg_block come from flash.h. */
struct boot_state_tail { uint8_t bytes[0x2C]; };   /* ctx[0x320..0x34B] = 11 words */
extern unsigned save_state_record_to_eeprom(uint32_t a, uint32_t b, uint32_t c,
                                            uint32_t d, struct boot_state_tail tail); /* 0x0803E2CC */

static void boot_init_cold(void);
static void boot_init_warm(void);
static void mainware_boot_init_sequence(uint8_t *ctx);

/* ─────────────────────────────────────────────────────────────────────────
 * Clock-tree bring-up. Both paths enable the PWR clock, select VOS scale 1,
 * then run OscConfig → ClockConfig (FLASH_LATENCY_3) → PeriphCLKConfig. The
 * cold path uses the LSI (RTC not yet running); the warm path uses the LSE that
 * already survived the soft reset. PLL: HSE/6 ×96 /2 (SYSCLK), HSE-sourced.
 * ───────────────────────────────────────────────────────────────────────── */
static void boot_init_cold(void)
{
    rcc_osc_init_t osc;
    rcc_clk_init_t clk;
    uint32_t periph[22];

    memset(&osc, 0, sizeof osc);
    memset(&clk, 0, sizeof clk);
    memset(periph, 0, sizeof periph);

    RCC_APB1ENR |= RCC_APB1ENR_PWREN;
    PWR_CR      |= PWR_CR_VOS;

    osc.OscillatorType = 9;            /* HSE | LSI */
    osc.HSEState       = 0x10000;      /* RCC_HSE_ON */
    osc.LSIState       = 1;            /* RCC_LSI_ON */
    osc.PLL.PLLState   = 2;            /* RCC_PLL_ON */
    osc.PLL.PLLSource  = 0x400000;     /* RCC_PLLSOURCE_HSE */
    osc.PLL.PLLM       = 6;
    osc.PLL.PLLN       = 0x60;         /* 96 */
    osc.PLL.PLLP       = 2;
    osc.PLL.PLLQ       = 2;
    osc.PLL.PLLR       = 2;
    if (rcc_oscillator_config(&osc) != 0) {
        Error_Handler();
    }

    clk.ClockType      = 0xf;          /* SYSCLK | HCLK | PCLK1 | PCLK2 */
    clk.SYSCLKSource   = 2;            /* RCC_SYSCLKSOURCE_PLLCLK */
    clk.AHBCLKDivider  = 0;
    clk.APB1CLKDivider = 0x1c00;
    clk.APB2CLKDivider = 0;
    if (rcc_clock_config(&clk, 3) != 0) {   /* FLASH_LATENCY_3 */
        Error_Handler();
    }

    periph[0] = 8;
    periph[9] = 0x200;
    if (rcc_periph_clock_config(periph) != 0) {
        Error_Handler();
    }
}

static void boot_init_warm(void)
{
    rcc_osc_init_t osc;
    rcc_clk_init_t clk;
    uint32_t periph[22];

    memset(&osc, 0, sizeof osc);
    memset(&clk, 0, sizeof clk);
    memset(periph, 0, sizeof periph);

    RCC_APB1ENR |= RCC_APB1ENR_PWREN;
    PWR_CR      |= PWR_CR_VOS;

    osc.OscillatorType = 5;            /* HSE | LSE */
    osc.HSEState       = 0x10000;      /* RCC_HSE_ON */
    osc.LSEState       = 1;            /* RCC_LSE_ON */
    osc.PLL.PLLState   = 2;            /* RCC_PLL_ON */
    osc.PLL.PLLSource  = 0x400000;     /* RCC_PLLSOURCE_HSE */
    osc.PLL.PLLM       = 6;
    osc.PLL.PLLN       = 0x60;
    osc.PLL.PLLP       = 2;
    osc.PLL.PLLQ       = 2;
    osc.PLL.PLLR       = 2;
    if (rcc_oscillator_config(&osc) != 0) {
        muco_assert_fail("src/main.c", 0x55c);
    }

    clk.ClockType      = 0xf;
    clk.SYSCLKSource   = 2;
    clk.AHBCLKDivider  = 0;
    clk.APB1CLKDivider = 0x1c00;
    clk.APB2CLKDivider = 0;
    if (rcc_clock_config(&clk, 3) != 0) {
        muco_assert_fail("src/main.c", 0x568);
    }

    periph[0] = 8;
    periph[9] = 0x100;
    if (rcc_periph_clock_config(periph) != 0) {
        muco_assert_fail("src/main.c", 0x56e);
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Board + subsystem bring-up (runs once, after the peripheral inits). Prints
 * the firmware banner, drives the power/LED GPIO rails, unlocks flash, brings
 * up the LED-matrix display, validates the persisted state record (rebuilding
 * defaults + re-saving on a bad/missing record), migrates the config schema,
 * resets the per-loop context block, and self-tests the I2C devices (HDC1080
 * temp/humidity, STC3115 gas-gauge, LIS3DH accelerometer, MAX9768 amp) with a
 * fault-counted retry loop that bus-recovers I2C and finally raises a fault
 * flag if too many devices fail. Closes by selecting the SIM source (PCB vs
 * holder) and formatting the model string into ctx+0x64A.
 * ───────────────────────────────────────────────────────────────────────── */
static void mainware_boot_init_sequence(uint8_t *ctx)
{
    uint32_t ver = *(volatile uint32_t *)(IMG_HEADER_BASE + 4);
    g_log_func("\r\nES3 v%d.%02d.%02d\r\n",
               ver >> 24, (ver & 0xffffff) >> 16, (ver & 0xffff) >> 8);

    HAL_GPIO_WritePin(GPIOD_BASE, 0x8000, 1);   /* PD15 */
    HAL_GPIO_WritePin(GPIOA_BASE, 0x1000, 1);   /* PA12 */
    HAL_GPIO_WritePin(GPIOA_BASE, 0x8000, 0);   /* PA15 low/high pulse */
    HAL_GPIO_WritePin(GPIOA_BASE, 0x8000, 1);
    HAL_GPIO_WritePin(GPIOB_BASE, 0x0008, 0);   /* PB3  */
    HAL_GPIO_WritePin(GPIOD_BASE, 0x0400, 0);   /* PD10 */
    HAL_GPIO_WritePin(GPIOD_BASE, 0x0800, 0);   /* PD11 */
    HAL_GPIO_WritePin(GPIOD_BASE, 0x1000, 0);   /* PD12 */
    HAL_GPIO_WritePin(GPIOB_BASE, 0x0400, 1);   /* PB10 */
    HAL_GPIO_WritePin(GPIOB_BASE, 0x0200, 0);   /* PB9  */
    HAL_GPIO_WritePin(GPIOE_BASE, 0x0020, 0);   /* PE5  */
    HAL_GPIO_WritePin(GPIOB_BASE, 0x8000, 1);   /* PB15 */
    HAL_GPIO_WritePin(GPIOD_BASE, 0x2000, 1);   /* PD13 */

    if (display_module_init() != 0) {
        g_log_func("  ERR Led Display\r\n");
    }
    flash_unlock_and_clear_status();
    module_ctx_init(ctx);

    obj_set_field34(0);
    obj_set_field38(0);
    led_channel3_set_brightness(0);
    tim_channel_enable_output((void *)0x20009A84u, 0);   /* TIM1 CH1 */
    tim_channel_enable_output((void *)0x20009A84u, 4);   /* TIM1 CH2 */
    tim_channel_enable_output((void *)0x20009A84u, 8);   /* TIM1 CH3 */
    peripheral_irq10_init_and_start();
    peripheral_disable_handle();

    int fail_count = 0;
    do {
        uint8_t id_blk[7];
        fail_count = (hdc1080_write_config_reg((void *)0x20009B04u, 0, 2) != 0);
        if (fail_count) {
            g_log_func(" ERROR HDC1080\r\n");
        }

        if (eeprom_read_id_block(id_blk) == 0) {
            if (eeprom_read_config_with_crc_fallback(ctx + 0x310) != 0) {
                g_log_func("   ERROR eerom read\r\n");
                memset(ctx + 0x318, 0, 0x34);
                U8(0x310) = 0x0b;
                U8(0x317) = 1;
                U8(0x312) = 0;
                U8(0x314) = 0;
                U8(0x313) = 1;
                U8(0x318) = 10;
                U32(0x31c) = 0;
                U32(0x324) = 0;
                U32(0x320) = 0;
                U32(0x328) = 0;
                U8(0x311) = 0;
                U8(0x316) = 4;
                U8(0x315) = 0xff;
                U16(0x332) = 0xffff;
                U8(0x341) = 0;
                U8(0x340) = (HAL_GPIO_ReadPin(GPIOC_BASE, 0x100) != 0);   /* PC8 */
                U16(0x342) = 0;
                watchdog_kick();
                if (save_state_record_to_eeprom(U32(0x310), U32(0x314), U32(0x318),
                                                U32(0x31c),
                                                *(const struct boot_state_tail *)(ctx + 0x320)) == 0) {
                    g_log_func("Save default values\r\n");
                } else {
                    g_log_func(" ERROR Save default values\r\n");
                }
            }
        } else {
            fail_count++;
            g_log_func("   ERROR I2C eerom\r\n");
        }

        U32(0x344) = 0;
        if (flash_read_config_with_crc_restore(ctx + 0xf4) != 0) {
            g_log_func("  ERR read flash, load defaults\r\n");
            settings_factory_reset(ctx, 1);
        }
        if ((U32(0x140) & 0xffffff) != CFG_SCHEMA_ID_MASK) {
            U32(0x140) = CFG_SCHEMA_ID_STAMP;
            g_log_func("Set new defaults\r\n");
            U16(0x102) = 200;
            U8(0x108) = 0;
            U8(0x107) = 0x26;
            sound_groups_init_default(ctx + 0xf4);
            uint8_t res = config_persist_dual_bank(U32(0xf4), U32(0xf8), U32(0xfc),
                                                   U32(0x100),
                                                   *(const struct boot_cfg_block *)(ctx + 0x104));
            g_log_func("res: %s\r\n", res ? "ERROR" : "OK");
        }

        memset(ctx + 0x148, 0, 0x78);
        HAL_GPIO_WritePin(GPIOE_BASE, 0x0008, U8(0x10b) == 1 ? 1 : 0);   /* PE3 status */
        if (U16(0x102) == 0) {
            g_log_func("Restore dark lux\r\n");
            U16(0x102) = 200;
        }

        U8(0x378) &= 0xfe;
        U32(0x3b8) = 0;
        U32(0x3bc) = 0;
        U16(0x3b2) = 200;
        U8(0x3c8) = 0;
        uint8_t b1 = U8(0x316) & 0x7f;
        U8(0x3c9) = b1;
        U8(0x3ca) = b1;
        U8(0x3cb) = U8(0x316) >> 7;
        U8(0x3cc) = 0;
        U16(0x3b0) = 0;
        U8(0x34c) = 0;
        U8(0x3e1) = 0;
        U8(0x34d) = 0;
        U8(0x34e) = 0;
        U8(0x34f) = 0;
        U8(0x3d0) = 0;
        U16(0x3d2) = 0xffff;
        U8(0x3d4) = 0xff;
        U16(0x3f8) = 0xff;
        U16(0x3fc) = 0xffff;
        U8(0xf0) = 0;
        U8(0xf1) = 0;
        U32(0x78) = 1;
        U32(0x7c) = 10;
        U32(0x80) = 1;
        U32(0x84) = 0;
        memset(ctx + 0x88, 0, 0x28);
        U32(0xb4) = 0x0b;
        U32(0xb8) = 9;
        U32(0xbc) = 1;
        U32(0xc0) = 0;
        memset(ctx + 0xc4, 0, 0x24);
        U16(0x332) = 0xffff;
        U16(0x3c2) = 0;
        U16(0x3c0) = 0;
        U16(0x3c6) = 0;
        U16(0x3c4) = 0;
        U32(0x358) = 0;
        U32(0x35c) = 0;
        U32(0x360) = 0;
        region_speed_preset_table_load(ctx + 0x1c4, U8(0x109));
        U32(0x398) = 0;
        U32(0x39c) = 0;
        U32(0x3a0) = 0;
        memcpy(ctx + 0x2de, (const void *)(DEFAULT_CFG_TABLE + 0x48), 0x30);
        watchdog_kick();

        if (display_write_reg20_init() != 0) {
            g_log_func("ERR Light sensor\r\n");
        }
        if (stc3115_wake() == 0) {
            stc3115_fuel_gauge_init();
        } else {
            fail_count++;
            g_log_func("  ERR ST3115 wake\r\n");
        }
        if (lis3dh_accel_init() == 0) {
            lis3dh_config_motion_int(0, 6);
        } else {
            fail_count++;
            g_log_func("  ERR LIS3DH\r\n");
        }
        watchdog_kick();
        HAL_GPIO_WritePin(GPIOD_BASE, 0x0020, 0);   /* PD5  */
        HAL_GPIO_WritePin(GPIOE_BASE, 0x0004, 1);   /* PE2  amp enable */
        if (audio_amp_init() == 0) {
            amp_volume_brownout_apply((const uint8_t *)0);
        } else {
            fail_count++;
            g_log_func("  ERR init MAX9768\r\n");
        }
        HAL_GPIO_WritePin(GPIOD_BASE, 0x0020, 1);   /* PD5  */

        if (fail_count < 3) {
            *(volatile uint8_t *)0x20000101u = 0;   /* clear the retry budget → exit */
        } else {
            g_log_func("i2c bus error\r\n");
            g_log_func("Clocking %d\r\n", clock_pulse_gpioa8_until_pc9());
            *(volatile uint8_t *)0x20000101u -= 1;
        }
    } while (*(volatile uint8_t *)0x20000101u != 0);

    if (fail_count > 2) {
        state_flags_set(0, 0x800000);
    }

    U32(0x3a4) = 0;
    U32(0x3a8) = 0;
    log_print_timestamp_prefix();
    if (HAL_GPIO_ReadPin(GPIOE_BASE, 0x0400) == 0) {        /* PE10 SIM-detect */
        g_log_func("SIM: PCB\r\n");
        HAL_GPIO_WritePin(GPIOE_BASE, 0x1000, 1);          /* PE12 */
    } else {
        g_log_func("SIM: Holder\r\n");
        HAL_GPIO_WritePin(GPIOE_BASE, 0x1000, 0);
    }

    char hwver;
    uint32_t ver_char = (*(volatile uint32_t *)BOOT_MAGIC_ADDR == BOOT_MAGIC_WARM)
                            ? 0x32 : 0x30;
    if (hw_version_lookup(&hwver) == 0) {
        ver_char = 0x3f;          /* '?' */
    } else if (hwver == 7) {
        ver_char = 0x31;          /* '1' */
    }
    uint32_t region_char = (U8(0x10b) == 1) ? 0x45 /* 'E' */ : 0x58 /* 'X' */;
    snprintf((char *)(ctx + 0x64a), 7, "%cS3.%c", (int)region_char, (int)ver_char);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Application entry + super-loop (OEM `main`, 0x0803DEA8). See the LINK NOTE
 * at the top of this file for why this is declared weak.
 * ───────────────────────────────────────────────────────────────────────── */
__attribute__((weak)) int main(void)
{
    uint8_t *ctx = (uint8_t *)G_APP_CTX_ADDR;

    SCB_VTOR = APP_VECTOR_TABLE;
    hal_mcu_init();
    enableIRQinterrupts();
    if (*(volatile uint32_t *)BOOT_MAGIC_ADDR == BOOT_MAGIC_WARM) {
        boot_init_warm();
    } else {
        boot_init_cold();
    }

    gpio_init();
    dma_controller_init();
    usart1_init();
    usart2_init();
    usart3_init();
    usart6_init();
    uart5_init();
    uart4_init();
    uart8_init();
    i2c2_init();
    i2c3_handle_init();
    tim1_pwm_init();
    uart7_init();
    crc_init();
    adc1_init();
    tim6_init();
    rtc_init();
    watchdog_init();
    tim7_init();
    tim10_init();
    comm_buffers_register_all();
    scheduler_init();
    watchdog_kick();
    button_press_state_machines_step();
    log_console_subsystem_init(0x55AA5501u, ctx);
    log_buffer_crc_check();
    log_wake_reason();
    dma_peripheral_transfer_4word_step();
    mainware_boot_init_sequence(ctx);
    uart_rx_ringbuf_get_byte(0);
    app_ctx_ptr_set(ctx);
    clear_buffer_0x180();
    clear_buffer_0x600();
    HAL_GPIO_WritePin(GPIOD_BASE, 0x80, 1);             /* PD7 */
    smodbus_queue_timer_init();
    bmodbus_queue_timer_init();
    reset_reason_log_and_clear();
    speed_capture_init(ctx + 0x10b, ctx + 0x31c);
    flash_program_rdp_level_once();

    for (;;) {
        watchdog_kick();

        light_tick_update(ctx);
        if (U8(0x34d) == 0) {
            modem_sim_state_machine();
        }
        if (U16(0x402) == 1) {
            light_pattern_step(ctx + 0x350, 0, U16(0x102), U8(0x10c));
            light_pattern_step(ctx + 0x351, 1, U16(0x102), U8(0x10c));
            light_pattern_step(ctx + 0x352, 2, U16(0x102), U8(0x10c));
        }
        sms_info_tracking_state_machine(ctx);
        modbus_shifter_link_monitor(ctx);
        modbus_bat_service_step(ctx);
        lipo_charge_state_monitor(ctx);

        if (ble_ssp_dispatch() != 0) {
            state_flags_clear(0x800000, 0);
        }
        if (ssp_ble_tx_queue_pump() != 0) {
            log_print_timestamp_prefix();
            g_log_func(" ERROR SSP BLE msg removed\r\n");
        }
        if (motor_fw_update_fsm_step() == 3) {
            if (sspm_rx_reply_handler() != 0) {
                state_flags_clear(0x400000, 0);
            }
            if (sspm_tx_queue_pump() == 1) {
                clear_buffer_0x180();
                log_print_timestamp_prefix();
                g_log_func(" ERROR SSP MOTOR msg not confirmed\r\n");
            }
        }

        led_matrix_render_frame_region(U16(0x354));
        led_matrix_overlay_frame_region();
        if (led_matrix_transmit_step() != 0) {
            g_log_func("ERR Display\r\n");
        }

        button_press_state_machines_step();
        charger_and_pc1_sense_debounce(ctx + 0x310);
        status_process(ctx);

        U16(0x3b0) = supply_voltage_sample_step();
        if (U16(0x3c6) == 0) {
            U32(0x3c0) = output_value_filter_step();
        } else {
            U32(0x3c0) = U32(0x3c4);
        }

        ble_telemetry_change_broadcast(ctx);
        subsystem_update_sm(ctx);

        int upload_arg;
        if (U8(0x314) != 0) {
            upload_arg = 0;
        } else if (U8(0x313) == 0) {
            upload_arg = 0;
        } else if ((U32(0x3b8) & 0x800000) == 0) {
            upload_arg = update_sm_is_idle() ? 1 : 0;
        } else {
            upload_arg = 0;
        }
        log_upload_sm_step(upload_arg);

        display_mode_sm_step(ctx);
        factory_reset_sm_step(ctx);
        staged_msg_validate_and_dispatch(ctx + 0x3d4);
    }
}
