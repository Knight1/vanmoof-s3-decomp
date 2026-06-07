# mainware — lamp engine + ambient light sensor (`lighting.c`)

`src/lighting.c` drives the bike's **front/rear lamps** and reads the **ambient
light sensor**. The lamps are three PWM brightness channels with a fade engine
and table-driven flash patterns; the sensor (over I2C2) provides the auto on/off
input. **Sourced** as faithful, behaviour-equivalent C (8 functions), fan-out
transcribed and **adversarially verified** against the live disassembly.

> Note: the super-loop function previously named `light_tick_update`
> (`0x080371E8`) is **not** part of this module — it is a console↔shifter/battery
> **UART passthrough bridge** (it runs the VT100 line editor and bridges firmware
> flashing), and belongs to the console/bridge layer.

## Lamp PWM channels (`g_lights` @ 0x20006DC0)

Three channels at a `0x14`-byte stride, each driving a TIM compare output:

| channel | base | brightness callback | PWM |
| --- | --- | --- | --- |
| 0 | `+0x00` | `obj_set_field34` | TIM CCR1 (`*0x20009A84 + 0x34`) |
| 1 | `+0x14` | `obj_set_field38` | TIM CCR2 (`+0x38`) |
| 2 | `+0x28` | `led_channel3_set_brightness` | TIM CCR3 (`+0x3C`) |

Per-channel layout (relative to the channel base): `+0x00` brightness callback
fn-ptr, `+0x04` pattern selector (0..10 = "load table N", `0x0E` = run), `+0x05`
latched request, `+0x06` step index, `+0x07` step-timer slot, `+0x08` active
step-table ptr, `+0x0C` target brightness, `+0x0D` current brightness, `+0x0E`
fade-timer slot, `+0x10` fade tick interval. `g_lights+0x3D` is the one-shot init
flag for the whole object; `g_lights+0x3C` is the sensor fault counter.

## `light_pattern_step(trigger, channel, threshold, mode)`

Called three times per super-loop iteration (channel 0/1/2). It:

1. One-shot-inits all three channels (allocates scheduler slots, wires the PWM
   callbacks).
2. Latches the incoming request byte (`*trigger`, consumed → 0).
3. For requests 1 / 0x0B / 3, recomputes the **target brightness** from `mode`
   (1 = force on → 100, 2 = force off → 0, 0 = auto: ambient lux `<` `threshold`
   → 100 else 0), then clamps by power state (`power_state_get_clamped()`: 2 →
   `0x32`, 1 → 0).
4. **Fade engine**: when the fade slot is idle, single-steps current brightness
   (`+0x0D`) toward the target (`+0x0C`) by ±1 and pushes the new value through
   the channel's PWM callback.
5. **Pattern runner** (`+0x04` switch): cases 1..10 load one of ten flash-pattern
   step-tables and enter the runner (`0x0E`); the runner walks the table
   (`action 8` = end) calling `light_pattern_action_apply` per step with the
   step interval scheduled.

`light_pattern_action_apply(action, level, ch)` applies one pattern step (0..6):
set brightness 100/0x32/0 immediately via the callback, or schedule a fade
(intervals `0x1E` / 6 ticks).

## Ambient light sensor (I2C2)

- `light_sensor_read_step()` — poll-throttled (`0x5DC`-tick scheduler slot, timer
  name `"lux_tmr"`). On idle (and once warmed up) it reads the sensor, caches the
  16-bit lux at the sensor object (`0x20000090 + 2`), and manages the fault
  counter at `g_lights+0x3C`: on `>4` consecutive failures it sets the fault flag
  (`state_flags_set(0,0x80)`) and logs `" ERR CM2323\r\n"`. Returns the cached lux.
- `light_sensor_i2c_read(out)` — one transaction over I2C2 (device `0x20`): write
  reg `0x50`, then read 2 bytes; composes the little-endian reading into `*out`.
  Returns the HAL status, or 2 if the bus is busy.
- `light_sensor_fault_count_get()` — read the sensor retry/fault counter byte.

## Hardware

| signal | detail |
| --- | --- |
| Lamp PWM | TIM CCR1/CCR2/CCR3 (TIM base via the PWM object @ `0x20009A84`; `tim1_pwm_init`) |
| Light sensor | I2C2 device `0x20`, reg `0x50` (handle struct `0x20009BB8`) |

SRAM: `g_lights` @ `0x20006DC0` (3 channels), light-sensor object @ `0x20000090`,
sensor fault counter at `g_lights+0x3C`.
