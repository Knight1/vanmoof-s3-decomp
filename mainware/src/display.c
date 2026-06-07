/*
 * display.c -- VanMoof S3 mainware LED-matrix display engine.
 *
 * The bike's signature dot-matrix display: two Lumissil IS31FL3236 LED-driver
 * chips on I2C2 (left half @ device 0x60, right half @ 0x66) fed from a dual
 * framebuffer, plus the bit-banged glyph/number/bar draw API, the I2C-DMA frame
 * transmitter with display-freeze bus recovery, and the ~40-state display-mode
 * presenter that picks what to show (speed, battery %, icons, animations).
 *
 * Behaviour-equivalent reconstruction of the live disassembly (exact control
 * flow, ctx/framebuffer offsets, IS31FL373x command sequences, glyph tables,
 * and log strings); transcribed + adversarially verified against the binary.
 *
 * Memory model (resolved from the OEM literal pool):
 *   g_request_ctx 0x20008230  display ctx: framebuffer half A @ +0x01 (0x96 B),
 *                             half B @ +0x99 (0x96 B); +0x130 render-state,
 *                             +0x131 overlay-state, +0x132 dirty/redraw, plus the
 *                             corner/turn/region/draw sub-fields.
 *   I2C2 handle   0x20009BB8  shared by both LED drivers + the panel.
 *   glyph ROM     0x0804F358  brightness LUT (8 B) + icon glyphs (+0x08) + digit
 *                             glyphs (+0x8C); 5x7 cells, 3 bits/pixel.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "display.h"
#include "app.h"        /* set_mode_state_byte, maybe_set_pending_request, reset_dual_buffers_and_flags */
#include "scheduler.h"  /* scheduler_alloc/release/start/slot_is_idle/set_timer_name */
#include "systick.h"    /* systick_now, systick_delay */
#include "log.h"        /* log_print_timestamp_prefix */

/* display/request context (a.k.a. g_request_ctx, OEM 0x20008230). */
#define G_DISP  ((uint8_t *)0x20008230u)

/* CubeF4 GPIO_InitTypeDef layout (used only by the I2C-bus recovery path). */
typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } disp_gpio_init_t;

/* glyph/brightness ROM mirrored from flash rodata 0x0804F358 (the LUT is the
 * first 8 bytes of the same blob the icon/digit renderers index). */
static const uint8_t s_matrix_lut[8] = { 0x00, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0xff };
static const uint8_t s_glyph_blob[210] = {
        /* +0x00 brightness LUT */ 0,4,8,16,32,64,128,255,
        /* +0x08 icon glyphs */
        0x0e,0x11,0x11,0x11,0x11,0x11,0x0e, 0x04,0x0c,0x04,0x04,0x04,0x04,0x0e,
        0x0e,0x11,0x01,0x02,0x04,0x08,0x1f, 0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e,
        0x11,0x11,0x11,0x0f,0x01,0x01,0x01, 0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e,
        0x0e,0x10,0x10,0x1e,0x11,0x11,0x0e, 0x1f,0x01,0x02,0x04,0x08,0x10,0x10,
        0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e, 0x0e,0x11,0x11,0x0f,0x01,0x01,0x01,
        0x19,0x19,0x02,0x04,0x08,0x13,0x13, 0x00,0x00,0x00,0x02,0x05,0x05,0x05,
        0x02,0x01,0x01,0x01,0x01,0x01,0x02, 0x05,0x01,0x02,0x07,0x06,0x01,0x06,
        0x01,0x06,0x05,0x05,0x03,0x01,0x01, 0x07,0x04,0x06,0x01,0x06,0x03,0x04,
        0x06,0x05,0x02,0x07,0x01,0x02,0x04, 0x04,0x02,0x05,0x02,0x05,0x02,0x02,
        0x05,0x03,0x01,0x02,0x00,0x00,
        /* +0x8c digit glyphs */
        0x06,0x09,0x09,0x09,0x09,0x09,0x06, 0x04,0x0c,0x04,0x04,0x04,0x04,0x0e,
        0x06,0x09,0x01,0x02,0x04,0x08,0x1f, 0x0e,0x01,0x01,0x06,0x01,0x01,0x0e,
        0x09,0x09,0x09,0x07,0x01,0x01,0x01, 0x0f,0x08,0x08,0x0e,0x01,0x01,0x0e,
        0x07,0x08,0x08,0x0e,0x09,0x09,0x06, 0x0f,0x01,0x02,0x04,0x08,0x08,0x08,
        0x06,0x09,0x09,0x06,0x09,0x09,0x06, 0x06,0x09,0x09,0x07,0x01,0x01,0x01
    };

/* cross-module leaf helpers (K&R: they gc-section out and the seven source
 * groups call them with varied arg styles -- K&R sidesteps signature clashes). */
extern int  HAL_GPIO_WritePin();
extern int  HAL_GPIO_ReadPin();
extern int  HAL_GPIO_Init();
extern int  FUN_08024760();   /* I2C2 blocking mem-write (CubeF4 HAL) */
extern int  FUN_080248d8();   /* I2C2 blocking mem-read  (CubeF4 HAL) */
extern int  FUN_08024bc0();   /* I2C2 DMA mem-write      (CubeF4 HAL) */
extern int  FUN_0803c8a8();   /* I2C2 bus-busy check (0 = ok, 2 = busy) */
extern int  FUN_0803c8d4();   /* I2C2 deinit */
extern int  FUN_0803a588();   /* clamp + linear range-scale */
extern int  FUN_0804031c();   /* shared announce/event-slot clear */
extern int  i2c2_init();

/* file-static helpers, defined below in OEM address order. */
static int      display_mode_set_if_changed(uint8_t mode);
static void     display_poll_turn_gpio(void);
static uint32_t lowest_set_bit_index(uint32_t lo, uint32_t hi);

/* OEM display_mode_set_if_changed 0x0802E7C8: write the display-mode byte at
 * 0x20000288 only when it differs; returns 1 if it changed. */
static int display_mode_set_if_changed(uint8_t mode)
{
    if (*(volatile uint8_t *)0x20000288u != mode) {
        *(volatile uint8_t *)0x20000288u = mode;
        return 1;
    }
    return 0;
}

/* OEM display_poll_turn_gpio 0x0802E7E0: sample PC2 (GPIOC, pin mask 0x4) and feed
 * it straight into the turn-indicator field (the read result is the call arg). */
static void display_poll_turn_gpio(void)
{
    matrix_set_turn_indicator((uint32_t)HAL_GPIO_ReadPin((void *)0x40020800u, 4));
}

/* Magic divide-by-10 constants from the literal pool.
   DAT_0802f038 = 0x66666667 (signed div-by-10 multiplier, used with >>0x22)
   DAT_0802f06c = 0xCCCCCCCD (unsigned div-by-10 multiplier, bit-35 test)         */
#define DISP_DIV10_SMUL   0x66666667UL
#define DISP_DIV10_UMUL   0xCCCCCCCDULL

/* Two shared SRAM control blocks reached via several literal-pool aliases.
   0x20000068 = display mode-SM state block (g_mode_sm):
       +0 = current mode byte, +1 = applied/prev mode, +2 = blink scheduler slot,
       +4 = u16 last-shown value, +6/+7 = sub scheduler slots.
     (Ghidra aliases: DAT_0802ea44, DAT_0802ed08, DAT_0802f04c)
   0x20000288 = display request/flag block (g_disp_flags):
       +1..+8 = one-shot edge flags / saved-state bytes; +3 = blink phase;
       +8 = saved mode.
     (Ghidra aliases: DAT_0802ea48, DAT_0802ed04, DAT_0802f044)                   */
