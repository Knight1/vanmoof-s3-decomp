# mainware — LED-matrix display engine (`display.c`)

`src/display.c` is the bike's **signature dot-matrix display**: the top-tube LED
matrix that shows speed, battery %, icons and animations. The panel is two
Lumissil **IS31FL3236** constant-current LED-driver chips on **I2C2**, fed from a
dual-half framebuffer in SRAM; this module is the driver, the framebuffer render
+ draw API, the I2C-DMA frame transmitter, and the display-mode presenter.

**Sourced** as faithful, behaviour-equivalent C (33 functions), transcribed by a
fan-out workflow and **adversarially verified** against the live disassembly
(every offset, shift, mask, glyph table and log string re-checked byte-for-byte).

## Panel hardware (I2C2, two IS31FL3236 halves)

| | |
| --- | --- |
| Bus | I2C2 (handle struct @ SRAM `0x20009BB8`, shared with the light sensor) |
| Left half | LED driver @ I2C device **0x60** |
| Right half | LED driver @ I2C device **0x66** |
| Panel sub-controller | I2C device **0x20** (`display_send_init_cmd` / `display_write_reg20_init`) |
| Geometry | 5×7 cells, 30-column (`0x1e`) rows, **two halves** (top + bottom) |

The IS31FL373x command protocol used throughout: write reg **`0xFE` = `0xC5`**
(unlock the command register), then **`0xFD` = _page_** (0 = PWM, 2 = LED-scaling,
4 = Function), then the page payload. `led_driver_panel_config` does the full
power-up: Function page reg `0x00`=`0x41` (config) + `0x01`=`0x80` (global current),
then writes the `0x96`-byte scaling page = `0xFF`, then selects the PWM page.
`led_driver_brightness_write` / `led_driver_standby_write` / the shipping-mode
helpers issue shorter page writes. On any failure the path logs `"NAK\r\n"`.

## Framebuffer (g_request_ctx @ 0x20008230)

The display context is the same SRAM object as the request/announce context
(`g_request_ctx`, OEM `reset_dual_buffers_and_flags`/`maybe_set_pending_request`):

| offset | field |
| --- | --- |
| `+0x01` | framebuffer **half A** (0x96 bytes → left driver @0x60) |
| `+0x99` | framebuffer **half B** (0x96 bytes → right driver @0x66) |
| `+0x130` | render-state (0 = bus idle, see `is_display_bus_ready`) |
| `+0x131` | overlay-state (`ctx_flag_0x131_is_clear`) |
| `+0x132` | dirty/redraw flag (raised by every draw) |
| `+0x134`/`+0x138` | render / overlay region source pointers |
| `+0x13e..+0x14e` | render & overlay region cursor vars |
| `+0x150`/`+0x151` | frame-DMA recovery flag / DMA sub-state |
| `+0x2a`,`+0x66`,`+0xe0`,`+0x11c` | the four corner-indicator LED cells |
| `+0x158`/`+0x159` | turn-indicator latch / shipping-mode latch |

Pixel address into a half: `base_off + row*0x1e + col` (half A `base_off = +0x01`,
half B `+0x99`). Brightness is 3 bits/pixel through the LUT
`{00,04,08,10,20,40,80,FF}` at flash rodata **`0x0804F358`** — the same blob holds
the 5×7 icon glyphs (`+0x08`) and digit glyphs (`+0x8C`).

## Render → transmit pipeline (super-loop)

1. `led_matrix_render_frame_region(frame_ticks)` — render the current
   animation/glyph region into both halves (uses `matrix_glyph_src_addr` for the
   per-pixel source address and `matrix_glyph_frame_delay` for the frame timing).
2. `led_matrix_overlay_frame_region()` — overlay pass: writes only the non-zero
   LUT pixels on top of the rendered frame.
3. `led_matrix_transmit_step()` — when the framebuffer is dirty, **I2C-DMA** the
   two `0x97`-byte halves to the drivers (`0x60` then `0x66`). If the I2C2 bus is
   hung it recovers: logs `" ERR dsp freeze\r\n"` (VanMoof abbreviates display →
   "dsp"), deinits I2C2, bit-bangs up to 200 SCL pulses on PA8 until SDA (PC9)
   releases, then re-inits I2C2. The super-loop logs `"ERR Display\r\n"` on a
   non-zero return.

## Draw API (writes the framebuffer, raises the dirty flag)

- `matrix_draw_number(value, x0)` — two-digit decimal (font `+0x8C`).
- `matrix_draw_speed(speed, …, blink)` — speed value with small-number static vs
  animated handling; ramps brightness (`g_request_ctx+5`) and uses the
  clamp/scale helper.
- `matrix_draw_icon(glyph, x0)` — 5×7 icon from font `+0x08`.
- `matrix_draw_level_bar(level)` / `matrix_draw_level_bar_blink(level)` — vertical
  level bar (e.g. battery), blink variant toggles the lit pixel via a 400-tick
  scheduler slot.
- `matrix_set_corner_led(1..4)` — light one of the four corner cells (`0x50`).
- `matrix_set_turn_indicator(1|3)` — left / right turn arrow.

## Display-mode presenter (`display_mode_sm_step`)

A ~40-case state machine (jump table, cases 0..0x29; 2 and 0xB fall through to a
no-op) driven from the super-loop with the app context. It reads the app-context
display fields (speed `+0x3c2`, digit `+0x3fc`/`+0x3fe`, payload `+0x3d4..+0x3e0`,
state-flag bitset `+0x3b8`, mode flags `+0xf0`/`+0xf1`, codes `+0x3c9`/`+0x3ca`),
picks what to show, and drives the draw API + `maybe_set_pending_request` against
flash request descriptors. `lowest_set_bit_index` maps the 64-bit state-flag
bitset to an alert-icon index; `led_driver_set_shipping_mode` is invoked from the
ship-mode case. `set_mode_state_byte`/`display_mode_set_if_changed` advance the SM.

## Notes

- `led_matrix_transmit_step` was previously mislabelled `dsp_recovery_telemetry_pump`
  ("dsp" in the log string is **display**, not the C28x motor DSP) — corrected.
- ARM `char` is unsigned: the scheduler "no slot" sentinel `0xFA` (which Ghidra
  renders as a signed `== -6`) is compared as `== 0xFA` throughout.
