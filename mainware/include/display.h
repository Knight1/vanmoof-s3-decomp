#ifndef MAINWARE_DISPLAY_H
#define MAINWARE_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

/* LED-matrix display engine (src/display.c): the dual IS31FL3236 dot-matrix
 * panel driver (I2C2, halves @ device 0x60/0x66), the framebuffer render +
 * bit-banged glyph/number/bar draw API, the I2C-DMA frame transmitter with
 * display-freeze bus recovery, and the display-mode presenter state machine.
 * See docs/display.md. Context object is g_request_ctx @ 0x20008230. */

/* ── panel + LED-driver bring-up (I2C2) ─────────────────────────────────────*/
/* Clear both framebuffer halves, reset the panel, arm the first redraw; returns
 * the panel-reset status (0 = both LED halves configured, non-0 = failure). */
uint32_t display_module_init(void);
/* 2-byte write {0x00,0x01} to the panel sub-controller (I2C device 0x20). */
uint16_t display_send_init_cmd(void);
/* 2-byte write {0x00,0x02} to the panel sub-controller (light-sensor bring-up). */
void     display_write_reg20_init(void);
/* Configure both IS31FL3236 halves (0x60 then 0x66), retrying each 3x; 0/1. */
uint32_t display_panel_reset(void);

/* ── IS31FL373x command helpers (addr = I2C device 0x60/0x66) ────────────────*/
int  led_driver_panel_config(uint8_t addr);            /* full power-up config */
int  led_driver_brightness_write(uint8_t addr, uint8_t val);
int  led_driver_standby_write(uint8_t addr);
void led_driver_set_shipping_mode(uint8_t mode);       /* gated by g_request_ctx+0x159 */
uint32_t led_driver_enter_shipping_mode(void);

/* ── framebuffer render + I2C-DMA transmit ──────────────────────────────────*/
void     led_matrix_render_frame_region(uint16_t frame_ticks);
void     led_matrix_overlay_frame_region(void);
/* I2C-DMA the two framebuffer halves to the driver chips; recovers a hung bus
 * (" ERR dsp freeze"). Returns the DMA HAL status of the active step. */
uint32_t led_matrix_transmit_step(void);
int      matrix_glyph_src_addr(int base, int frame, int row, int x0,
                               uint32_t span, uint16_t col);
uint16_t matrix_glyph_frame_delay(int base, int frame);

/* ── draw API (writes the framebuffer halves, raises the dirty flag) ─────────*/
void matrix_draw_level_bar(int level);
void matrix_draw_level_bar_blink(int level);
void matrix_draw_speed(uint32_t speed, char digits, uint32_t arg3, uint32_t arg4,
                       char blink);
void matrix_draw_icon(int glyph, int x0);
void matrix_draw_number(uint32_t value, int x0);
void matrix_set_corner_led(uint32_t which);   /* 1..4 -> one corner LED on */
void matrix_set_turn_indicator(uint32_t side);/* 1 = left, 3 = right */

/* ── display-mode presenter ─────────────────────────────────────────────────*/
void display_mode_sm_step(uint8_t *ctx);

/* ── small accessors over g_request_ctx ─────────────────────────────────────*/
bool     is_display_bus_ready(void);      /* render-state (+0x130) idle */
bool     ctx_flag_0x131_is_clear(void);   /* overlay-state (+0x131) idle */
uint8_t  display_aux_byte_get(void);      /* +0x13C */
void     display_set_aux_flag(void);      /* +0x13C = 1 */
void     display_request_set(int32_t req);
int32_t  display_request_get(void);
void     display_request_clear(void);
void     display_request_recovery(void);  /* +0x150 = 1 (freeze recovery) */

#endif
