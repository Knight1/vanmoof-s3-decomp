# Data EEPROM layout & provisioning — batteryware

How the BMS persists state, the full EEPROM map, and what must be written
to bring a blank pack up. Companion to `hardware.md`, `fedl5236.md`,
`memory-map.md`.

**Tooling** (`../tools/`):
- `eeprom_decode.py <dump.bin>` — decode a 6 KB EEPROM dump
  (`0x08080000`, len `0x1800`) into all documented fields. Refuses a
  main-flash image (the EEPROM is a *separate* region — read `0x08080000`
  len `0x1800`, e.g. `st-flash read eeprom.bin 0x08080000 0x1800`).
- `eeprom_example.py [--version 1.14.1|1.17.1] [--capacity MAH] [--esn …]
  [--date YYYYMMDD] [--boot-flag HEX] [--chg-cal PERMILLE] [--dsg-cal PERMILLE]
  [--unprovisioned]` — generate a **complete** known-good EEPROM covering both
  firmwares' fields: the bmsboot boot flag at `0x08080000` (`0x55` = normal
  boot) and the reset-cause word, plus all batteryware fields (boot mode,
  validated params, FW-magic, ESN/date, config block including the
  charge/discharge current-cal gains — see §5). Set `--version` to match the
  app on the chip (the FW-magic must agree). `--chg-cal`/`--dsg-cal` restore
  saved current-cal gains (default `1000` = no correction). `--unprovisioned`
  leaves the gauge's capacity flag `0` so the firmware factory-inits on first
  boot. `build_eeprom()` is importable. Field offsets are from the 1.17.1
  decomp; confirm against other versions.
- `bms_build_image.py BMSBOOT.bin BATTERYWARE.bin [--gen-eeprom |
  --eeprom FILE]` — assemble a flashable chip image: bmsboot at
  `0x08000000`, batteryware at `0x08005000` (the AP bank), and an optional
  6 KB EEPROM at `0x08080000`. Validates each input (sizes, header magic,
  image-header / bootloader-trailer CRCs, AP-bank fit) and emits a combined
  Intel HEX plus raw per-region bins, with ready-to-run OpenOCD /
  STM32CubeProgrammer / st-flash commands. `--gen-eeprom` folds in a default
  EEPROM with the FW-magic auto-matched to the supplied app.

## 1. What the "EEPROM" actually is