void display_mode_sm_step(uint8_t *ctx)
{
    uint8_t *g_mode_sm   = (uint8_t *)0x20000068;
    uint8_t *g_disp_flags = (uint8_t *)0x20000288;
    uint16_t uVar1;
    uint8_t  bVar3;
    int      iVar4;
    int      iVar5;
    uint32_t uVar6;
    uint32_t uVar7;

    /* --- mode-change edge handling --- */
    if (g_mode_sm[1] != g_mode_sm[0]) {
        if (4 < g_mode_sm[0]) {
            reset_dual_buffers_and_flags();
        }
        bVar3 = g_mode_sm[0];
        if (((uint8_t)(bVar3 - 6) < 2) || (bVar3 == 0xf)) {
            if (ctx[0xf0] != 0) {
                g_disp_flags[1] = 0;
            }
            if (ctx[0xf1] != 0) {
                g_disp_flags[2] = 0;
            }
        }
        g_mode_sm[1] = bVar3;
    }

    /* --- blink scheduler slot (1 s period); 0xFA == scheduler 'no slot' sentinel --- */
    if (g_mode_sm[2] == 0xfa) {
        bVar3 = scheduler_alloc();
        g_mode_sm[2] = bVar3;
        scheduler_start(bVar3, 1000, (void *)0x0);
    }
    iVar4 = scheduler_slot_is_idle(g_mode_sm[2]);
    if (iVar4 != 0) {
        scheduler_start(g_mode_sm[2], 1000, (void *)0x0);
        g_disp_flags[3] = g_disp_flags[3] ^ 1;
    }

    display_poll_turn_gpio();

    switch (g_mode_sm[0]) {
    case 0:
        iVar4 = is_display_bus_ready();
        if (iVar4 != 0) {
            FUN_0804031c(7);
            set_mode_state_byte(6);
        }
        break;
    case 1:
        if (*(char *)(g_disp_flags + 3) == '\0') {
            set_mode_state_byte(g_disp_flags[8]);
        }
        break;
    case 3:
        iVar5 = scheduler_slot_is_idle(g_mode_sm[7]);
        if (iVar5 != 0) {
            scheduler_release(g_mode_sm + 7);
            *(uint16_t *)(g_mode_sm + 4) = 0;
            set_mode_state_byte(g_disp_flags[4]);
        }
        break;
    case 4:
        if ((ctx[0x3c9] != 0) && (iVar4 = is_display_bus_ready(), iVar4 != 0)) {
            maybe_set_pending_request((int32_t)0x0804c930); /* request descriptor */
        }
        iVar5 = scheduler_slot_is_idle(g_mode_sm[7]);
        if (iVar5 != 0) {
            scheduler_release(g_mode_sm + 7);
            *(uint16_t *)(g_mode_sm + 4) = 0;
            set_mode_state_byte(g_disp_flags[4]);
        }
        break;
    case 5:
        maybe_set_pending_request((int32_t)0x0804da64); /* request descriptor */
        set_mode_state_byte(0);
        break;
    case 6:
        if (*(uint16_t *)(ctx + 0x3c2) < 10) {
            if (ctx[0xf0] == 0) {
                if (*(char *)(g_disp_flags + 1) == '\0') {
                    if ((*(int *)(ctx + 0x3bc) == 0 && *(int *)(ctx + 0x3b8) == 0) ||
                        (*(char *)(g_disp_flags + 3) == '\0')) {
                        iVar4 = is_display_bus_ready();
                        if (iVar4 != 0) {
                            maybe_set_pending_request((int32_t)0x0804cb1c); /* request descriptor */
                            matrix_set_corner_led(ctx[0x3cc]);
                        }
                    } else {
                        g_disp_flags[4] = 6;
                        set_mode_state_byte(0xc);
                    }
                } else {
                    g_mode_sm[4] = 0;
                    g_mode_sm[5] = 0;
                    display_request_clear();
                    maybe_set_pending_request((int)(ctx + 0x78));
                    g_disp_flags[1] = 0;
                }
            } else if (*(char *)(g_disp_flags + 1) == '\0') {
                g_disp_flags[1] = 1;
                display_request_clear();
                maybe_set_pending_request((int)ctx);
            }
        } else {
            set_mode_state_byte(7);
        }
        if (*(char *)(g_disp_flags + 5) != '\0') {
            g_disp_flags[5] = 0;
            display_request_set((int32_t)0x0804a4ec); /* request descriptor */
        }
        if (*(char *)(g_disp_flags + 6) != '\0') {
            g_disp_flags[6] = 0;
            display_request_set((int32_t)0x0804a2b8); /* request descriptor */
        }
        if (*(int16_t *)(ctx + 0x3fc) != -1) {
            matrix_draw_speed((char)*(int16_t *)(ctx + 0x3fc), *(uint32_t *)(ctx + 0x3d4),
                              *(uint32_t *)(ctx + 0x3d8), *(uint32_t *)(ctx + 0x3dc),
                              *(uint16_t *)(ctx + 0x3e0));
        }
        break;
    case 7:
        if (*(uint16_t *)(ctx + 0x3c2) < 9) {
            set_mode_state_byte(6);
        } else if (ctx[0xf0] == 0) {
            if (*(char *)(g_disp_flags + 1) != '\0') {
                *(uint16_t *)(g_mode_sm + 4) = 0;
                display_request_clear();
                maybe_set_pending_request((int)(ctx + 0x78));
                g_disp_flags[1] = 0;
            }
            matrix_set_corner_led(ctx[0x3cc]);
            if ((*(int16_t *)(ctx + 0x3c2) != *(int16_t *)(g_mode_sm + 4)) &&
                (iVar4 = is_display_bus_ready(), iVar4 != 0)) {
                /* g_mode_sm[6] holds a scheduler slot; 0xFA (signed -6) == 'no slot' sentinel.
                 * ARM char is unsigned, so compare the byte against 0xFA directly. */
                if (g_mode_sm[6] == 0xFA) {
                    bVar3 = scheduler_alloc();
                    g_mode_sm[6] = bVar3;
                    scheduler_set_timer_name(bVar3, 10, (const char *)0x0805080c); /* "slow_show_speed_tmr" */
                    scheduler_start(g_mode_sm[6], 10, (void *)0x0);
                }
                iVar5 = scheduler_slot_is_idle(g_mode_sm[6]);
                if (iVar5 != 0) {
                    scheduler_start(g_mode_sm[6], 1000, (void *)0x0);
                    uVar1 = *(uint16_t *)(ctx + 0x3c2);
                    *(uint16_t *)(g_mode_sm + 4) = uVar1;
                    if (ctx[0x10a] == 0) {
                        matrix_draw_number((uVar1 / 10) & 0xff, 3);
                    } else {
                        matrix_draw_number((uVar1 & 0xfff) >> 4, 3);
                    }
                }
            }
        } else if (*(char *)(g_disp_flags + 1) == '\0') {
            g_disp_flags[1] = 1;
            display_request_clear();
            maybe_set_pending_request((int)ctx);
        }
        if (*(char *)(g_disp_flags + 5) != '\0') {
            g_disp_flags[5] = 0;
            display_request_set((int32_t)0x0804a4ec); /* request descriptor */
        }
        if (*(char *)(g_disp_flags + 6) != '\0') {
            g_disp_flags[6] = 0;
            display_request_set((int32_t)0x0804a2b8); /* request descriptor */
        }
        if (*(int16_t *)(ctx + 0x3fc) != -1) {
            matrix_draw_speed((char)*(int16_t *)(ctx + 0x3fc), *(uint32_t *)(ctx + 0x3d4),
                              *(uint32_t *)(ctx + 0x3d8), *(uint32_t *)(ctx + 0x3dc),
                              *(uint16_t *)(ctx + 0x3e0));
        }
        break;
    case 8:
        if (((*(uint32_t *)(ctx + 0x3b8) & 0x200000) == 0) ||
            (0xaaa < *(int16_t *)(ctx + 0x3f8)) ||
            (*(int16_t *)(ctx + 0x3f8) < 0x6c4)) {
            if (((*(uint32_t *)(ctx + 0x3b8) & 0x3fffff) == 0) ||
                (*(char *)(g_disp_flags + 3) == '\0')) {
                iVar4 = display_mode_set_if_changed(ctx[0x3e0] == 1);
                if (iVar4 != 0) {
                    reset_dual_buffers_and_flags();
                }
                if (*(char *)(g_disp_flags + 5) != '\0') {
                    g_disp_flags[5] = 0;
                    maybe_set_pending_request((int32_t)0x08047fcc); /* request descriptor */
                    display_request_set((int32_t)0x0804a4ec);       /* request descriptor */
                }
                if (*(char *)(g_disp_flags + 6) != '\0') {
                    g_disp_flags[6] = 0;
                    display_request_set((int32_t)0x0804a2b8);       /* request descriptor */
                }
                if (ctx[0x3e0] == 1) {
                    iVar4 = is_display_bus_ready();
                    if (iVar4 != 0) {
                        maybe_set_pending_request((int32_t)0x08047e30); /* request descriptor */
                    }
                } else {
                    iVar4 = display_request_get();
                    if ((int32_t)0x0804a2b8 != iVar4) { /* request descriptor */
                        if ((*(int16_t *)(ctx + 0x3fe) == -32000) ||
                            (*(int16_t *)(ctx + 0x3fc) == -1)) {
                            if (*(char *)(g_disp_flags + 7) != '\0') {
                                reset_dual_buffers_and_flags();
                            }
                            g_disp_flags[7] = 0;
                            iVar4 = is_display_bus_ready();
                            if (iVar4 != 0) {
                                maybe_set_pending_request((int32_t)0x0804c368); /* request descriptor */
                            }
                        } else if ((*(int16_t *)(ctx + 0x3fc) < 0x5a) ||
                                   (10 < *(int16_t *)(ctx + 0x3fe))) {
                            g_disp_flags[7] = 0;
                            iVar4 = is_display_bus_ready();
                            if (iVar4 != 0) {
                                maybe_set_pending_request((int32_t)0x0804c368); /* request descriptor */
                            }
                        } else if (*(char *)(g_disp_flags + 7) == '\0') {
                            g_disp_flags[7] = 1;
                            reset_dual_buffers_and_flags();
                            maybe_set_pending_request((int32_t)0x0804c1f4); /* request descriptor */
                        }
                    }
                }
            } else {
                g_disp_flags[4] = 8;
                set_mode_state_byte(0xc);
            }
        } else {
            set_mode_state_byte(9);
        }
        if (*(int16_t *)(ctx + 0x3fc) == -1) {
            if ((ctx[0x3e0] == 1) && (iVar4 = ctx_flag_0x131_is_clear(), iVar4 != 0)) {
                display_request_set((int32_t)0x0804ac3c); /* request descriptor */
            }
        } else {
            matrix_draw_speed((char)*(int16_t *)(ctx + 0x3fc), *(uint32_t *)(ctx + 0x3d4),
                              *(uint32_t *)(ctx + 0x3d8), *(uint32_t *)(ctx + 0x3dc),
                              *(uint16_t *)(ctx + 0x3e0));
        }
        if (*(int16_t *)(ctx + 0x3fc) != -1) {
            matrix_draw_speed((char)*(int16_t *)(ctx + 0x3fc), *(uint32_t *)(ctx + 0x3d4),
                              *(uint32_t *)(ctx + 0x3d8), *(uint32_t *)(ctx + 0x3dc),
                              *(uint16_t *)(ctx + 0x3e0));
        }
        break;
    case 9:
        iVar4 = is_display_bus_ready();
        if (iVar4 != 0) {
            maybe_set_pending_request((int32_t)0x0804693c); /* request descriptor */
        }
        if ((*(uint32_t *)(ctx + 0x3b8) & 0x200000) == 0) {
            set_mode_state_byte(8);
        }
        break;
    case 10:
        iVar4 = is_display_bus_ready();
        if (iVar4 != 0) {
            led_driver_set_shipping_mode(0x14);
            maybe_set_pending_request((int32_t)0x0804c368); /* request descriptor */
        }
        iVar4 = (int)*(int16_t *)(ctx + 0x3d2);
        if (iVar4 != -1) {
            /* signed div-by-10 of iVar4 then +9 (magic 0x66666667, >>0x22) */
            matrix_draw_speed((((int)((int64_t)(int32_t)DISP_DIV10_SMUL * (int64_t)iVar4 >> 0x22) -
                               (iVar4 >> 0x1f)) + 9U) & 0xff,
                              *(uint32_t *)(ctx + 0x3d4), *(uint32_t *)(ctx + 0x3d8),
                              *(uint32_t *)(ctx + 0x3dc), *(uint16_t *)(ctx + 0x3e0));
        }
        break;
    case 0xc:
        if (((*(uint32_t *)(ctx + 0x3b8) & 0x100000) == 0) ||
            (0xa46 < *(int16_t *)(ctx + 0x3f8)) ||
            (*(int16_t *)(ctx + 0x3f8) < 0x6c4)) {
            display_request_clear();
            maybe_set_pending_request((int32_t)0x0804c1d0); /* request descriptor */
            uVar6 = lowest_set_bit_index(*(uint32_t *)(ctx + 0x3b8), *(uint32_t *)(ctx + 0x3bc));
            matrix_draw_number(uVar6, 4);
        } else {
            display_request_clear();
            maybe_set_pending_request((int32_t)0x0804693c); /* request descriptor */
        }
        g_disp_flags[8] = g_disp_flags[4];
        set_mode_state_byte(1);
        break;
    case 0xd:
        display_request_clear();
        if (ctx[0x3c9] != 0) {
            maybe_set_pending_request((int32_t)0x0804c930); /* request descriptor */
        }
        matrix_draw_icon(ctx[0x3c9], 0xb);
        set_mode_state_byte(2);
        break;
    case 0xe:
        iVar4 = is_display_bus_ready();
        if ((iVar4 != 0) && (ctx[0x3ca] != 0)) {
            maybe_set_pending_request((int32_t)0x0804c930); /* request descriptor */
        }
        matrix_draw_icon(ctx[0x3ca], 0xb);
        *(uint16_t *)(g_mode_sm + 4) = 0;
        break;
    case 0xf:
        g_disp_flags[6] = 0;
        uVar7 = systick_now();
        /* bit-35 test of (0xCCCCCCCD * systick) == (systick/10) & 1 toggle */
        if (((uint64_t)DISP_DIV10_UMUL * (uint64_t)uVar7 & 0x800000000ULL) == 0) {
            if (ctx[0xf1] == 0) {
                if (*(char *)(g_disp_flags + 2) != '\0') {
                    maybe_set_pending_request((int)(ctx + 0xb4));
                    g_disp_flags[2] = 0;
                }
            } else if (*(char *)(g_disp_flags + 2) == '\0') {
                g_disp_flags[2] = 1;
                maybe_set_pending_request((int)(ctx + 0x3c));
            }
        } else if (ctx[0xf0] == 0) {
            if (*(char *)(g_disp_flags + 1) != '\0') {
                maybe_set_pending_request((int)(ctx + 0x78));
                g_disp_flags[1] = 0;
            }
        } else if (*(char *)(g_disp_flags + 1) == '\0') {
            g_disp_flags[1] = 1;
            maybe_set_pending_request((int)ctx);
        }
        break;
    case 0x10:
        maybe_set_pending_request((int32_t)0x0804c1d0); /* request descriptor */
        uVar6 = lowest_set_bit_index(0, 0x10000000);
        matrix_draw_number(uVar6, 4);
        set_mode_state_byte(2);
        break;
    case 0x11:
        iVar4 = is_display_bus_ready();
        if (iVar4 != 0) {
            maybe_set_pending_request((int32_t)0x0804ac3c); /* request descriptor */
        }
        break;
    case 0x12:
        iVar4 = is_display_bus_ready();
        if (iVar4 != 0) {
            maybe_set_pending_request((int32_t)0x0804bf58); /* request descriptor */
        }
        break;
    case 0x13:
        iVar4 = is_display_bus_ready();
        if (iVar4 != 0) {
            maybe_set_pending_request((int32_t)0x0804bbbc); /* request descriptor */
        }
        break;
    case 0x14:
        iVar4 = is_display_bus_ready();
        if (iVar4 != 0) {
            maybe_set_pending_request((int32_t)0x0804bb84); /* request descriptor */
        }
        break;
    case 0x15:
        iVar4 = is_display_bus_ready();
        if (iVar4 != 0) {
            maybe_set_pending_request((int32_t)0x080455c8); /* request descriptor */
        }
        break;
    case 0x16:
        set_mode_state_byte(2);
        break;
    case 0x17:
        maybe_set_pending_request((int32_t)0x080491f0); /* request descriptor */
        set_mode_state_byte(2);
        break;
    case 0x18:
        maybe_set_pending_request((int32_t)0x08048518); /* request descriptor */
        break;
    case 0x19:
        display_request_clear();
        maybe_set_pending_request((int32_t)0x0804cfd8); /* request descriptor */
        set_mode_state_byte(2);
        break;
    case 0x1a:
        maybe_set_pending_request((int32_t)0x08046658); /* request descriptor */
        set_mode_state_byte(2);
        break;
    case 0x1b:
        maybe_set_pending_request((int32_t)0x0804c104); /* request descriptor */
        set_mode_state_byte(2);
        break;
    case 0x1c:
        maybe_set_pending_request((int32_t)0x08048158); /* request descriptor */
        set_mode_state_byte(2);
        break;
    case 0x1d:
        maybe_set_pending_request((int32_t)0x080491f0); /* request descriptor */
        set_mode_state_byte(2);
        break;
    case 0x1e:
        iVar4 = is_display_bus_ready();
        if (iVar4 != 0) {
            maybe_set_pending_request((int32_t)0x0804c88c); /* request descriptor */
        }
        break;
    case 0x1f:
        iVar4 = is_display_bus_ready();
        if (iVar4 != 0) {
            maybe_set_pending_request((int32_t)0x0804c7e8); /* request descriptor */
        }
        break;
    case 0x20:
        iVar4 = is_display_bus_ready();
        if (iVar4 != 0) {
            maybe_set_pending_request((int32_t)0x0804c734); /* request descriptor */
        }
        break;
    case 0x21:
        maybe_set_pending_request((int32_t)0x0804c5f4); /* request descriptor */
        set_mode_state_byte(2);
        break;
    case 0x22:
        maybe_set_pending_request((int32_t)0x080466d4); /* request descriptor */
        set_mode_state_byte(2);
        break;
    case 0x23:
        maybe_set_pending_request((int32_t)0x08046690); /* request descriptor */
        set_mode_state_byte(2);
        break;
    case 0x24:
        display_request_clear();
        maybe_set_pending_request((int32_t)0x0804c1d0); /* request descriptor */
        uVar6 = lowest_set_bit_index(*(uint32_t *)(ctx + 0x3b8), *(uint32_t *)(ctx + 0x3bc));
        matrix_draw_number(uVar6, 4);
        set_mode_state_byte(2);
        break;
    case 0x25:
        maybe_set_pending_request((int32_t)0x080464ec); /* request descriptor */
        set_mode_state_byte(2);
        break;
    case 0x26:
        iVar4 = is_display_bus_ready();
        if (iVar4 != 0) {
            maybe_set_pending_request((int32_t)0x080471ac); /* request descriptor */
        }
        break;
    case 0x27:
        iVar4 = is_display_bus_ready();
        if (iVar4 != 0) {
            maybe_set_pending_request((int32_t)0x08046ce4); /* request descriptor */
        }
        break;
    case 0x28:
        iVar4 = is_display_bus_ready();
        if (iVar4 != 0) {
            maybe_set_pending_request((int32_t)0x08046f60); /* request descriptor */
        }
        break;
    case 0x29:
        iVar4 = is_display_bus_ready();
        if (iVar4 != 0) {
            maybe_set_pending_request((int32_t)0x08046ac8); /* request descriptor */
        }
        break;
    }
    return;
}

