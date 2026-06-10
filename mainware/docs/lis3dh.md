# LIS3DH accelerometer driver (`lis3dh.c`)

The S3 main controller carries an **ST LIS3DH** 3-axis MEMS accelerometer on
the **I²C3** bus (the same bus the config EEPROM and the STC3115 gas-gauge sit
on). It is used purely as a **motion / tamper wake source**: when the bike is
locked, INT1 fires on movement and drives the alarm / wake logic in
`status_process`.

* I²C address: **8-bit `0x33`** (7-bit `0x19`, SA0 high).
* `WHO_AM_I` (reg `0x0F`) must read **`0x33`** — the probe in `lis3dh_accel_init`
  rejects anything else.
* All register sub-addresses are OR'd with **`0x80`** (the LIS3DH multi-byte
  auto-increment bit). HAL transfers are blocking with a **50 ms** timeout.

Sourced from the OEM functions at `0x0802E434..0x0802E782` (the register layer)
and `0x0803D070..0x0803D1D4` + `0x08029AB8` (transport + high-level), all
adversarially verified against the raw disassembly.

## Device model

The driver is transport-agnostic: a tiny per-device vtable is installed once by
`lis3dh_accel_init`, and every access dispatches through it.

```c
typedef struct lis3dh_dev {
    int (*write)(uint8_t reg, const uint8_t *buf, uint16_t len);  /* +0x00 */
    int (*read )(uint8_t reg, uint8_t *buf, uint16_t len);        /* +0x04 */
    int (*wait )(uint32_t timeout);                               /* +0x08 */
    uint8_t whoami;                                               /* +0x0C scratch */
} lis3dh_dev_t;
```

The single instance `g_lis3dh_dev` lives at SRAM **`0x2000838C`**. The three
slots are filled with the I²C transport leaves:

| Slot | OEM | Function | Behaviour |
| --- | --- | --- | --- |
| `+0x00` write | `0x0803D098` | `lis3dh_i2c_write` | `HAL_I2C_Mem_Write(&hi2c3, 0x33, reg\|0x80, 1, buf, len, 50)` |
| `+0x04` read  | `0x0803D074` | `lis3dh_i2c_read`  | `HAL_I2C_Mem_Read(&hi2c3, 0x33, reg\|0x80, 1, buf, len, 50)` |
| `+0x08` wait  | `0x0803D070` | `lis3dh_i2c_wait`  | `return 0` (HAL is blocking — nothing to poll) |

The three vtable thunks `lis3dh_read`/`lis3dh_wait`/`lis3dh_write`
(`0x0802E434/444/44E`) just forward to the installed pointer and propagate its
return value. Register helpers share one shape:

```
rc = read(reg); if (wait() != 0) return 3; if (rc == 0) { modify; rc = write(reg); } return rc;
```

return `0` = OK, `3` = bus wait failed.

## Register helper API

| OEM | Function | Register | Field |
| --- | --- | --- | --- |
| `0x0802E45E` | `lis3dh_set_power_mode` | CTRL_REG1[3]=LPen + CTRL_REG4[3]=HR | 0=high-res, 1=normal, 2=low-power |
| `0x0802E520` | `lis3dh_set_odr` | CTRL_REG1[7:4] | output data rate (0 = power-down) |
| `0x0802E566` | `lis3dh_set_filtered_data` | CTRL_REG2[3] | FDS |
| `0x0802E714` | `lis3dh_set_hpf_int` | CTRL_REG2[2:0] | HPCLICK/HPIS2/HPIS1 (note: read-fail falls through to write) |
| `0x0802E5AC` | `lis3dh_set_full_scale` | CTRL_REG4[5:4] | FS (0=±2g…3=±16g) |
| `0x0802E782` | `lis3dh_set_latch_int1` | CTRL_REG5[3] | LIR_INT1 |
| `0x0802E752` | `lis3dh_write_ctrl_reg3` | CTRL_REG3 | INT1 source routing (write-only) |
| `0x0802E760` | `lis3dh_read_ctrl_reg3` | CTRL_REG3 | read-back |
| `0x0802E5F2` | `lis3dh_read_reference` | REFERENCE (0x26) | read (clears the HP filter) |
| `0x0802E614` | `lis3dh_read_whoami` | WHO_AM_I (0x0F) | read |
| `0x0802E636` | `lis3dh_write_int1_cfg` | INT1_CFG (0x30) | write-only |
| `0x0802E644` | `lis3dh_read_int1_cfg` | INT1_CFG (0x30) | read-back |
| `0x0802E666` | `lis3dh_read_int1_src` | INT1_SRC (0x31) | read (clears the latch) |
| `0x0802E688` | `lis3dh_set_int1_threshold` | INT1_THS[6:0] | threshold |
| `0x0802E6CE` | `lis3dh_set_int1_duration` | INT1_DURATION[6:0] | duration |

