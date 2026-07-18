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

A ~40-case state machine (jump table, cases `0`..`0x29`; `2` and `0xB` fall
through to a no-op) driven from the super-loop with the app context. It reads the
app-context display fields (speed `+0x3c2`, digit `+0x3fc`/`+0x3fe`, payload
`+0x3d4..+0x3e0`, fault bitset `+0x3b8`/`+0x3bc`, pack voltage `+0x3f8`, mode
flags `+0xf0`/`+0xf1`, codes `+0x3c9`/`+0x3ca`), picks what to show, and drives the
draw API + `maybe_set_pending_request` against flash request descriptors.

The **mode byte is `g_mode_sm[0]` @ `0x20000068`** — the switch selector, written
by `set_mode_state_byte(m)` (`app.c`) from all over the app (states / shifter /
modem / OTA / testmode) to request a screen. `display_mode_set_if_changed` shadows
it into `g_disp_flags[0]` @ `0x20000288`. Two small SRAM control blocks back the SM:

`g_mode_sm` @ `0x20000068`:

| byte | role |
| --- | --- |
| `[0]` | **current mode** (switch selector) |
| `[1]` | previous mode — edge detect; on change clears the per-frame latches |
| `[2]` | blink-timer scheduler slot; each expiry toggles `g_disp_flags[3]` |
| `[4..5]` | last-drawn speed (uint16 cache — suppresses redundant redraws) |
| `[6]` | `"slow_show_speed_tmr"` slot — ~1 s speed-refresh cadence (mode 7) |
| `[7]` | timed-show slot — auto-return for modes 3/4 |

`g_disp_flags` @ `0x20000288`: `[1]`/`[2]` main/secondary frame latches, `[3]`
**blink phase** (0/1, toggled by `[2]`'s timer — gates the fault display so the
error frame alternates with the normal screen), `[4]` saved *return* mode, `[5]`/`[6]`
overlay-request latches (turn signals / notifications), `[7]` battery-frame blink
latch, `[8]` restore-target mode consumed by mode 1.

### Operational modes

| Mode | Screen | Transition |
| --- | --- | --- |
| `0` | init — wait for the display bus, reset announce records | → `6` |
| `1` | restore the saved mode | → `g_disp_flags[8]` |
| `2`, `0xB` | idle / no-op (terminal — a screen was already drawn) | — |
| `3` | timed "show" (info screen); wait the show timer | → `g_disp_flags[4]` |
| `4` | timed "show" + power-level icon (`g_disp_req_0804c930`) | → `g_disp_flags[4]` |
| `5` | blank / off (`g_disp_req_0804da64`) | → `0` |
| `6` | **standby idle** (parked, speed < 10) — idle frame `0804cb1c` + corner LED; draws speed | fault → `0xc`; speed ≥ 10 → `7` |
| `7` | **riding** (speed ≥ 9) — `matrix_draw_number(km/h, 3)` on a ~1 s cadence + speed bar | speed < 9 → `6` |
| `8` | **riding + battery/charge** — battery/charge frames; draws speed | low-supply in range → `9`; low-word fault + blink → `0xc` |
| `9` | battery-low frame (`0804693c`) | supply-low bit clears → `8` |
| `0xa` | secondary/assist speed — `matrix_draw_speed((v/10)+9, …)` from `+0x3d2` | — |
| `0xd` | power-level icon — `matrix_draw_icon(+0x3c9)` | → `2` |
| `0xe` | turn/indicator icon — `matrix_draw_icon(+0x3ca)` | — |
| `0xf` | charging animation — toggles two frames on a `systick/10` blink | — |

### Fault / error display modes

Four modes render a fault. Two draw the **numeric error code** —
`matrix_draw_number(lowest_set_bit_index(low, high), 4)` over the error frame
`g_disp_req_0804c1d0`, i.e. *the lowest set bit index of the 64-bit fault pair*
(see [error-flags.md](error-flags.md) for the full 0..63 code map):