void display_write_reg20_init(void)
{
    /* 2-byte master transmit to I2C device 0x20: {reg=0x00, val=0x02}. */
    uint8_t buf[2];
    buf[0] = 0;
    buf[1] = 2;
    /* DAT_0803b26c = 0x20009BB8 = I2C2 device handle; dev=0x20; len=2; to=0x32 */
    FUN_08024760((void *)0x20009BB8, 0x20, buf, 2, 0x32);
}

uint16_t display_send_init_cmd(void)
{
    /* 2-byte master transmit to I2C device 0x20: {reg=0x00, val=0x01}. */
    uint8_t buf[2];
    buf[0] = 0;
    buf[1] = 1;
    /* DAT_0803b2ec = 0x20009BB8 = I2C2 device handle; dev=0x20; len=2; to=0x32 */
    return (uint16_t)FUN_08024760((void *)0x20009BB8, 0x20, buf, 2, 0x32);
}

/* matrix_draw_level_bar @ 0x0803b2f0
 * Draws a vertical level bar (param_1 = lit-segment count) into BOTH framebuffer
 * halves of the display context (G_DISP base = 0x20008230).
 *  - Top group: 4 panel rows (iRow=4..1) into half A (G_DISP+0x01, base offset -0xf == -0x10+1).
 *  - Bottom group: 3 panel rows (iRow=5..3) into half B (G_DISP+0x99, base offset +0x89 == -0x10+0x99).
 * Each row writes 3 columns (iCol=0..2). A pixel is lit (0xff) when (level - iCol) < param_1,
 * else cleared (0). 'level' starts at 2 and advances +3 (mod 256) per row.
 * Pixel byte = *(G_DISP + iRow*0x1e + iCol + <off>). Row stride 0x1e (30) matches one panel row. */
void matrix_draw_level_bar(int param_1)
{
    uint8_t *g_disp = (uint8_t *)0x20008230;   /* DAT_0803b364 = G_DISP base */
    uint32_t level;
    int iRow;
    int iCol;
    uint8_t pix;

    level = 2;
    /* half A: rows 4,3,2,1 -> G_DISP+0x01 region */
    for (iRow = 4; 0 < iRow; iRow = iRow + -1) {
        for (iCol = 0; iCol < 3; iCol = iCol + 1) {
            if ((int)(level - iCol) < param_1) {
                pix = 0xff;
            }
            else {
                pix = 0;
            }
            *(uint8_t *)(g_disp + iRow * 0x1e + iCol - 0xf) = pix;
        }
        level = (level + 3) & 0xff;
    }
    /* half B: rows 5,4,3 -> G_DISP+0x99 region */
    for (iRow = 5; 2 < iRow; iRow = iRow + -1) {
        for (iCol = 0; iCol < 3; iCol = iCol + 1) {
            if ((int)(level - iCol) < param_1) {
                pix = 0xff;
            }
            else {
                pix = 0;
            }
            *(uint8_t *)(g_disp + iRow * 0x1e + iCol + 0x89) = pix;
        }
        level = (level + 3) & 0xff;
    }
}