## Public API

| OEM | Function | Notes |
| --- | --- | --- |
| `0x0803D0BC` | `lis3dh_accel_init` | install vtable, **`NVIC_DisableIRQ(0x48/0x49)`**, verify WHO_AM_I. 0=ok / 1=read-fail / 2=wait-fail / 3=wrong-id |
| `0x0803D120` | `lis3dh_config_motion_int(mode, threshold)` | full motion-wake INT1 setup (below) |
| `0x0803D110` | `lis3dh_powerdown` | `lis3dh_set_odr(dev, 0)` |
| `0x08029AB8` | `lis3dh_int1_clear` | drain latched INT1 (≤99 retries, watchdog-kicked) |
| `0x0803D1D4` | `lis3dh_int1_read_source` | read INT1_SRC; log `" ERR LIS1"` on bus error; return the byte |

`NVIC_DisableIRQ` is the CMSIS `__NVIC_DisableIRQ` at `0x080270FC` (writes
`NVIC->ICER`, base `0xE000E100` + word `0x20`). `accel_init` **masks** the two
EXTI lines wired to INT1/INT2 (IRQ `0x48`/`0x49`) while it probes + configures.

### Motion-wake configuration (`lis3dh_config_motion_int`)

`status_process` calls this with `(0, 6)` for high sensitivity and `(0, 0x20)`
for low sensitivity. Sequence:

1. `set_hpf_int(1)` + `set_filtered_data(1)` — route the event through the HPF.
2. read CTRL_REG3, set `I1_IA1` (bit 6), write back — route IA1 to the INT1 pad.
3. `set_latch_int1(0)`, `set_full_scale(0)` (±2 g).
4. `set_int1_threshold(threshold)`, `set_int1_duration(0)`.
5. read REFERENCE (clears the HPF state).
6. read INT1_CFG, then: `mode==0` → `|= 0x2A` (XHIE|YHIE|ZHIE, OR-of-events);
   `mode!=0` → `(cfg & 0xD7) | 0x02` (X high event only). Then unconditionally
   clear bit 7 (AOI) — i.e. `&= 0x7F`. Write INT1_CFG.
7. `set_power_mode(0)` (high-res), `set_odr(5)` (100 Hz).

### INT1 servicing (`lis3dh_int1_clear`)

Loops up to 99 times while the INT1 GPIO (**GPIOC**, IDR mask `0x08`) stays
asserted: kick the watchdog, read INT1_SRC via `lis3dh_int1_read_source`, log
`"Clear Lis 0x%02X"` whenever the source byte changes (cached at
`g_lis3dh_int1_last_src`, SRAM `0x200001E0`), and `systick_delay(10)`. On
exhausting the budget it logs `"Err Clear Lis"`.

## Addresses

| Address | Symbol | Meaning |
| --- | --- | --- |
| `0x2000838C` (SRAM) | `g_lis3dh_dev` | device handle (vtable + WHO_AM_I scratch) |
| `0x200001E0` (SRAM) | `g_lis3dh_int1_last_src` | last INT1_SRC value (re-log on change) |
| `0x20009B04` (SRAM) | `hi2c3` | shared I²C3 HAL handle (EEPROM/STC3115 too) |
| `0x08052F2C` (flash) | — | `" ERR LIS1\r\n"` |
| `0x0804FED8` (flash) | — | `"Clear Lis 0x%02X\r\n"` |
| `0x0804FEEC` (flash) | — | `"Err Clear Lis\r\n"` |

> `charge_time_estimate_reset` (`0x0802E40C`) sits adjacent in flash but is
> **not** part of this driver — it resets the battery charge-time estimate and
> re-arms the BMS poll. Sourced into `states.c`; see `hardware.md`.