All `0x08080xxx` addresses are the **STM32L072 on-chip data EEPROM**
(base `0x08080000`, nominally 6 KB → `0x080817FF`). It is *not* an
external chip and *not* main flash (the app is linked at `0x08005000`
and the part's program flash ends at `0x08030000`).

- **Reads** are plain memory loads — `memcpy_oem(src, n, dst)` for blocks,
  or direct `*(volatile T *)0x08080xxx`.
- **Writes** go through `spi_register_write(type, addr, val)`
  (`spi.c`, OEM `FUN_0800f6f0`): `type` 0/1/2 = byte/halfword/word, a
  direct store to the EEPROM cell, bracketed by `dma_wait_for_ready()`
  which polls the FLASH controller `BSY` flag (`0x40022018`). This is the
  STM32L0 data-EEPROM programming path, *not* SPI to a peripheral.
- **Verified writes** use `memcmp_verify(actual, len, expected)`
  (`spi.c`, OEM `FUN_080093a6`): commits `expected[i]` → `&actual[i]`
  byte-by-byte and re-reads until it sticks. The arg that is an
  `0x08080xxx` address is the EEPROM side, so:
  - `memcmp_verify(0x08080xxx, n, RAM)` → **persist RAM → EEPROM**.
  - `memcmp_verify(RAM, n, 0x08080xxx)` → **reload EEPROM → RAM**.

> The `spi_*` / `smbus_*` naming is decomp legacy. `smbus_*` = SPI to the
> ML5236 AFE (see `fedl5236.md`); `spi_register_write` / `memcmp_verify`
> = on-chip EEPROM. They are unrelated buses.

## 2. Full EEPROM map (everything the firmware touches)

### Low area (`0x08080000`–`0x0808003x`) — boot, calibration, identity

| Offset | Size | Field | Meaning | Set by / read by |
| --- | --- | --- | --- | --- |
| `0x08080001` | 1 | **boot mode** | Power-on dispatch selector. `0x17`/`0x18` → boot straight into the MOS-failure protection handler; `0x17` also sets `g_fault_flags` bit 6. `7/8/9/10` → power-on OVP/UVP re-entry check. `0`/other → normal boot. | read in `main.c:187` |
| `0x08080006` | 2 | param `s_ctx[+2]` | Validated config field; fallback `0x0310` if 0 or FW-magic mismatch. | `config_init` |
| `0x0808000F`–`0x0808001C` | 14 | **ESN** (electronic serial number) | 14-byte ASCII pack serial. Written via Modbus write-multiple (func `0x10`) to holding registers `0x0C`–`0x12`; read back via func `0x03` regs `0x0C`–`0x12`. | `flash.c` (write), `uart.c:469-475` (read) |
| `0x0808001D`–`0x08080020` | 4 | **manufacture date** | 4 bytes `[0x00, year, month, day]` across registers `0x13`–`0x14`. | `flash.c` (write), `uart.c:476-477` (read) |
| `0x08080021` | 4 | anti-replay `t_now` | Tick value latched the last time ESN/date were written. | `flash.c` |
| `0x08080025` | 4 | anti-replay `t_ms` | "" (ms tick). | `flash.c` |
| `0x08080029` | 4 | anti-replay `t_to` | "" (timeout tick). The ESN/date write only commits when the live tick triplet differs from this stored triplet (replay guard); on success the new triplet is persisted. | `flash.c` |
| `0x0808002E` | 4 | **FW magic** | Must equal `0x011701B1` ("1.17.1 BMS"). On mismatch, `config_init` reverts every EEPROM-validated field to its fallback default and re-persists the magic. | `config_init` |
| `0x08080032` | 2 | param `s_ctx[+14]` | Validated; fallback `0x0D01`. | `config_init` |
| `0x08080034` | 1 (rd 2) | param `s_ctx[+16]` | Validated (low byte); fallback `7`. | `config_init` |
| `0x08080036` | 2 | param `s_ctx[+18]` | Validated; fallback `0x0C4E`. | `config_init` |

### Config block (`0x08080C00`, 128 B) — the live BMS state

Loaded verbatim into RAM `cfg @ 0x200029A8` at boot (`bms_setup`), and
persisted back wholesale on factory init. Known fields (offset within the
block):

| Off | Size | Field | Meaning |
| --- | --- | --- | --- |
| `+0x24` | 4 | **stored SOC** | State-of-charge accumulator (coulomb count). Reloaded by `config_resend_all` (`0x08080C24`). |
| `+0x28` | 4 | **full-charge capacity** (mAh) | Modbus read reg `0x16`. Set to `cfg2_b[+6]` at factory init; **`0` doubles as the "unprovisioned" flag** → `bms_setup` runs factory init when this is zero. |
| `+0x2C` | 4 | **remaining capacity** (mAh) | Modbus read reg `0x17`. Intra-cycle charge accumulator (`SOC % 0x3840`). Reloaded from `0x08080C2C`. |
| `+0x34` | 2 | **cycle count** | Charge-cycle counter. Modbus read reg `0x19` (`uart.c:482` `RD16(0x200029A8+0x34)`). Persisted at `0x08080C34`. |
| `+0x36` | 1 | **RSOC %** | Relative state-of-charge percent. Modbus read reg `0x05`. Reloaded from `0x08080C36`. |
| `+0x37` | 1 | **absolute SOC %** | Modbus read reg `0x18` (`uart.c:481`). Set to `100` at factory init. |
| `+0x3A` | 2 | **charge current-cal gain** | Per-mille multiplier the fuel gauge applies to measured charge current (`I = I_raw · gain/1000`); `0x3E8` (1000) = ×1.000. Loaded into `g_chg_cal_gain @0x200025B2`; set by `CHG CAL`. Clamped to `(0x384, 0x44B]` (901–1099); `bms_setup` resets **both** gains to 1000 if either is out of range. (Not a voltage threshold — see §5.) |
| `+0x3C` | 2 | **discharge current-cal gain** | Same, for discharge current (`g_dsg_cal_gain @0x200025B0`); set by `DSG CAL`. Same clamp/default. |
| `+0x40` | 2 | (factory-zeroed) | Persisted to `0x08080C40`. |
| `+0x42` | 2 | (factory-zeroed) | Persisted to `0x08080C42`. |
| `+0x44/+0x45/+0x46` | 1 each | per-phase voltage markers | Default `'A'` (`0x41`); updated by `cell_balance_update` after 5 consecutive over-threshold readings, persisted to `0x08080C44/45/46`. |

> **Ambiguity flagged:** `main.c:219` reads `(uint16_t)(cfg_blk+0x46)` as a
> UVP2 threshold, but `cfg_blk` there is `0x200028D0` (`s_ctx`), a
> *different* RAM block than `cfg @ 0x200029A8`. The shared EEPROM cell
> `0x08080C46` feeds both `s_ctx[+70]` (config_init) and `cfg[+0x46]`
> (cell-balance). Whether these are intentionally the same datum needs
> confirmation when those handlers are verified.

### Secondary config (`0x08080C80`, 56 B)

Loaded into RAM `cfg2 @ 0x20002AD0`; zeroed and re-persisted at factory
init. Field meanings not yet individually decoded.

### Event / telemetry history log (ring buffers)

`bms_set_state` appends a **0x38-byte record** per state transition,
indexed by a sequence number, into two regions:
`0x08080200 + idx·0x38` (low half) and `0x08080E00 + (idx−0x32)·0x38`
(high half). `modem.c` dumps both as hex over the service UART.

> **Caveat:** the reconstructed `2 × 50 × 0x38` layout overlaps the config
> block at `0x08080C00` and runs past the nominal 6 KB EEPROM. The exact
> capacity / record size / index split is **not yet verified** — treat the
> ring-buffer extent as approximate.

## 3. ESN (serial number) & manufacture date

> Corrects an earlier draft of this doc that claimed the BMS stores no
> serial number and no cycle count. Both exist. The `0x0808000F`–`0x08080020`
> region — previously mislabeled "calibration pairs" — is the **ESN +
> manufacture date**, and the **cycle count** lives in the config block at
> `+0x34` (see §2).

The service tool provisions identity over Modbus **write-multiple
registers (function `0x10`)**, handled by `flash.c::flash_stream_handler`.
The register-to-EEPROM mapping (one 16-bit register = one 2-byte EEPROM
cell, in order) is:

| Modbus reg | Bytes | EEPROM | Holds |
| --- | --- | --- | --- |
| `0x0C`–`0x12` | 14 | `0x0808000F`–`0x0808001C` | **ESN** — 14 ASCII chars |
| `0x13` | 2 | `0x0808001D`–`0x0808001E` | date bytes `[0x00, year]` |
| `0x14` | 2 | `0x0808001F`–`0x08080020` | date bytes `[month, day]` |

So the 4-byte manufacture date is stored as **`[0x00, year, month, day]`**
(the host supplies it as `YYYYMMDD` and packs it into those two registers).

Write rules (firmware side):

- The whole `0x0C`–`0x14` span is written in **one** `0x10` frame; the
  handler walks command words and admits successive register/EEPROM pairs.
- The write is **anti-replay gated**: it only commits when the live tick
  triplet differs from the stored reference at `0x08080021/25/29`, and on
  completion it persists the new triplet. Re-sending the *identical* frame
  in the same tick window is ignored.
- Each cell is committed with `memcmp_verify` (write-and-read-back-until-
  stable), so a failed EEPROM write retries rather than silently dropping.
- **Byte order caveat:** Modbus registers are big-endian on the wire but
  each is stored as a little-endian 16-bit cell, so consecutive ESN ASCII
  bytes are **swapped within each register pair** in raw EEPROM
  (`uart.c:469` reads them back as `(EEPROM[odd]<<8)|EEPROM[even]`, the
  inverse of the write, so the host round-trips correctly).

Read-back: function `0x03` registers `0x0C`–`0x12` return the ESN and
`0x13`–`0x14` the date; the cycle count is register `0x19`. The `Reset ESN`
command (func `0x06`, register `0x0A`) clears the identity via the
`arm_tick_persist` path.

### Other persisted state

The BMS also persists: the **config block** (SOC, RSOC/abs-SOC, full-charge
& remaining capacity, **cycle count**, charge/discharge **current-cal gains**),
the **FW magic**, the **boot mode**, the **anti-replay ticks**, and the
**event log**.

## 4. Provisioning a blank pack

The STM32L0 data EEPROM erases to `0x00`, and the firmware **self-heals**
from an all-zero EEPROM:

- boot mode `0x00` → safe normal boot (not the MOS-failure or protection
  re-entry paths);
- FW magic `0` ≠ `0x011701B1` → `config_init` writes its fallback defaults
  *and* re-persists the magic;
- config-block `+0x28` provisioned flag `0` → `bms_setup` runs factory
  init, writing the default config + secondary blocks back to EEPROM.

So a freshly-erased pack comes up working on first boot. To provision
**explicit** known-good values instead of relying on self-init:

| Write | Value | Why |
| --- | --- | --- |
| `0x08080001` | `0x00` (or `0x0A`) | normal boot. **Never `0x17`/`0x18`** — those boot into the MOS-failure handler, and `0x17` arms `g_fault_flags` bit 6, which can drive the **secondary fuse** (see `state_handler_17_19`, `hardware.md` PB7). |
| `0x0808002E` | `B1 01 17 01` (LE `0x011701B1`) | FW magic — prevents config_init from wiping the validated fields. |
| `0x08080006` | `10 03` (`0x0310`) | validated default. |
| `0x08080032` | `01 0D` (`0x0D01`) | validated default. |
| `0x08080034` | `07` | validated default (low byte). |
| `0x08080036` | `4E 0C` (`0x0C4E`) | validated default. |
| `0x08080C44/45/46` | `41 41 41` (`'A'`) | per-phase voltage markers. |
| `0x08080C00 +0x28` | non-zero | mark provisioned (skip factory init). Leave `0` to let the firmware factory-init. |
| `0x08080C00 +0x24` | initial SOC | starting coulomb count (e.g. measured from open-circuit voltage). |
| `0x08080C00 +0x37` | `100` | RSOC full-scale reference. |
| `0x08080C00 +0x3A/+0x3C` | `E8 03` (`0x03E8`=1000) | charge/discharge current-cal gains (per-mille; `1000` = no correction). Must be in `(900,1099]` or **both** reset to 1000. See §5. |
| `0x0808000F`–`0x0808001C` | 14 ASCII bytes | **ESN** — write via Modbus reg `0x0C`–`0x12` (func `0x10`), not by poking EEPROM directly. |
| `0x0808001D`–`0x08080020` | `00 YY MM DD` | **manufacture date** — write via Modbus reg `0x13`–`0x14`. |

The ESN and date are normally written **over Modbus** (func `0x10`, regs
`0x0C`–`0x14`) by the service tool, which also stamps the anti-replay
triplet — prefer that over poking EEPROM directly so the replay guard and
read-back verification stay consistent.

> **Single most important rule:** do not leave `0x08080001` = `0x17` or
> `0x18`. Every other field either self-heals or only affects gauge
> accuracy; that one byte can trip the irreversible pack fuse at boot.

## 5. Current calibration (`CHG CAL` / `DSG CAL`)

> Corrects this doc's earlier label of config-block `+0x3A`/`+0x3C` as
> "voltage threshold hi/lo". They are the **charge / discharge current
> calibration gains**: `fuel_gauge.c::fg_coulomb_update` (OEM `0x080039c2`,
> runtime `0x0800E9C2`) *multiplies* the measured pack current by them and
> divides by 1000 — it never compares them to a voltage.

The ML5236 measures pack current on the ISP–ISM shunt (`fedl5236.md` §3.3);
the raw 16-bit code is scaled (`×0x32C80 >> 16` ≈ ×3.166 per LSB) into an
internal current, then trimmed by a per-direction gain:

| EEPROM | cfg | SRAM (runtime) | Applies to | Console |
| --- | --- | --- | --- | --- |
| `0x08080C3A` | `+0x3A` | `0x200025B2` (`g_chg_cal_gain`) | charge current (`I ≥ 0`) | `CHG CAL` / `CHG CAL?` |
| `0x08080C3C` | `+0x3C` | `0x200025B0` (`g_dsg_cal_gain`) | discharge current (`I < 0`) | `DSG CAL` / `DSG CAL?` |

`I_corrected = I_raw · gain / 1000`. **Default `1000` = ×1.000 (no
correction).** Usable range **901..1099** (≈ ±10 %); `bms_setup` resets
*both* to 1000 if *either* is `≤ 900` or `> 1099`.

### The calibration procedure (firmware side)

The console commands are dispatched by `command_parser` (`cmd.c`) into
frame-sharing handlers (`cmd_chg_cal_set_h` @ `0x0800A0E8` etc.):

- **`CHG CAL?` / `DSG CAL?`** (no value) print the stored gain back as
  `CHG CAL=<gain>` (it reads `g_chg_cal_gain`), so you can record a pack's
  gains before reflashing and restore them with
  `eeprom_example.py --chg-cal/--dsg-cal`.
- **`CHG CAL=<ref>`** stores `<ref>` (the *known* current being forced
  through the pack, in the firmware's internal current unit) at SRAM
  `0x20002594`, zeroes the live gain, and arms bit `0x2000` of the control
  word `0x20002C00` (`DSG CAL` arms bit `0x4000`). With the bit set,
  `fg_coulomb_update` averages **40 samples** of the raw (un-gained) current,
  computes `measured = Σ·25/ref` (so `measured ≈ 1000` when the reading
  matches `ref`), derives `gain = 2000 − measured` (a linearised inverse
  about 1000), writes it to `cfg+0x3A`/`+0x3C`, **persists the 2 bytes to
  `0x08080C3A`/`0x08080C3C` via `memcmp_verify`**, clears the arm bit, and
  prints `CHG CAL OK` / `DSG CAL OK`.

So the gains are genuinely per-pack (they absorb shunt + amplifier
tolerance) and must be re-measured against a known reference current after a
blank reflash — unless you saved them with the `?` query first. The tooling
writes the neutral default (`1000`) so a freshly-flashed pack reads
uncorrected current until then.
