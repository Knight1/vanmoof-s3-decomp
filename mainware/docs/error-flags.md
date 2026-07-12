# Error / fault flags

The bike's fault state is a **64-bit pair** in the session context (`session_ctx`
@ `0x200083A8`):

| Field | Addr | Role |
| --- | --- | --- |
| low word  | `session_ctx + 0x3B8` | battery/BLE faults + the mirrored BMS fault register |
| high word | `session_ctx + 0x3BC` | mainware system faults |

## Accessors

The pair is manipulated only through three helpers (`app.c`, via the cached ctx
pointer at `0x20000944`):

```c
void state_flags_set  (uint32_t low, uint32_t high);  /* 0x0802A268: +0x3B8|=low, +0x3BC|=high */
void state_flags_clear(uint32_t low, uint32_t high);  /* 0x0802A240: +0x3B8&=~low,+0x3BC&=~high */
int  state_flags_test (uint32_t low, uint32_t high);  /* 0x0802A28C: (+0x3BC&high)||(+0x3B8&low) */
```

`state_flags_test` is an **OR across both words** — a nonzero return means at least
one of the requested bits is set in either word.

## Where they surface

- **Console `show`** — `console_cmd_show` prints `Error Flags: 0x<HIGH> <LOW>`
  (`+0x3BC` first, then `+0x3B8`).
- **`status_process`** also rotates the word bit-by-bit and logs each set bit's
  index (`"Error Flags: %d"`), or `"Error Flags: None"` when clear.
- **BLE read `0x5563`** returns the 8 bytes at `+0x3B8` (both words).
- **Diagnostics self-test** (`diagnostics_run_step`) loads the pair as one 64-bit
  value: `(low | high) == 0` → `"Diag ok"`, else `"Diag fail"` (+ raises error
  state `0x22`).
- **LED-matrix fault display** (`display_mode_sm_step` mode 8, gated on any low-word
  bit in `0x3FFFFF`): shows the user-facing **error number = the lowest set bit index
  (0..63)** across the pair — low word → codes 0..31, high word → codes 32..63
  (`lowest_set_bit_index(low, high)`). So e.g. low bit 24 shows as "error 24", high
  bit 8 (horn stuck) as "error 40". The `status_process` `"Error Flags: %d"` log uses
  the same index.

---

## Low word — `session_ctx + 0x3B8`

Bits **0..15 mirror the BMS's own 16-bit fault register** (`battery_fault_warning_report`,
`0x08039EB4`): each super-loop it copies `ctx+0x3F6` (the value read back from the
batteryware BMS) into these bits — `0xFFFF` → set bit 16 instead (see below) and
clear the mirror; `0` → clear the mirror; else `|=` the raw fault bits. Bits
`0x1000`/`0x2000` of this range are auto-masked while the charger is present (PC4),
i.e. charge/discharge-specific BMS faults are suppressed on the charger. For the
individual BMS fault-bit meanings (OV / UV / OT / OCP / SCP / …) see the
**batteryware** BMS decomp — this word is a verbatim copy of that register.

Bits **16..23** are mainware-level battery / BLE faults:

| Bit | Mask | Meaning | Set / cleared by |
| --- | --- | --- | --- |
| 0..15 | `0x0000FFFF` | **BMS fault register** (mirrored from `ctx+0x3F6`); `0x1000`/`0x2000` masked while charging | `battery_fault_warning_report` |
| 16 | `0x00010000` | **BMS unreachable** — the fault register reads `0xFFFF` (bus dead / all-faults) | `battery_fault_warning_report` |
| 17 | `0x00020000` | **BMS charge/discharge FET no-response** (`"BMS: No CHG"` / `"BMS: No DSG"`) | `battery_telemetry_step` |
| 18 | `0x00040000` | **BMS not detected** — PC10 present-pin timeout (cleared on `"BMS detected"`) | `battery_telemetry_step` |
| 19 | `0x00080000` | **BMS Modbus-bus error** — reset / no-ID (`"ERR MBB reset"`, `"BMS: no ID"`) | `battery_telemetry_step` |
| 20 | `0x00100000` | **Supply voltage low** — `ctx+0x3B0` < 25000 mV during battery bring-up | `status_process` |
| 21 | `0x00200000` | **"Possible error 21"** — `ctx+0x3FE` reads < 0xB persistently (checked twice, 10 s apart) | `status_process` |
| 22 | `0x00400000` | **SSP/BLE TX backlog overflow** — >4 committed inter-module frames unacknowledged | `sspm_tx_queue_pump`; cleared when `sspm_rx_reply_handler` drains (`main`) |
| 23 | `0x00800000` | **BLE coprocessor reboot** in progress (`"Reboot BLE"`, drives the CC2642 reset) | `ssp`; cleared when `ble_ssp_dispatch` recovers (`main`) |
| 24 | `0x01000000` | **eShifter fault A** — mode-cmd 4/7 (overwrites the pair, enters fault-display mode `0x18`) | `shifter_mode_command_dispatch` |
| 25 | `0x02000000` | **eShifter fault B** — mode-cmd 5 | `shifter_mode_command_dispatch` |
| 27 | `0x08000000` | **eShifter fault C** — mode-cmd 6 | `shifter_mode_command_dispatch` |
| 26, 28-31 | `0x04000000`, `0x10000000`..`0x80000000` | **testmode-injected** fault codes (diagnostic self-test only) | `testmode_command_dispatch` |