/*
 * Compute the flash address of the 32-bit glyph column word for one matrix
 * column. The glyph source is a flat array of uint32 columns laid out as
 * [3 x uint16 region-header words][column 0][column 1]... ; each column is one
 * uint32 word (3 bits/pixel, 6 rows -> 18 bits used). The +3 skips the
 * 3-word (6-byte, counted as 3 uint16 -> here counted in the *4 step as 3
 * words) header; param_6 (region height) and (param_3-param_4) (current row
 * minus row-base) index into the column run.
 *
 * Faithful transcription of the decompiler: result = base + word_index*4 where
 *   word_index = param_6 + (param_3 - param_4)*param_6 + param_2 + 3.
 * param_5 (uVar9 / region width) is passed but unused in the arithmetic
 * (it is consumed only by the caller's loop bounds).
 */
int matrix_glyph_src_addr(int param_1, int param_2, int param_3, int param_4,
                          uint32_t param_5, uint16_t param_6)
{
    (void)param_5; /* present in ABI, not used by the index math */
    return param_1 +
           ((uint32_t)param_6 + (param_3 - param_4) * (uint32_t)param_6 +
            param_2 + 3) * 4;
}

/*
 * Fetch the per-frame delay (in scheduler ticks) for glyph frame param_2.
 * The glyph table stores a uint16 delay word at column index (param_2 + 3),
 * i.e. just past the 3-word region header. Faithful transcription:
 *   return *(uint16_t *)(param_1 + (param_2 + 3) * 4);
 */
uint16_t matrix_glyph_frame_delay(int param_1, int param_2)
{
    return *(uint16_t *)(param_1 + (param_2 + 3) * 4);
}

/*
 * led_driver_panel_config @ 0x0803b38c
 *
 * Full power-up configuration of one IS31FL3236 LED-driver half on I2C2.
 * 'addr' is the 8-bit I2C device address (0x60 = left half, 0x66 = right half).
 *
 * IS31FL373x command protocol used throughout:
 *   reg 0xFE = 0xC5   -> unlock command register (must precede every page select)
 *   reg 0xFD = <page> -> select page (0=PWM, 2=LED-control/scaling, 4=Function)
 *   then write the page payload.
 *
 * Sequence:
 *   1. unlock, select Function page 4
 *   2. Function reg 0x00 = 0x41  (configuration register)
 *   3. Function reg 0x01 = 0x80  (global current control)
 *   4. unlock, select page 2 (LED-control / scaling), write reg 0x00 then
 *      0x96 (150) payload bytes = 0xFF (all channels enabled / full scaling)
 *   5. unlock, select page 0 (PWM) and leave it selected
 *
 * Returns 0 on full success, 1 if any I2C write failed.
 */
int led_driver_panel_config(uint8_t addr)
{
    /* I2C2 device handle (literal pool DAT_0803b4cc == 0x20009BB8) */
    void *const h = (void *)0x20009BB8;
    int rc;
    /*
     * Stack TX buffer. The page-2 step writes a register byte (buf[0]=0x00)
     * followed by 0x96 (150) payload bytes, hence 0x97 bytes total.
     * The two-byte command steps only use buf[0] (reg) and buf[1] (data).
     */
    uint8_t buf[0x97];

    /* unlock command register */
    buf[0] = 0xfe; buf[1] = 0xc5;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    /* page select 4 (Function page) */
    buf[0] = 0xfd; buf[1] = 4;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    /* Function reg 0x00 = 0x41 (configuration) */
    buf[0] = 0; buf[1] = 0x41;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    /* Function reg 0x01 = 0x80 (global current control) */
    buf[0] = 1; buf[1] = 0x80;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    /* unlock command register */
    buf[0] = 0xfe; buf[1] = 0xc5;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    /* page select 2 (LED-control / scaling page) */
    buf[0] = 0xfd; buf[1] = 2;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    /*
     * Write the whole page: reg 0x00 then 0x96 payload bytes all 0xFF.
     * memset fills the entire 0x97-byte buffer with 0xFF, then buf[0] is
     * overwritten with 0x00 (the page register/start address).
     */
    memset(buf, 0xff, 0x97);
    buf[0] = 0;
    rc = FUN_08024760(h, addr, buf, 0x97, 100);
    if (rc != 0) return 1;

    /* unlock command register */
    buf[0] = 0xfe; buf[1] = 0xc5;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    /* select page 0 (PWM page) and leave it active */
    buf[0] = 0xfd; buf[1] = 0;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    return 0;
}

uint32_t display_panel_reset(void)
{
    uint8_t tries;

    /* Bring up the LEFT LED driver (I2C addr 0x60); retry up to 3 times.
     * On every failed attempt, fire the shared output-sink callback with "NAK\r\n". */
    tries = 0;
    while (tries < 3) {
        if (led_driver_panel_config(0x60) == 0)
            break;
        /* DAT_0803b520 = 0x20009D98 -> shared SRAM object; its first word is a
         * function pointer (output/print sink). DAT_0803b524 = 0x08052D08 = "NAK\r\n". */
        (*(void (**)(const char *))0x20009D98)((const char *)0x08052D08);
        tries++;
    }
    if (tries >= 3)
        return 1;

    /* Bring up the RIGHT LED driver (I2C addr 0x66); same retry/NAK logic. */
    tries = 0;
    while (tries < 3) {
        if (led_driver_panel_config(0x66) == 0)
            break;
        (*(void (**)(const char *))0x20009D98)((const char *)0x08052D08);
        tries++;
    }
    if (tries >= 3)
        return 1;

    return 0;
}

/* matrix_draw_level_bar_blink @ 0x0803b528
 * Same vertical-bar fill as matrix_draw_level_bar, but the TOP-MOST lit cell in each
 * half is blinked on/off via a 400 ms scheduler timer.
 *  - g_disp  (DAT_0803b634) = G_DISP base 0x20008230.
 *  - g_blink_slot (DAT_0803b638) = 0x200000df : a single byte holding the scheduler slot id
 *    for the blink toggle timer. 0xFA is the scheduler 'no slot' sentinel (ARM char is
 *    UNSIGNED, so this is compared as == 0xFA). On first use a slot is allocated.
 *  - G_DISP+0x12f is the blink phase flag (XOR-toggled each timer expiry).
 *
 * Column order here is REVERSED (iCol=2..0) vs the non-blink variant so that 'lastLit'
 * tracks the LAST column scanned that is lit -> i.e. the lowest column index that is still
 * lit in the highest row scanned; that cell index is then overwritten with the blink phase.
 * lastLit for each half = ((iCol + iRow*0x1e) & 0xff) - 0x10, masked to a byte.
 * Half A blink pixel is written at G_DISP+0x01+lastLitA; half B at G_DISP+0x99+lastLitB.
 * Bottom-half blink (uVar7 != 0) takes priority; otherwise top-half blink (uVar5 != 0). */
void matrix_draw_level_bar_blink(int param_1)
{
    uint8_t *g_disp = (uint8_t *)0x20008230;     /* DAT_0803b634 = G_DISP base */
    uint8_t *g_blink_slot = (uint8_t *)0x200000df; /* DAT_0803b638 -> scheduler slot id byte */
    uint32_t level;
    uint32_t lastLitA;
    uint32_t lastLitB;
    int iRow;
    int iCol;
    int8_t pix;
    uint8_t newSlot;
    uint8_t phase;

    level = 2;
    lastLitA = 0;
    /* half A: rows 4,3,2,1 -> G_DISP+0x01 region, columns scanned 2,1,0 */
    for (iRow = 4; 0 < iRow; iRow = iRow + -1) {
        for (iCol = 2; -1 < iCol; iCol = iCol + -1) {
            if ((int)(level - iCol) < param_1) {
                pix = -1;          /* 0xff */
            }
            else {
                pix = 0;
            }
            *(int8_t *)(g_disp + iRow * 0x1e + iCol - 0xf) = pix;
            if (pix != 0) {
                lastLitA = (((iCol + iRow * 0x1e) & 0xffU) - 0x10) & 0xff;
            }
        }
        level = (level + 3) & 0xff;
    }
    lastLitB = 0;
    /* half B: rows 5,4,3 -> G_DISP+0x99 region, columns scanned 2,1,0 */
    for (iRow = 5; 2 < iRow; iRow = iRow + -1) {
        for (iCol = 2; -1 < iCol; iCol = iCol + -1) {
            if ((int)(level - iCol) < param_1) {
                pix = -1;          /* 0xff */
            }
            else {
                pix = 0;
            }
            *(int8_t *)(g_disp + iRow * 0x1e + iCol + 0x89) = pix;
            if (pix != 0) {
                lastLitB = (((iCol + iRow * 0x1e) & 0xffU) - 0x10) & 0xff;
            }
        }
        level = (level + 3) & 0xff;
    }

    /* allocate the blink timer slot on first use (0xFA == scheduler 'no slot' sentinel) */
    if (*g_blink_slot == 0xfa) {
        newSlot = scheduler_alloc();
        *g_blink_slot = newSlot;
    }
    /* when the 400 ms timer is idle: toggle phase and restart it */
    if (scheduler_slot_is_idle(*g_blink_slot) != 0) {
        *(uint8_t *)(g_disp + 0x12f) = *(uint8_t *)(g_disp + 0x12f) ^ 1;
        scheduler_start(*g_blink_slot, 400, (void *)0x0);
    }

    /* apply blink phase to the top-most lit cell; bottom half wins if it has one */
    if (lastLitB == 0) {
        if (lastLitA != 0) {
            phase = 0;
            if (*(int8_t *)(g_disp + 0x12f) != 0) {
                phase = 0xff;
            }
            *(uint8_t *)(g_disp + lastLitA + 1) = phase;
        }
    }
    else {
        if (*(int8_t *)(g_disp + 0x12f) == 0) {
            phase = 0;
        }
        else {
            phase = 0xff;
        }
        *(uint8_t *)(g_disp + lastLitB + 0x99) = phase;
    }
}

/*
 * led_driver_brightness_write @ 0x0803b63c
 *
 * Set the IS31FL3236 global current control (overall panel brightness) on one
 * driver half. 'addr' = I2C device address, 'val' = global current value.
 *
 * Sequence:
 *   0. bus busy check (FUN_0803c8a8): non-zero -> abort, return 2
 *   1. unlock, select Function page 4
 *   2. Function reg 0x01 = val   (global current control)
 *   3. unlock, select page 0 (PWM) and leave it active
 *
 * Returns 0 on success, 1 on I2C write failure, 2 if the I2C2 bus is busy.
 */
