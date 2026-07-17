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
- **LED-matrix fault display** — the matrix draws an error frame (`g_disp_req_0804C1D0`)
  with a decimal **error number = the lowest set bit index (0..63)** across the pair
  (`matrix_draw_number(lowest_set_bit_index(low, high), 4)`; low word → codes 0..31,
  high word → codes 32..63). It appears in two `display_mode_sm_step` contexts:
  **state `0xc`** — the *riding* fault display, entered on a **low-word** bit in
  `0x3FFFFF` (codes 0..21; code 20 "supply low" shows the battery icon instead while
  the pack voltage is in range); and **state `0x24`** — the *at-rest / standby* error
  screen, which `status_process` enters whenever the **whole pair is non-zero**
  (`low || high`) plus via the shifter (codes 24..27) and testmode paths. State `0x24`
  shows **any** code 0..63 — so the high-word faults (no-SIM, wrong-SIM, horn/boost
  stuck, motor, …) render here too. When several faults are set the **lowest** bit
  wins. The `status_process` `"Error Flags: %d"` log uses the same index, and the
  `"Possible error 21"` log text is itself that matrix code (low bit 21). The
  **"Matrix"** column in the tables below is each flag's shown code. **Exception:**
  the shifter/OTA codes 24..37 (marked `‡`) are set together with display mode `0x18`,
  which shows a *fixed* error frame (`g_disp_req_08048518`) rather than the numeric
  bit-index — see the note under the high-word table.

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

| Bit | Mask | Matrix | Meaning | Set / cleared by |
| --- | --- | --- | --- | --- |
| 0..15 | `0x0000FFFF` | 0..15 | **BMS fault register** (mirrored from `ctx+0x3F6`); `0x1000`/`0x2000` masked while charging | `battery_fault_warning_report` |
| 16 | `0x00010000` | **16** | **BMS unreachable** — the fault register reads `0xFFFF` (bus dead / all-faults) | `battery_fault_warning_report` |
| 17 | `0x00020000` | **17** | **BMS charge/discharge FET no-response** (`"BMS: No CHG"` / `"BMS: No DSG"`) | `battery_telemetry_step` |
| 18 | `0x00040000` | **18** | **BMS not detected** — PC10 present-pin timeout (cleared on `"BMS detected"`) | `battery_telemetry_step` |
| 19 | `0x00080000` | **19** | **BMS Modbus-bus error** — reset / no-ID (`"ERR MBB reset"`, `"BMS: no ID"`) | `battery_telemetry_step` |
| 20 | `0x00100000` | **20** *(battery icon)* | **Supply voltage low** — `ctx+0x3B0` < 25000 mV during battery bring-up | `status_process` |
| 21 | `0x00200000` | **21** | **"Possible error 21"** — `ctx+0x3FE` reads < 0xB persistently (checked twice, 10 s apart) | `status_process` |
| 22 | `0x00400000` | **22** | **SSP/BLE TX backlog overflow** — >4 committed inter-module frames unacknowledged | `sspm_tx_queue_pump`; cleared when `sspm_rx_reply_handler` drains (`main`) |
| 23 | `0x00800000` | **23** | **BLE coprocessor reboot** in progress (`"Reboot BLE"`, drives the CC2642 reset) | `ssp`; cleared when `ble_ssp_dispatch` recovers (`main`) |
| 24 | `0x01000000` | 24 ‡ | **eShifter error A** — `shifter_mode_command_dispatch(4/7)` (SSP/BLE-driven, `ssp.c`); *overwrites* the whole pair and enters display mode `0x18` | `shifter_mode_command_dispatch` |
| 25 | `0x02000000` | 25 ‡ | **eShifter error B** — cmd 5 | `shifter_mode_command_dispatch` |
| 27 | `0x08000000` | 27 ‡ | **eShifter error C** — cmd 6 | `shifter_mode_command_dispatch` |
| 26, 28-31 | `0x04000000`, `0x10000000`..`0x80000000` | 26, 28-31 ‡ | **OTA / status error codes** via `testmode_command_dispatch(cmd)`: cmd 12→26 (`status_process`), cmd 2→28 (OTA, `update.c`), cmd 3/4/5→29/30/31 | `testmode_command_dispatch` |

---

## High word — `session_ctx + 0x3BC`