| Mode | Screen | Entered by / meaning |
| --- | --- | --- |
| `0xc` | **numeric fault — riding path.** Draws the error number, *unless* the only cause is bit 20 "supply low" (`0x100000`) with the pack voltage `+0x3f8` in `[0x6c4, 0xa46]`, in which case it shows the battery icon `0804693c` instead. Saves `g_disp_flags[8]`, then → `1` (restore). | modes `6`/`8` when a **low-word** bit (`0x3fffff`) is set and the blink phase `g_disp_flags[3]` is high |
| `0x24` | **numeric fault — standby path.** Same error frame + number, then → `2`. Shows **any** code 0..63, so high-word faults (no-SIM, wrong-SIM, horn/boost stuck, motor, …) surface here. | `status_process` when the **whole pair** is non-zero (`low \|\| high`) |
| `0x10` | **fixed error 60.** `matrix_draw_number(lowest_set_bit_index(0, 0x10000000), 4)` — a **hardcoded** code 60 (high-word bit 28) over the same error frame, then → `2`. The literal argument means the "60" is fixed by the mode, *not* read from the fault pair. | trigger not yet in the sourced set (no `set_mode_state_byte(0x10)` call site reconstructed) |
| `0x18` | **fixed error frame** (`g_disp_req_08048518`) — a *generic* error graphic with **no** number. Stays in mode `0x18`. | the **shifter / OTA codes 24..37** (`shifter_mode_command_dispatch`, `testmode_command_dispatch`) — these set the fault bit *and* `set_mode_state_byte(0x18)`, so the matrix shows this frame, not the numeric bit-index (the number is what BLE `0x5563` / the log report) |

`lowest_set_bit_index` (the 64-bit → code mapper) is the shared primitive for
modes `0xc`/`0x10`/`0x24`. `led_driver_set_shipping_mode` is invoked from mode `0xa`.

### Canned status / splash frames

Modes `0x11`..`0x17`, `0x19`..`0x23`, `0x25`..`0x29` each just push one fixed flash
request descriptor (lock / unlock / charging / boot / OTA / region splash screens)
and most fall back to mode `2`. They are selected by their respective subsystems;
the per-descriptor bitmaps live in `src/display_requests.c` (`g_disp_req_<addr>`).
Identified so far: **`0x22` = "Diag fail"** / **`0x23` = "Diag ok"** — the self-test
result screens set by `status_process` state `0x17` (see [error-flags.md](error-flags.md)).

## Request descriptors (`display_requests.c`)

The presenter and `matrix_draw_speed` pick *what* to animate by handing the render
pipeline a pointer to a **request descriptor** in flash rodata, via
`maybe_set_pending_request()` (→ `g_request_ctx+0x134`, the render source) or
`display_request_set()` (→ `+0x138`, the overlay source). `led_matrix_render_frame_region`
then walks it; `matrix_glyph_src_addr` / `matrix_glyph_frame_delay` index it.

A descriptor is a `uint32` array:

| word | field |
| --- | --- |
| `[0]` | origin column |
| `[1]` | width (columns) |
| `[2]` | frame count `F` |
| `[3 .. 3+F-1]` | per-frame delay (ticks, low 16 bits) |
| `[3+F ..]` | packed 3-bit/pixel bitmap — `F` frames × `width` columns |

Total length = `3 + F·(1 + width)` words. The OEM stores 38 of these (a few small
glyphs, several full-width multi-frame animations up to ~6 KB). They were
**materialized** as byte-faithful named C arrays in `src/display_requests.c`
(`g_disp_req_<oem-addr>`) — extracted from `mainware_1.07.06.bin` (image base
`0x08020000`), the length formula verified across the whole contiguous
`0x0804c104`→`0x0804da64` descriptor chain — replacing the bare magic addresses
the call sites used to pass. The data gc-sections out of the linked image, so the
build stays `text 1996`.

## Notes

- `led_matrix_transmit_step` was previously mislabelled `dsp_recovery_telemetry_pump`
  ("dsp" in the log string is **display**, not the C28x motor DSP) — corrected.
- ARM `char` is unsigned: the scheduler "no slot" sentinel `0xFA` (which Ghidra
  renders as a signed `== -6`) is compared as `== 0xFA` throughout.