int led_driver_brightness_write(uint8_t addr, uint8_t val)
{
    void *const h = (void *)0x20009BB8;   /* DAT_0803b6fc == 0x20009BB8 */
    int rc;
    uint8_t buf[2];

    /* I2C2 busy check: 0=ok, non-zero (2)=busy */
    if (FUN_0803c8a8() != 0)
        return 2;

    /* unlock command register */
    buf[0] = 0xfe; buf[1] = 0xc5;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    /* page select 4 (Function page) */
    buf[0] = 0xfd; buf[1] = 4;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    /* Function reg 0x01 = val (global current control / brightness) */
    buf[0] = 1; buf[1] = val;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    /* unlock command register */
    buf[0] = 0xfe; buf[1] = 0xc5;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    /* select page 0 (PWM page) and leave it active */
    buf[0] = 0xfd; buf[1] = 0;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    return 0;
}

uint32_t display_module_init(void)
{
    uint8_t *ctx = G_DISP;            /* DAT_0803b734 = 0x20008230 */

    /* Clear both framebuffer halves (0x96 bytes each). */
    memset(ctx + 0x01, 0, 0x96);     /* half A */
    memset(ctx + 0x99, 0, 0x96);     /* half B */

    uint32_t rc = display_panel_reset();  /* r0 survives the strb stores -> returned */

    *(uint8_t *)(ctx + 0x130) = 0;   /* render-state */
    *(uint8_t *)(ctx + 0x131) = 0;   /* overlay-state */
    *(uint8_t *)(ctx + 0x132) = 1;   /* dirty/redraw -> force first render */
    return rc;
}

/* OEM display_request_set 0x0803B74C: latch the active overlay-request ptr and raise
 * the overlay-pending flag when non-NULL. */
void display_request_set(int32_t req)
{
    *(int32_t *)(G_DISP + 0x138) = req;
    if (req != 0)
        G_DISP[0x131] = 1;
}

/* OEM display_request_get 0x0803B760 */
int32_t display_request_get(void)
{
    return *(int32_t *)(G_DISP + 0x138);
}

/* OEM display_request_clear 0x0803B76C */
void display_request_clear(void)
{
    G_DISP[0x131] = 0;
    G_DISP[0x130] = 0;
}

/* OEM is_display_bus_ready 0x0803B7AC */
bool is_display_bus_ready(void)
{
    return G_DISP[0x130] == 0;
}

/* OEM ctx_flag_0x131_is_clear 0x0803B7C0 */
bool ctx_flag_0x131_is_clear(void)
{
    return G_DISP[0x131] == 0;
}

/* OEM display_aux_byte_get 0x0803B7D4 (g_request_ctx +0x13C) */
uint8_t display_aux_byte_get(void)
{
    return G_DISP[0x13c];
}

/*
 * Render a glyph/animation region into the two framebuffer halves of G_DISP.
 * A 4-state machine driven by G_DISP[0x130] (render-state):
 *   0: idle  -> release the render scheduler slot.
 *   1: setup -> snap region origin (x), width, height from the region-source
 *               descriptor at G_DISP+0x134 into G_DISP+0x13e/+0x140/+0x142,
 *               reset frame index (+0x144) and clear the two half-frame
 *               'cursor' bytes (+0x00 and +0x98); advance to state 2.
 *   2: draw  -> for every column in [x, x+width): decode 6 pixels (3 bits each)
 *               from the glyph column word via matrix_glyph_src_addr, look the
 *               3-bit value up in the 8-entry brightness LUT @ 0x0804f358, and
 *               store into the framebuffer. Top 5 rows (shifts 0x18,0x15,...)
 *               go to half A (base +1, row stride 0x1e), bottom 4 rows (shifts
 *               9,6,3,0) go to half B (base +0x99). Set dirty flag (+0x132)=1,
 *               advance to state 3.
 *   3: dwell -> allocate a render scheduler slot (if none, sentinel 0xFA),
 *               look up this frame's delay; if 0 use frame_ticks; if still 0
 *               go idle, else arm the scheduler. When the slot goes idle,
 *               release it, advance frame index (+0x144); if more frames remain
 *               (< +0x142) loop back to state 2, else go idle.
 *
 * Scheduler slot byte for this renderer lives at SRAM 0x200000e0 (referenced
 * both as DAT_0803b97c == &slot and as *(DAT_0803b984+1) == 0x200000df+1).
 * The 'no slot' sentinel is 0xFA (Ghidra shows '== -6' on the signed byte).
 */
void led_matrix_render_frame_region(uint16_t frame_ticks)
{
    /* file-static copy of the render brightness LUT @ 0x0804f358 (8 bytes) */
    
    uint8_t *ctx        = G_DISP;             /* 0x20008230 */
    uint8_t *slot_ptr   = (uint8_t *)0x200000e0; /* render scheduler slot id */
    int8_t   slot_id;
    int      delay;
    uint8_t  new_slot;
    int16_t  prev_frame;
    uint32_t col;
    uint32_t x_base, width;
    int      i;
    int8_t   shift;
    uint32_t *glyph;

    switch (ctx[0x130]) {
    case 0: /* idle */
        scheduler_release(slot_ptr);
        break;

    case 1: { /* setup: snap region geometry from descriptor @ +0x134 */
        uint32_t *desc = *(uint32_t **)(ctx + 0x134);
        ctx[0x00] = 0;                                  /* half-A cursor */
        ctx[0x98] = 0;                                  /* half-B cursor */
        *(int16_t *)(ctx + 0x13e) = (int16_t)desc[0];   /* x origin */
        *(int16_t *)(ctx + 0x140) = (int16_t)desc[1];   /* width    */
        *(int16_t *)(ctx + 0x142) = (int16_t)desc[2];   /* height/frames */
        *(uint16_t *)(ctx + 0x144) = 0;                 /* frame index */
        ctx[0x130] = 2;
        break;
    }

    case 2: /* draw region into both framebuffer halves */
        x_base = (uint32_t)*(uint16_t *)(ctx + 0x13e);
        /* --- half A: top 5 rows (shifts 0x18,0x15,0x12,0x0f,0x0c) --- */
        col = x_base;
        for (;;) {
            width = (uint32_t)*(uint16_t *)(ctx + 0x140);
            if ((int)(width + x_base) <= (int)col)
                break;
            i = 5;
            shift = 0x18;
            while (i > 0) {
                glyph = (uint32_t *)matrix_glyph_src_addr(
                    *(int *)(ctx + 0x134), ctx[0x144],
                    col & 0xffff, x_base, width,
                    *(uint16_t *)(ctx + 0x142));
                i = i - 1;
                ctx[col + i * 0x1e + 1] =
                    s_matrix_lut[(*glyph >> (uint8_t)shift) & 7];
                shift = (int8_t)(shift - 3);
            }
            col = col + 1;
        }
        /* --- half B: bottom 4 rows (shifts 9,6,3,0) --- */
        for (col = x_base; (int)col < (int)(width + x_base); col = col + 1) {
            i = 5;
            shift = 9;
            while (i > 1) {
                glyph = (uint32_t *)matrix_glyph_src_addr(
                    *(int *)(ctx + 0x134), ctx[0x144],
                    col & 0xffff, x_base, width,
                    *(uint16_t *)(ctx + 0x142));
                i = i - 1;
                ctx[col + i * 0x1e + 0x99] =
                    s_matrix_lut[(*glyph >> (uint8_t)shift) & 7];
                shift = (int8_t)(shift - 3);
            }
        }
        ctx[0x132] = 1;     /* dirty / redraw */
        ctx[0x130] = 3;
        break;

    case 3: /* dwell on this frame, then advance */
        slot_id = *(int8_t *)(slot_ptr); /* *(DAT_0803b984+1) */
        if (slot_id == (int8_t)0xFA) {   /* no slot yet -> arm one */
            new_slot = scheduler_alloc();
            *slot_ptr = new_slot;
            delay = matrix_glyph_frame_delay(
                *(int *)(ctx + 0x134), *(uint16_t *)(ctx + 0x144));
            *(int16_t *)(ctx + 0x146) = (int16_t)delay;
            if (delay == 0)
                *(uint16_t *)(ctx + 0x146) = frame_ticks;
            if (*(uint16_t *)(ctx + 0x146) == 0)
                ctx[0x130] = 0;          /* zero delay -> go idle */
            else
                scheduler_start(new_slot,
                                (uint32_t)*(uint16_t *)(ctx + 0x146),
                                (void *)0);
        }
        if (scheduler_slot_is_idle(*(uint8_t *)slot_ptr) != 0) {
            scheduler_release(slot_ptr);
            prev_frame = *(int16_t *)(ctx + 0x144);
            *(uint16_t *)(ctx + 0x144) = (uint16_t)(prev_frame + 1);
            if ((uint16_t)(prev_frame + 1) < *(uint16_t *)(ctx + 0x142))
                ctx[0x130] = 2;          /* more frames -> redraw */
            else
                ctx[0x130] = 0;          /* done -> idle */
        }
        break;
    }
}

/* Render-brightness LUT, RODATA @ 0x0804F358 (8 bytes).
   Indexed by the 3-bit glyph value (0..7) extracted from the source word. */

/* Overlay frame-region renderer: identical structure to the base renderer, but
   only writes pixels whose LUT value is non-zero (transparent overlay).
   DAT_0803bb20 = G_DISP request ctx (0x20008230)
   DAT_0803bb24 = scheduler-slot id object @ 0x200000E1 (passed to scheduler_release)
   DAT_0803bb28 = &s_matrix_lut[0]  (0x0804F358)
   DAT_0803bb2c = render-state object @ 0x200000DF (+2 = a scheduler slot id) */