| Bit | Mask | Matrix | Meaning | Set / cleared by |
| --- | --- | --- | --- | --- |
| 0-5 | `0x01`..`0x20` | 32-37 ‡ | **OTA / shifter error codes** via `testmode_command_dispatch(cmd)`: cmd 6..11 → codes 32..37 (e.g. cmd 10→36, `shifter.c`) | `testmode_command_dispatch` |
| 6 | `0x00000040` | 38 | **Internal-LiPo gauge (STC3115) read failure** — ≥5 consecutive `stc_read` errors (`" ERR Read STC"`) | `status_process` |
| 7 | `0x00000080` | 39 | **Ambient light sensor (CM2323) fault** — >4 consecutive I2C fails (`" ERR CM2323"`) | `light_sensor_read_step` |
| 8 | `0x00000100` | 40 | **Horn/bell button (PC0) stuck** — held pressed > 20 s | `charger_and_pc1_sense_debounce` |
| 9 | `0x00000200` | 41 | **Boost button (PC1) stuck** — held pressed > 20 s | `charger_and_pc1_sense_debounce` |
| 10 | `0x00000400` | 42 | **Fb-coil / PB3 sense** — set while PA11 ("Fb coil det") **and** PB3 read high; cleared when both low | `status_process` |
| 12 | `0x00001000` | 44 | **eShifter Modbus comm error** — 5 consecutive shifter-bus failures | `modbus_shifter_link_monitor` |
| 13 | `0x00002000` | 45 | **Motor error** — motor status word `ctx+0x364` has bit `0x4` set (`"Motor error %04X"`) | `status_process` |
| 15 | `0x00008000` | 47 | **BMS reports no SOC** — `ctx+0x3FC` == -1 (`"BMS: No SOC"`) | `battery_telemetry_step` |
| 16 | `0x00010000` | 48 | **Wheel moving** — *status bit, not a fault* (the wheel-speed capture "in motion" flag) | `exti9_5_app_hook` / `tim7` |
| 21 | `0x00200000` | 53 | **Motor critical fault** — `ctx+0x364` bits `0x2100` all set | `status_process` |
| 22 | `0x00400000` | **54** | **No SIM present** — modem SIM-detect pin (PE10) low at power-on | `modem_sim_state_machine` |
| 23 | `0x00800000` | 55 | **Boot device init failure** — >2 on-board I2C devices failed to init (LIS3DH/MAX9768/…, `"i2c bus error"`) | boot init (`main`) |
| 24 | `0x01000000` | **56** | **Wrong ICCID / SIM** — the SIM's ICCID doesn't match the VanMoof Vodafone-NL prefix `"8931440400"` (`"Wrong iccid, sim/no sim"`) | `sim_iccid_check` |
| 25 | `0x02000000` | **57** | **Cannot read SIM ICCID** (`"Cannot read SIM"`) | `modem_sim_state_machine` |
| 26 | `0x04000000` | **58** | **GSM/modem power fail** (`"GSM power fail"`) | `modem_sim_state_machine` |

Bits not listed are unused by the reconstructed sources.

**‡ codes 24..37** — unlike the operational faults, these are written together with
`set_mode_state_byte(0x18)`, and display **mode `0x18`** shows a **fixed error frame**
(`g_disp_req_08048518`), *not* the numeric bit-index. So on the matrix they appear as
that generic error frame; the number in the "Matrix" column is the code reported over
BLE `0x5563` and the `"Error Flags: %d"` log (and is drawn as a matrix number only if
the fault persists into the standby numeric display, mode `0x24`). They are set by
`shifter_mode_command_dispatch` (codes 24/25/27) and `testmode_command_dispatch`
(codes 26, 28..37) — despite the latter's name it is invoked from the OTA/update
(`update.c`), status (`status_process`) and shifter (`shifter.c`) paths, not only
factory test.

**Alias note:** the modem sets/tests these via `async_request_post(low, high)` and
`async_request_poll(low, high, …)`, which are just mis-named aliases of
`state_flags_set` (`0x0802A268`) / `state_flags_test` (`0x0802A28C`) — e.g.
`sim_iccid_check` calls `async_request_post(0, 0x1000000)` (= bit 24), and
`modem_registration_get` polls `async_request_poll(0, 0x7400000, …)` = the three
modem-fault bits (`0x400000|0x2000000|0x4000000`) at once. Note that several mask
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