---

## High word — `session_ctx + 0x3BC`

| Bit | Mask | Meaning | Set / cleared by |
| --- | --- | --- | --- |
| 0-5 | `0x01`..`0x20` | **testmode-injected** fault codes (diagnostic self-test only) | `testmode_command_dispatch` |
| 6 | `0x00000040` | **Internal-LiPo gauge (STC3115) read failure** — ≥5 consecutive `stc_read` errors (`" ERR Read STC"`) | `status_process` |
| 7 | `0x00000080` | **Ambient light sensor (CM2323) fault** — >4 consecutive I2C fails (`" ERR CM2323"`) | `light_sensor_read_step` |
| 8 | `0x00000100` | **Horn/bell button (PC0) stuck** — held pressed > 20 s | `charger_and_pc1_sense_debounce` |
| 9 | `0x00000200` | **Boost button (PC1) stuck** — held pressed > 20 s | `charger_and_pc1_sense_debounce` |
| 10 | `0x00000400` | **Fb-coil / PB3 sense** — set while PA11 ("Fb coil det") **and** PB3 read high; cleared when both low | `status_process` |
| 12 | `0x00001000` | **eShifter Modbus comm error** — 5 consecutive shifter-bus failures | `modbus_shifter_link_monitor` |
| 13 | `0x00002000` | **Motor error** — motor status word `ctx+0x364` has bit `0x4` set (`"Motor error %04X"`) | `status_process` |
| 15 | `0x00008000` | **BMS reports no SOC** — `ctx+0x3FC` == -1 (`"BMS: No SOC"`) | `battery_telemetry_step` |
| 16 | `0x00010000` | **Wheel moving** — *status bit, not a fault* (the wheel-speed capture "in motion" flag) | `exti9_5_app_hook` / `tim7` |
| 21 | `0x00200000` | **Motor critical fault** — `ctx+0x364` bits `0x2100` all set | `status_process` |
| 22 | `0x00400000` | **No SIM present** — modem SIM-detect pin (PE10) low at power-on | `modem_sim_state_machine` |
| 23 | `0x00800000` | **Boot device init failure** — >2 on-board I2C devices failed to init (LIS3DH/MAX9768/…, `"i2c bus error"`) | boot init (`main`) |
| 25 | `0x02000000` | **Cannot read SIM ICCID** (`"Cannot read SIM"`) | `modem_sim_state_machine` |
| 26 | `0x04000000` | **GSM/modem power fail** (`"GSM power fail"`) | `modem_sim_state_machine` |

Bits not listed are unused by the reconstructed sources. Note that several mask
*values* recur across the two words with unrelated meanings (e.g. `0x400000` =
"SSP backlog" in the low word but "no SIM" in the high word; `0x800000` = "BLE
reboot" low vs "boot I2C fail" high) — the word (arg position to `state_flags_*`)
disambiguates them.

## Related raw registers (not part of this pair)

- `ctx+0x3F6` — the **BMS fault** register (read from batteryware); mirrored into
  `+0x3B8[0:15]`, and logged on change as `"BMS fault 0x%04X"`.
- `ctx+0x442` — the **BMS warning** register; logged on change as `"BMS warning 0x%04X"`.
- `ctx+0x364` — the **motor** status/error word (from motorware); drives high-word
  bits `0x2000` and `0x200000`, logged as `"Motor error %04X"`.
- `ctx+0x3A4..0x3AB` — the 8-byte **motor error-flags** block exposed on BLE `0x5548`.