void led_matrix_overlay_frame_region(void)
{
    uint8_t *ctx = G_DISP;            /* DAT_0803bb20 */
    char cVar1;
    short sVar2;
    uint8_t slot;
    uint32_t *puVar4;
    int iVar5;
    uint32_t uVar6;
    uint32_t *puVar7;
    int sVar8;                       /* sbyte shift count */
    uint32_t uVar9;
    uint32_t uVar10;
    uint32_t uVar11;

    switch (ctx[0x131]) {            /* overlay-state */
    case 0:
        scheduler_release((uint8_t *)0x200000E1);   /* DAT_0803bb24 */
        break;

    case 1:
        ctx[0] = 0;                 /* clear half-A base byte */
        ctx[0x98] = 0;              /* clear half-B base byte */
        puVar7 = *(uint32_t **)(ctx + 0x138);       /* region src ptr */
        *(short *)(ctx + 0x148) = (short)puVar7[0];
        *(short *)(ctx + 0x14a) = (short)puVar7[1];
        *(short *)(ctx + 0x14c) = (short)puVar7[2];
        *(uint16_t *)(ctx + 0x14e) = 0;
        ctx[0x131] = 2;
        break;

    case 2:
        uVar11 = (uint32_t)*(uint16_t *)(G_DISP + 0x148);
        uVar6 = uVar11;
        /* Half-A pass: columns, 5 rows, 3-bit fields at shifts 0x18,0x15,0x12,0xf,0xc */
        while (1) {
            uVar9 = (uint32_t)*(uint16_t *)(G_DISP + 0x14a);
            uVar10 = uVar11;
            if ((int)(uVar9 + uVar11) <= (int)uVar6)
                break;
            sVar8 = 0x18;
            for (iVar5 = 5; 0 < iVar5; iVar5 = iVar5 - 1) {
                puVar4 = (uint32_t *)matrix_glyph_src_addr(
                            *(uint32_t *)(G_DISP + 0x138),
                            G_DISP[0x14e],
                            uVar6 & 0xffff, uVar11, uVar9,
                            *(uint16_t *)(G_DISP + 0x14c));
                cVar1 = s_matrix_lut[(*puVar4 >> sVar8) & 7];   /* DAT_0803bb28 */
                if (cVar1 != '\0') {
                    /* overlay: only write non-zero LUT pixels */
                    G_DISP[uVar6 + (iVar5 - 1) * 0x1e + 1] = cVar1;
                }
                sVar8 = sVar8 - 3;
            }
            uVar6 = uVar6 + 1;
        }
        /* Half-B pass: 4 rows, 3-bit fields at shifts 9,6,3,0 */
        for (; ctx = G_DISP, (int)uVar10 < (int)(uVar9 + uVar11); uVar10 = uVar10 + 1) {
            sVar8 = 9;
            for (iVar5 = 5; 1 < iVar5; iVar5 = iVar5 - 1) {
                puVar4 = (uint32_t *)matrix_glyph_src_addr(
                            *(uint32_t *)(G_DISP + 0x138),
                            G_DISP[0x14e],
                            uVar10 & 0xffff, uVar11, uVar9,
                            *(uint16_t *)(G_DISP + 0x14c));
                cVar1 = s_matrix_lut[(*puVar4 >> sVar8) & 7];   /* DAT_0803bb28 */
                if (cVar1 != '\0') {
                    G_DISP[uVar10 + (iVar5 - 1) * 0x1e + 0x99] = cVar1;
                }
                sVar8 = sVar8 - 3;
            }
        }
        G_DISP[0x132] = 1;          /* dirty/redraw */
        ctx[0x131] = 3;
        break;

    case 3:
        /* render-state object @ 0x200000DF (DAT_0803bb2c); byte +2 = frame-delay
           scheduler slot.  0xFA is the scheduler "no slot" sentinel (Ghidra shows
           it as signed == -6). */
        if (*(uint8_t *)(0x200000DF + 2) == 0xFA) {
            slot = scheduler_alloc();
            *(uint8_t *)(0x200000DF + 2) = slot;
            uVar6 = matrix_glyph_frame_delay(
                        *(uint32_t *)(G_DISP + 0x138),
                        *(uint16_t *)(G_DISP + 0x14e));
            if (uVar6 == 0) {
                G_DISP[0x131] = 0;
            } else {
                scheduler_start(slot, uVar6, (void *)0x0);
            }
        }
        iVar5 = scheduler_slot_is_idle(*(uint8_t *)(0x200000DF + 2));
        if (iVar5 != 0) {
            scheduler_release((uint8_t *)0x200000E1);   /* DAT_0803bb24 */
            ctx = G_DISP;
            sVar2 = *(short *)(G_DISP + 0x14e);
            *(uint16_t *)(G_DISP + 0x14e) = (uint16_t)(sVar2 + 1U);
            if ((uint16_t)(sVar2 + 1U) < *(uint16_t *)(ctx + 0x14c)) {
                G_DISP[0x131] = 2;
            } else {
                G_DISP[0x131] = 0;
            }
        }
    }
    return;
}

/* OEM display_request_recovery 0x0803BB30: raise the display-freeze recovery flag
 * consumed by led_matrix_transmit_step. */
void display_request_recovery(void)
{
    G_DISP[0x150] = 1;
}

/*
 * led_matrix_transmit_step @ 0x0803bb40
 *
 * I2C2-DMA frame pump. A small state machine (G_DISP[+0x151]) that DMA-writes
 * the two 0x97-byte framebuffer halves to the left LED driver (I2C addr 0x60)
 * and the right LED driver (I2C addr 0x66). On scheduler-slot timeout / DMA
 * lockup it performs an I2C2 bus-recovery (clock-out + re-init) and logs
 * " ERR dsp freeze\r\n".
 *
 * G_DISP (g_request_ctx) = (uint8_t *)0x20008230
 *   +0x132 = dirty/redraw flag (request a new transmit)
 *   +0x13c = per-half DMA-complete flag (set by the I2C2 TX-complete callback)
 *   +0x150 = bus-error / freeze flag (forces recovery on entry)
 *   +0x151 = transmit state machine index (0..4)
 *   +0x00  = framebuffer half A (0x97 bytes incl. leading reg addr)
 *   +0x98  = framebuffer half B (0x97 bytes incl. leading reg addr)
 *
 * Scheduler slot id is held at *(uint8_t *)0x200000e2  (== G_TX_SLOT[+3]).
 * 0xFA (decompiler shows -6) is the "no slot allocated" sentinel.
 */
uint32_t led_matrix_transmit_step(void)
{
    /* base of the object that carries the tx scheduler-slot id at byte +3 */
    uint8_t *g_tx_slot_obj = (uint8_t *)0x200000df;   /* DAT_0803bc98 */
    uint8_t *g_disp        = G_DISP;                  /* DAT_0803bc9c == 0x20008230 */
    uint8_t *slot_ptr      = (uint8_t *)0x200000e2;   /* DAT_0803bca0 == g_tx_slot_obj+3 */
    void *log_obj          = (void *)0x20009d98;      /* DAT_0803bca4: log fn-ptr container */

    disp_gpio_init_t gpio_init;
    uint8_t  pulses;
    int      idle;
    uint32_t ret;

    /* ---- entry: detect a frozen transfer (slot idle) or a flagged bus error ---- */
    idle = scheduler_slot_is_idle(g_tx_slot_obj[3]);
    if ((idle != 0) || (*(int8_t *)(g_disp + 0x150) != 0)) {
        /* release the tx scheduler slot (pass address of the slot-id byte) */
        scheduler_release(slot_ptr);
        g_disp[0x150] = 0;

        /* log " ERR dsp freeze\r\n" via the runtime log fn-ptr */
        log_print_timestamp_prefix();
        (*(void (**)(const char *))log_obj)(
            (const char *)0x08052d10 /* " ERR dsp freeze\r\n" */);

        /* tear down I2C2 and bit-bang a clock-recovery on the bus */
        FUN_0803c8d4();   /* I2C2 deinit */

        pulses = 0;

        /* configure SCL recovery pin: GPIOB pin 6 (0x40), output, no-pull, low speed */
        gpio_init.Mode  = 1;   /* GPIO_MODE_OUTPUT_PP */
        gpio_init.Pull  = 0;
        gpio_init.Speed = 0;
        gpio_init.Alternate = 0;
        gpio_init.Pin   = 0x40;
        HAL_GPIO_Init((void *)0x40020400 /* GPIOB */, &gpio_init);

        /* clock out up to 200 SCL pulses on GPIOA pin 8 until GPIOC pin 9 (SDA)
         * goes high (slave releases the bus) */
        while ((pulses < 200) &&
               (HAL_GPIO_ReadPin((void *)0x40020800 /* GPIOC */, 0x200) == 0)) {
            HAL_GPIO_WritePin((void *)0x40020000 /* GPIOA */, 0x100, 1);
            systick_delay(1);
            HAL_GPIO_WritePin((void *)0x40020000 /* GPIOA */, 0x100, 0);
            systick_delay(1);
            pulses = (uint8_t)(pulses + 1);
        }

        i2c2_init();
        g_disp[0x151] = 0;   /* reset transmit state machine */
    }

    /* ---- transmit state machine ---- */
    ret = 0;
    switch (g_disp[0x151]) {
    case 0:
        /* idle: kick off a transfer if a redraw is pending */
        if (*(int8_t *)(g_disp + 0x132) != 0) {
            /* allocate + start the freeze-watchdog scheduler slot if none held
             * (0xFA == -6 == "no slot") */
            if (g_tx_slot_obj[3] == 0xFA) {
                uint8_t slot = scheduler_alloc();
                g_tx_slot_obj[3] = slot;
                scheduler_start(slot, 100, (void *)0);
            }
            g_disp[0x132] = 0;   /* consume the redraw request */
            g_disp[0x151] = 1;   /* -> send half A */
        }
        break;

    case 1:
        /* DMA-write half A (0x97 bytes) to left driver @ I2C addr 0x60 */
        ret = (uint32_t)FUN_08024bc0((void *)0x20009bb8 /* I2C2 handle */,
                                     0x60, g_disp, 0x97);
        g_disp[0x13c] = 0;       /* clear DMA-complete flag */
        g_disp[0x151] = 2;       /* -> wait for half A complete */
        break;

    case 2:
        /* wait for half A DMA-complete callback */
        if (*(int8_t *)(g_disp + 0x13c) == 0) {
            ret = 0;
        } else {
            g_disp[0x151] = 3;   /* -> send half B */
            ret = 0;
        }
        break;

    case 3:
        /* DMA-write half B (0x97 bytes @ +0x98) to right driver @ I2C addr 0x66 */
        ret = (uint32_t)FUN_08024bc0((void *)0x20009bb8 /* I2C2 handle */,
                                     0x66, g_disp + 0x98, 0x97);
        g_disp[0x13c] = 0;       /* clear DMA-complete flag */
        g_disp[0x151] = 4;       /* -> wait for half B complete */
        break;

    case 4:
        /* wait for half B DMA-complete callback; then release watchdog slot */
        if (*(int8_t *)(g_disp + 0x13c) == 0) {
            ret = 0;
        } else {
            scheduler_release(slot_ptr);
            g_disp[0x151] = 0;   /* back to idle */
            ret = 0;
        }
        break;

    default:
        ret = 0;
        break;
    }

    return ret;
}

/* OEM display_set_aux_flag 0x0803BCBC (g_request_ctx +0x13C) */
void display_set_aux_flag(void)
{
    G_DISP[0x13c] = 1;
}

/* matrix_set_corner_led @ 0x0803bccc
 *   Lights exactly one of the four framebuffer "corner" LEDs (selected by param_1
 *   in 1..4) at brightness 0x50 and clears the other three. param_1==0 or >4 leaves
 *   the four corner bytes untouched (the switch falls through). The current corner
 *   selection is tracked in a small SRAM object at 0x200000df (field +4); when it
 *   changes, the display dirty/redraw flag (G_DISP+0x132) is raised.
 *   Corner-LED byte offsets in G_DISP: +0x66, +0x2a, +0x11c, +0xe0. */
void matrix_set_corner_led(uint32_t param_1)
{
    uint8_t *disp = G_DISP;                         /* DAT_0803bd50 = 0x20008230 */
    uint8_t *trk  = (uint8_t *)0x200000df;          /* DAT_0803bd54 = corner-LED state tracker */

    switch (param_1) {
    case 1:
        *(uint8_t *)(disp + 0x66)  = 0x50;
        *(uint8_t *)(disp + 0x2a)  = 0;
        *(uint8_t *)(disp + 0x11c) = 0;
        *(uint8_t *)(disp + 0xe0)  = 0;
        break;
    case 2:
        *(uint8_t *)(disp + 0x66)  = 0;
        *(uint8_t *)(disp + 0x2a)  = 0x50;
        *(uint8_t *)(disp + 0x11c) = 0;
        *(uint8_t *)(disp + 0xe0)  = 0;
        break;
    case 3:
        *(uint8_t *)(disp + 0x66)  = 0;
        *(uint8_t *)(disp + 0x2a)  = 0;
        *(uint8_t *)(disp + 0x11c) = 0x50;
        *(uint8_t *)(disp + 0xe0)  = 0;
        break;
    case 4:
        *(uint8_t *)(disp + 0x66)  = 0;
        *(uint8_t *)(disp + 0x2a)  = 0;
        *(uint8_t *)(disp + 0x11c) = 0;
        *(uint8_t *)(disp + 0xe0)  = 0x50;
        break;
    }

    /* trk[+4] is an unsigned byte (ARM char is unsigned); compare against the
     * full param_1 word and store the truncated low byte, exactly as the asm
     * (ldrb / strb) does. */
    if (*(uint8_t *)(trk + 4) != param_1) {
        *(uint8_t *)(trk + 4) = (uint8_t)param_1;
        *(uint8_t *)(disp + 0x132) = 1;            /* mark display dirty/redraw */
    }
}

/* matrix_draw_speed @ 0x0803bd58
 *
 * Speed/number animation dispatcher for the dot-matrix display.
 *
 * param_1 (speed): unsigned speed value (km/h-ish units). Selects which
 *   static glyph/region descriptor to show, or (>=0x17) the animated
 *   running-speed bar.
 * param_2..param_4: forwarded animation/region args; only param_2 is
 *   consulted here, as a SIGNED byte (the decompiler reads it with
 *   LDRSB, so the compare "param_2 < 6" is a signed-char compare).
 * param_5 (blink): 1 -> draw the level bar in blinking mode; else solid.
 *
 * Gated by ctx_flag_0x131_is_clear(): if an overlay request is already
 * pending (G_DISP+0x131 != 0) the whole call is a no-op.
 *
 * The object at 0x200000df is a separate display-brightness/ramp object;
 * byte +5 is a 0..100 brightness ramp counter that this routine drives.
 * G_DISP (0x20008230) holds the tick (+0x154), blink flag (+0x12f) and
 * dirty/redraw flag (+0x132).
 */
void matrix_draw_speed(uint32_t speed, char param_2, uint32_t param_3,
                       uint32_t param_4, char blink)
{
    /* 0x200000df: display brightness/ramp object (byte +5 = 0..100 ramp) */
    uint8_t *bright_obj = (uint8_t *)0x200000df; /* DAT_0803be60 */
    uint8_t level;
    uint32_t now;

    (void)param_2; (void)param_3; (void)param_4; /* stored to frame, only param_2 used below */

    if (ctx_flag_0x131_is_clear() == 0)
        return;

    if (speed < 6) {
        bright_obj[5] = 100;
        matrix_draw_level_bar(0);
        /* DAT_0803be68 -> 0x0804aad4 (static glyph/region descriptor) */
        display_request_set((int32_t)0x0804aad4);
    }
    else if (speed < 0xe) {
        bright_obj[5] = 100;
        matrix_draw_level_bar(0);
        /* DAT_0803be6c -> 0x0804a990 (static glyph/region descriptor) */
        display_request_set((int32_t)0x0804a990);
    }
    else if (speed < 0x13) {
        bright_obj[5] = 100;
        matrix_draw_level_bar(0);
        /* DAT_0803be70 -> 0x0804a84c (static glyph/region descriptor) */
        display_request_set((int32_t)0x0804a84c);
    }
    else if (speed < 0x17) {
        bright_obj[5] = 100;
        matrix_draw_level_bar(0);
        /* DAT_0803be64 -> 0x0804a708 (static glyph/region descriptor) */
        display_request_set((int32_t)0x0804a708);
    }
    else {
        /* Animated running-speed branch: one update per systick tick. */
        now = systick_now();
        if (now != *(uint32_t *)(G_DISP + 0x154)) {
            now = systick_now();
            *(uint32_t *)(G_DISP + 0x154) = now;

            /* ramp brightness up toward 100 */
            if (bright_obj[5] < 100) {
                bright_obj[5] = (uint8_t)(bright_obj[5] + 1);
            }

            /* scale speed 7..0x5c -> bar level 0..0x15 (clamp+linear) */
            level = (uint8_t)FUN_0803a588(speed, 7, 0x5c, 0, 0x15);

            if (blink == 1) {
                /* param_2 read as signed char (LDRSB at sp+0x14) */
                if (param_2 < 6) {
                    if (*(char *)(G_DISP + 0x12f) == 0) {
                        /* DAT_0803be7c -> 0x0804ac3c (running-bar region descriptor) */
                        display_request_set((int32_t)0x0804ac3c);
                    }
                    else {
                        bright_obj[5] = 0;
                        /* DAT_0803be78 -> 0x0804ac18 (running-bar region descriptor, blank phase) */
                        display_request_set((int32_t)0x0804ac18);
                    }
                }
                else {
                    /* DAT_0803be7c -> 0x0804ac3c (running-bar region descriptor) */
                    display_request_set((int32_t)0x0804ac3c);
                }
                matrix_draw_level_bar_blink((uint8_t)level);
            }
            else {
                /* DAT_0803be7c -> 0x0804ac3c (running-bar region descriptor) */
                display_request_set((int32_t)0x0804ac3c);
                matrix_draw_level_bar((uint8_t)level);
            }

            *(uint8_t *)(G_DISP + 0x132) = 1; /* mark dirty/redraw */
        }
    }
}

/* matrix_set_turn_indicator @ 0x0803be8c
 *   Sets the left/right turn-indicator arrows in the framebuffer.
 *     param_1 == 1 -> left arrow on  (G_DISP+0x01 = 0x50), else 0
 *     param_1 == 3 -> right arrow on (G_DISP+0x1f and G_DISP+0x111 = 0x50), else 0
 *   Note: G_DISP+0x1f and G_DISP+0x111 are written with the SAME value computed
 *   for the param_1==3 test (0x50 when ==3, otherwise 0), matching the asm which
 *   reuses r2 for both stores. The current turn state is tracked in G_DISP+0x158;
 *   when it changes the dirty/redraw flag (G_DISP+0x132) is raised. */
void matrix_set_turn_indicator(uint32_t param_1)
{
    uint8_t *disp = G_DISP;                         /* DAT_0803bec4 = 0x20008230 */
    uint8_t v;

    v = (param_1 == 1) ? 0x50 : 0;
    *(uint8_t *)(disp + 0x01) = v;                  /* left arrow */

    v = (param_1 == 3) ? 0x50 : 0;
    *(uint8_t *)(disp + 0x1f)  = v;                 /* right arrow (cell A) */
    *(uint8_t *)(disp + 0x111) = v;                 /* right arrow (cell B) */

    /* G_DISP+0x158 turn-state tracker is an unsigned byte (ldrb); compare against
     * the full param_1 word and store the truncated low byte (strb). */
    if (*(uint8_t *)(disp + 0x158) != param_1) {
        *(uint8_t *)(disp + 0x158) = (uint8_t)param_1;
        *(uint8_t *)(disp + 0x132) = 1;            /* mark display dirty/redraw */
    }
}

/*
 * matrix_draw_icon @ 0x0803bec8
 *   param_1 = icon glyph index, param_2 = horizontal pixel offset (column base).
 * Bit-unpacks a 5x7 icon glyph from the shared font/glyph blob at flash 0x0804F358
 * into the two framebuffer halves of G_DISP. The glyph blob layout (decoded from
 * rodata 0x0804F358):
 *   [0..7]    : 8-byte render brightness LUT  {0,4,8,16,32,64,128,255}
 *   [8..139]  : icon glyph rows (7 rows per icon, 5 valid bits each)  <- used here
 *   [140..209]: digit glyph rows (10 digits x 7 rows)                 <- matrix_draw_number
 * Icon row byte: bit4..bit0 = the 5 columns (left->right). Columns 0,1,2 (bits 4,3,2)
 * land in framebuffer half A (G_DISP+0x01), columns 3,4 (bits 1,0) land in half B
 * (G_DISP+0x99). Each lit pixel is written 0xFF, dark 0x00. Sets the dirty flag at +0x132.
 * NOTE: ARM char is unsigned; the row->bit shift uses an unsigned byte shift count,
 * mirrored here with explicit unsigned masking.
 */
void matrix_draw_icon(int param_1, int param_2)
{
    /* Shared font/glyph blob @ flash 0x0804F358 (210 bytes). Indexed exactly as the
     * disassembly does: icon rows at base+8, *7 stride. */
        uint8_t *disp = G_DISP; /* 0x20008230 */
    int row;
    int col;
    uint8_t shift;
    uint8_t pix;
    uint8_t glyph_byte;

    /* Half A: columns 3,2,1 mapped from bits 4,3,2 -> framebuffer +0x01 */
    for (row = 0; row < 7; row = row + 1) {
        shift = 4;
        for (col = 3; 0 < col; col = col + -1) {
            glyph_byte = s_glyph_blob[param_1 * 7 + row + 8];
            if (((glyph_byte >> (shift & 0xff)) & 1) == 0) {
                pix = 0;
            } else {
                pix = 0xff;
            }
            *(uint8_t *)(disp + (col * 0x1e - (0x1e - param_2)) + row + 1) = pix;
            shift = (uint8_t)(shift - 1);
        }
    }
    /* Half B: columns 5,4 mapped from bits 1,0 -> framebuffer +0x99 */
    for (row = 0; row < 7; row = row + 1) {
        shift = 1;
        for (col = 5; 3 < col; col = col + -1) {
            glyph_byte = s_glyph_blob[param_1 * 7 + row + 8];
            if (((glyph_byte >> (shift & 0xff)) & 1) == 0) {
                pix = 0;
            } else {
                pix = 0xff;
            }
            *(uint8_t *)(disp + (col * 0x1e - (0x1e - param_2)) + row + 0x99) = pix;
            shift = (uint8_t)(shift - 1);
        }
    }
    *(uint8_t *)(disp + 0x132) = 1; /* dirty / redraw flag */
}

/*
 * matrix_draw_number @ 0x0803bfa0
 *   param_1 = number to render (0..99, capped to 99 if larger), param_2 = pixel x-offset.
 * Renders up to two decimal digits using the digit glyph rows of the shared font/glyph
 * blob at flash 0x0804F358 (digit rows at base+0x8c, 7 bytes/digit, 4 valid columns).
 * Tens digit -> framebuffer half A (G_DISP+0x01), units digit -> half B (G_DISP+0x99).
 *   - param_1 > 99 : clamp to 99 (uVar5 tens=9).
 *   - param_1 == 0 : clear both halves (all dark).
 *   - param_1 < 10 : leading tens column is blanked (drawn as 0).
 * Division by 10 is the reciprocal-multiply  (0xCCCCCCCD * x) >> 35  (umull hi word then >>3).
 * Sets the dirty flag at +0x132.
 * NOTE: the (param_1 == 0xFE) special-case in the <100 branch is dead in practice (a value
 * <100 is never 0xFE) but is preserved for faithfulness; it routes to the clear-both path.
 * NOTE: bit shift counts are unsigned-byte (iVar2-2 masked to a byte), mirrored here.
 */
void matrix_draw_number(uint32_t param_1, int param_2)
{
        uint8_t *disp = G_DISP; /* 0x20008230 */
    uint32_t tens;
    uint32_t units;
    int row;
    int col;
    int addr;
    uint8_t pix;
    uint8_t glyph_byte;

    if (param_1 < 100) {
        tens = (uint32_t)(((uint64_t)0xCCCCCCCD * (uint64_t)param_1) >> 0x23); /* param_1 / 10 */
        if (param_1 == 0xfe) {
            row = 0;
            goto clear_both; /* dead in practice (param_1<100) */
        }
    } else {
        tens = 9;
        param_1 = 99;
    }

    if (param_1 != 0) {
        /* Half A: tens digit (blanked if param_1 < 10) at framebuffer +0x01 */
        for (row = 0; row < 7; row = row + 1) {
            for (col = 5; 1 < col; col = col + -1) {
                if (param_1 < 10) {
                    *(uint8_t *)(disp + (col * 0x1e - (0x1e - param_2)) + row + 1) = 0;
                } else {
                    glyph_byte = s_glyph_blob[tens * 7 + row + 0x8c];
                    if (((glyph_byte >> ((col - 2U) & 0xff)) & 1) == 0) {
                        pix = 0;
                    } else {
                        pix = 0xff;
                    }
                    *(uint8_t *)(disp + (col * 0x1e - (0x1e - param_2)) + row + 1) = pix;
                }
            }
        }
        /* units = param_1 - (param_1/10)*10 */
        units = (param_1 + (uint32_t)(((uint64_t)0xCCCCCCCD * (uint64_t)param_1) >> 0x23) * -10) & 0xff;
        /* Half B: units digit at framebuffer +0x99 */
        for (row = 0; row < 7; row = row + 1) {
            for (col = 5; 1 < col; col = col + -1) {
                glyph_byte = s_glyph_blob[units * 7 + row + 0x8c];
                if (((glyph_byte >> ((col - 2U) & 0xff)) & 1) == 0) {
                    pix = 0;
                } else {
                    pix = 0xff;
                }
                *(uint8_t *)(disp + (col * 0x1e - (0x1e - param_2)) + row + 0x99) = pix;
            }
        }
        goto set_dirty;
    }
    row = 0;

clear_both:
    /* param_1 == 0 (or the dead 0xFE case): clear both halves */
    for (; row < 7; row = row + 1) {
        for (col = 5; 1 < col; col = col + -1) {
            addr = (col * 0x1e - (0x1e - param_2)) + row + (int)(uintptr_t)disp;
            *(uint8_t *)(addr + 1) = 0;
            *(uint8_t *)(addr + 0x99) = 0;
        }
    }

set_dirty:
    *(uint8_t *)(disp + 0x132) = 1; /* dirty / redraw flag */
}

/* led_driver_set_shipping_mode @ 0x0803c0e4
 * Pushes the shipping-mode brightness setting (mode) to both IS31FL3236 halves
 * (left @ I2C 0x60, right @ 0x66) only when the cached "ship" state at
 * G_DISP+0x159 changes. Each chip gets up to 3 attempts; on every failed
 * attempt it logs "NAK\r\n" via g_log_func.
 * mode: ARM char is unsigned -> keep as uint8_t. */
void led_driver_set_shipping_mode(uint8_t mode)
{
    int rc;
    uint8_t retry;

    if (*(uint8_t *)(G_DISP + 0x159) != mode) {
        *(uint8_t *)(G_DISP + 0x159) = mode;

        /* left half @ 0x60: up to 3 tries */
        retry = 0;
        while (retry < 3 && (rc = led_driver_brightness_write(0x60, mode), rc != 0)) {
            g_log_func("NAK\r\n");   /* DAT_0803c13c=g_log_func obj @0x20009d98, arg DAT_0803c140=0x08052d08 */
            retry = (uint8_t)(retry + 1);
        }

        /* right half @ 0x66: up to 3 tries */
        retry = 0;
        while (retry < 3 && (rc = led_driver_brightness_write(0x66, mode), rc != 0)) {
            g_log_func("NAK\r\n");
            retry = (uint8_t)(retry + 1);
        }
    }
}

/*
 * led_driver_standby_write @ 0x0803c144
 *
 * Put one IS31FL3236 driver half into software shutdown / standby.
 * 'addr' = I2C device address.
 *
 * Sequence:
 *   0. bus busy check (FUN_0803c8a8): non-zero -> abort, return 2
 *   1. unlock, select Function page 4
 *   2. Function reg 0x00 = 0x00  (configuration register: SSD=0 -> software shutdown)
 *
 * Returns 0 on success, 1 on I2C write failure, 2 if the I2C2 bus is busy.
 */
int led_driver_standby_write(uint8_t addr)
{
    void *const h = (void *)0x20009BB8;   /* DAT_0803c1bc == 0x20009BB8 */
    int rc;
    uint8_t buf[2];

    /* I2C2 busy check: 0=ok, non-zero (2)=busy */
    if (FUN_0803c8a8() != 0)
        return 2;

    /* unlock command register */
    buf[0] = 0xfe; buf[1] = 0xc5;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    /* page select 4 (Function page) */
    buf[0] = 0xfd; buf[1] = 4;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    /* Function reg 0x00 = 0x00 -> software shutdown (standby) */
    buf[0] = 0; buf[1] = 0;
    rc = FUN_08024760(h, addr, buf, 2, 100);
    if (rc != 0) return 1;

    return 0;
}

/* led_driver_enter_shipping_mode @ 0x0803c1c0
 * Drives both IS31FL3236 halves (left @ 0x60, right @ 0x66) into standby/shutdown
 * for shipping. Each chip is retried up to 3 times; a failed attempt logs
 * "NAK\r\n" via g_log_func. Returns 0 on success, 1 if either chip's retry
 * budget (3) is exhausted.
 *
 * NOTE on control-flow: the disassembly enters the left-half loop via a forward
 * branch to the condition test (b 0x803c1d2), so the very first retry-counter
 * increment + log only happens AFTER a failed brightness/standby write. The
 * structure below reproduces that: the left loop logs on entry to the body,
 * which is only reached after a non-zero return. retry<3 is "r4<=2" (cmp #2/bhi).
 */
uint32_t led_driver_enter_shipping_mode(void)
{
    int rc;
    uint32_t result;
    uint8_t retry;

    /* left half @ 0x60 */
    retry = 0;
    while (1) {
        if (retry < 3) {
            rc = led_driver_standby_write(0x60);
            if (rc == 0)
                break;
        } else {
            break;
        }
        g_log_func("NAK\r\n");   /* DAT_0803c210=g_log_func obj @0x20009d98, arg DAT_0803c214=0x08052d08 */
        retry = (uint8_t)(retry + 1);
    }

    if (retry < 3) {
        /* right half @ 0x66 */
        retry = 0;
        while (retry < 3 && (rc = led_driver_standby_write(0x66), rc != 0)) {
            g_log_func("NAK\r\n");
            retry = (uint8_t)(retry + 1);
        }
        if (retry < 3)
            result = 0;   /* both halves quiesced */
        else
            result = 1;   /* right half retries exhausted */
    } else {
        result = 1;       /* left half retries exhausted */
    }
    return result;
}

/* OEM lowest_set_bit_index 0x0803FBE4: index (0..0x3F) of the lowest set bit of the
 * 64-bit value (lo|hi), else 0x62. Maps a state-flag bitset to an icon index. */
static uint32_t lowest_set_bit_index(uint32_t lo, uint32_t hi)
{
    uint32_t i = 0;
    do {
        if (((lo >> (i & 0xff)) | (hi << ((0x20 - i) & 0xff)) | (hi >> ((i - 0x20) & 0xff))) & 1)
            return i & 0xff;
        i++;
    } while (i < 0x40);
    return 0x62;
}
