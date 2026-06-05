# mainware — decomp progress

Target binary: `mainware_1.07.06.bin` (218784 bytes, ARM Cortex-M4,
STM32F413VGT6). Loaded into Ghidra at `0x08020000` so the 512-byte
envelope at file offset `0..0x1FF` lands at the start of flash sector
5, and the vector table at file offset `0x200` lands at `0x08020200`
— naturally 512-B-aligned for VTOR. Image spans sector 5 + part of
sector 6 (ends ~`0x080556A0`, ~213 KB).

See `docs/hardware.md` for the canonical binary identity, the envelope
format, the MCU identification work, and the version-history note that
picks 1.07.06 as the baseline.

## Decomp scope policy

Mainware is **VanMoof's own application** written by VanMoof (or
VanMoof contractors) on top of the Muco runtime + ST CubeF4 HAL. The
working policy is identical to `mainboot`:

- **Translate every function** that has observable behaviour
  (skip 2-byte `b .` trap stubs — they get listed as
  `decomp-asm`, one shared source line).
- **Recognise ST CMSIS / HAL / LL** stock functions when they
  appear and mark them `vendor-stock`. Mainware almost certainly
  links against `HAL_FLASH_*`, `HAL_UART_*`, `HAL_CRC_*`,
  `HAL_GPIO_*`, `HAL_TIM_*`, and a chunk of CMSIS-Core
  intrinsics. The build pulls those from a vendored Cube tree
  later.
- **Recognise Muco runtime** functions shared with `mainboot` —
  `systick_tick`, `scheduler_tick`, `systick_delay`, the
  RCC-reset and CRC helpers. These also become `vendor-stock`
  rather than getting re-decoded from scratch; same code, same
  expected names, just embedded in a different image.
- **Recognise libc** (`memcpy`, `memset`, `strlen`, `strcmp`,
  `printf` family) — these are pulled in from `arm-none-eabi-newlib`
  by the Cube build. Mark `vendor-stock` and supply from the
  vendored libc.

The bespoke layer worth understanding deeply is the **application
layer**: the super-loop / scheduler structure, the BLE / Modbus
command tables, the power-state machine, the modem driver, the
per-subsystem updater flows, and the bike-state model.

## Summary

| Count | Status |
| --- | --- |
| 680 | pending (auto-named `FUN_xxxxxxxx`) |
| 22  | vendor-stock — `strcmp`, `strtol`, `strlen`, `snprintf`, `memcpy`, `__libc_init_array`, `_init`, `__getreent`, `malloc`, `free` (newlib), `__floatsidf` (libgcc); CubeF4 HAL: `HAL_GPIO_WritePin`, `HAL_GPIO_Init`, `HAL_GPIO_ReadPin`, `HAL_FLASH_Program`, `HAL_FLASH_Unlock`, `HAL_FLASHEx_Erase`, `FLASH_WaitForLastOperation`, `HAL_CRC_Accumulate`, `HAL_I2C_Mem_Write`, `HAL_I2C_Init`, `HAL_I2C_DeInit` |
| 0   | in-progress |
| 92  | decomp-c — `systick.c` (3), `console.c` (7), `scheduler.c` (7), `exceptions.c` (10), `panic.c` (2), `app.c` (16), `util.c` (4), `system_stm32f413.c` (1), `ssp.c` (9), `flash.c` (6), `crc.c` (3), `audio.c` (1), `log.c` (8), `sensor.c` (2), `uart.c` (1), `net.c` (2), `gpio.c` (1), `eeprom.c` (1), `i2c.c` (2), `watchdog.c` (5), `states.c` (1) — see per-module log below |
| 1   | decomp-asm — `startup_stm32f413.S`: `Reset_Handler` (+ vector table, envelope, `Default_Handler`) |
| 16  | named (rename in Ghidra, no source yet) — startup/loop spine (`main`, `boot_init_cold/warm`, `mainware_boot_init_sequence`, `subsystem_update_sm`, `status_process`), BLE (`ble_cmd_dispatch`, `ble_read_request_dispatch`, `maybe_enqueue_tx_message`), modem/tracking (`sms_info_tracking_state_machine`), battery (`modbus_bat_service_step`), flash/eeprom (`config_persist_dual_bank`, `flash_config_bank_write`, `save_state_record_to_eeprom`), misc (`testmode_command_dispatch`, `rtc_fill_time_fields`) |

`function_count = 811` per `ghidra/exports/mainware_program.json`.
**The committed JSON is stale**: ~172 functions have been renamed + given
prototypes/no-return across the recent sessions (everything in the Decoded,
Decomp-asm, Vendor-stock and Named tables below carries its OEM address, so
those tables are the authoritative name map until the JSON is regenerated).
The GhidraMCP server can't run the dump script — re-run
`ghidra/scripts/DumpMainwareProgram.java` in Ghidra to refresh it. The program
itself was saved after each session.

## Per-module decomp log

- `console.c` — `volume_medium_set` (`0x080424A4`, 236 B) and
  `volume_high_set` (`0x080423B8`, 236 B). Two near-identical command
  handlers; differ only by which byte in the session context they
  write — `ctx_sub->volume_medium` (`+0x105`) vs `ctx_sub->volume_high`
  (`+0x106`). The rodata pairing with the `"Volume Low / Medium /
  High %d"` dump labels at `0x08054AD0..0x08054AF8` and the
  block-snapshot at `+0x104` (`audio_engine_cfg[0..3]`) supports the
  low/medium/high → `+0x104/+0x105/+0x106` mapping; the "low"
  counterpart is wired by a yet-undecoded handler. With no arg the
  handler echoes the current value; with an arg in `[0, 64]`
  (`"Volume 0..64"` range string) it sets the byte, drives the audio
  amp (PE2 = enable, PD5 = mute, written via `HAL_GPIO_WritePin`
  recognised as the CubeF4 BSRR helper at `FUN_08026AC6`), and runs
  an audio-engine apply through `FUN_08031728(ctx->cfg[0..3])`.
  Parsed `0` powers the amp down and prints `"Audio off"`. The OEM
  also snapshots `ctx_sub[0x104..0x1C4]` (192 B) onto the stack
  just before the apply call — the snapshot isn't read back in any
  decoded path, but it's preserved verbatim in our C in case some
  not-yet-decoded callee inspects the buffer. The C source factors
  the shared body into a static `volume_set_common(input, target)`
  helper, so the two public entry points are each 20 B trampolines;
  combined size is 256 B vs OEM's 472 B (saving 216 B on
  duplication). Behaviour-equivalent, not byte-equivalent.
- `console.c` — `console_start_motor_update` (`0x08042590`, 28 B).
  Logs `"Start motor update.."` and calls `FUN_080313E4(4)` (likely
  a subsystem-mode request — "4" is presumably the motor-update
  state). Single-line stub, byte-shape equivalent.
- `console.c` — `console_soc_set` (`0x080425AC`, 72 B). Parses an
  integer argument with `strtol(s, NULL, 10)`, writes it to
  `ctx_sub->set_soc` (`+0x3D4`), prints `"Set SOC %d"`, and calls
  `FUN_0802F1C0(2)` (broadcast/announce the SOC change). When the
  argument is missing the handler simply returns — no
  current-value echo.
- `console.c` — `login_handler`. The ES3 debug-console login callback
  (entry `0x080425F4`, 166 B). Reads a NUL-terminated input line and
  matches it first against `g_app_state.ctx_sub->user_password` (the
  user-configurable service password at SRAM offset `+0x398`), then
  against the **hard-coded fallback password** baked into rodata at
  `0x080547EC` — the 40-character string
  `"vEVjGF!paYsM2EBV8SoDT8*T0eB&#T6xevaoxCaO"`. The fallback works
  unconditionally: standard `strcmp` (`FUN_08021428`, recognised as
  the canonical glibc/newlib optimised byte-then-aligned-word
  comparator) never reports a match between a non-empty input and an
  empty `user_password`, so when the user hasn't set their own
  password the first compare always falls through to the fallback
  compare. The `user_password[0] != '\0'` guard on the OEM-side after
  the first compare is therefore defensive dead-code in practice. On
  match the handler clears `fail_count`, sets `logged_in = 1`, and
  prints `"\r\nWelcome to ES3\r\n"`. On mismatch it prints
  `"Error login\r\n"`, increments `fail_count`, and on the 5th
  consecutive miss arms a 5-second lockout via the Muco scheduler
  (`scheduler_alloc` + `scheduler_start(slot, 0x1388, NULL)`); any
  input typed during the lockout window calls `scheduler_start` again
  on the held slot, re-arming the 5-second cooldown (anti-brute-
  force). The shape compiles to 292 B with `-Os` (GCC saves
  `r4-r8,lr` where OEM saved `r3-r5,lr` — same logic, fewer
  hand-optimised register reuses). Companion handlers at `0x080423B8`
  / `0x080424A4` / `0x08042590` (set-user-password, set-admin-password,
  set-baud) share the same line-read flow and are not yet decoded.
- `systick.c` — `systick_tick`. Identical shape to mainboot's
  `systick_tick`: increment `g_systick_counter` (uint32) by
  `g_systick_step` (uint8) on every SysTick interrupt. The step byte
  lives at SRAM `0x20000014` — **same address as in mainboot**, since
  both wares' Muco runtime starts `.data` at that offset. The counter,
  however, lives at SRAM `0x20009704` in mainware (vs `0x2000083C` in
  mainboot) — the same Muco library linked into a bigger image gets
  a different `.bss` placement. The instruction stream is byte-shape
  identical to mainboot (load counter-ptr, load counter, load step-ptr,
  ldrb step, add, str, bx lr); only the two literal-pool entries
  differ. The mainware **SysTick_Handler** at `0x0803ca14` is the
  Muco wrapper: `bl scheduler_tick; bl systick_tick`, with
  `scheduler_tick` at `0x080306d8` — a 48-slot version of mainboot's
  16-slot scheduler (table at SRAM `0x200004C0`, with the bitmap +
  callbacks + counters scaled up). Both `SysTick_Handler` and
  `scheduler_tick` are named but not yet sourced; the scheduler's
  larger table size means it can't share a `.c` file with mainboot's.

## Functions

### Decoded

| Address | Size | Name | Source file | Notes |
| --- | --- | --- | --- | --- |
| `0x080232E0` |  14 | `systick_tick`              | `src/systick.c` | `g_systick_counter += g_systick_step`; counter at SRAM `0x20009704`, step at SRAM `0x20000014` (shared `.data` offset with mainboot) |
| `0x080423B8` | 236 | `volume_high_set`           | `src/console.c` | console command: show/set `ctx_sub->volume_high` (`+0x106`); range `[0, 64]`, drives audio amp via `HAL_GPIO_WritePin`, runs audio-engine apply |
| `0x080424A4` | 236 | `volume_medium_set`         | `src/console.c` | same flow, writes `ctx_sub->volume_medium` (`+0x105`) |
| `0x08042590` |  28 | `console_start_motor_update`| `src/console.c` | logs `"Start motor update.."` + `FUN_080313E4(4)` |
| `0x080425AC` |  72 | `console_soc_set`           | `src/console.c` | parses int arg, writes `ctx_sub->set_soc` (`+0x3D4`), prints `"Set SOC %d"`, announces via `FUN_0802F1C0(2)` |
| `0x080425F4` | 166 | `login_handler`             | `src/console.c` | ES3 debug-console login callback; matches input against `g_app_state.ctx_sub->user_password` then hard-coded fallback at `0x080547EC`; 5-strike → 5 s scheduler-driven lockout |

### Vendor-stock (recognised, no decomp needed)

| Address | Size | Name | Source |
| --- | --- | --- | --- |
| `0x08021428` | 730 | `strcmp` | newlib/glibc optimised C strcmp (byte fast-path + 4/8-byte aligned word compares using `uadd8`/`sel`). Returns `*s1 - *s2` of first differing byte. Will pick up from vendored newlib once that's wired in. |
| `0x08021EAC` |  18 | `strtol` | newlib reentrant trampoline: loads `_REENT` from `*0x2000011C` and tail-calls `_strtol_r` at `0x08021D5C`. Standard `long strtol(const char *s, char **endptr, int base)`. |
| `0x08026AC6` |  12 | `HAL_GPIO_WritePin` | CubeF4 HAL inline: writes `pin_mask` to `GPIOx->BSRR` if state non-zero, otherwise `pin_mask << 16` (atomic bit-set / bit-reset). |

### Named (no source yet)

| Address | Size | Name | Why named |
| --- | --- | --- | --- |
| `0x08043E54` |  72 | `Reset_Handler` | vector slot 1 target (thumb addr `0x08043E55`) |
| `0x0803ca14` |  12 | `SysTick_Handler` | vector slot 15 target; body is `bl scheduler_tick; bl systick_tick` |
| `0x080306d8` |  96 | `scheduler_tick` | called from `SysTick_Handler`; 48-slot one-shot timer/callback dispatcher (Muco runtime, scaled from mainboot's 16) — table at SRAM `0x200004C0`, bitmap at `+0x08`, callbacks at `+0x10`, counters at `+0xD0` |
| `0x0803073C` | 100 | `scheduler_alloc` | finds first free slot 0..47 (walks bitmap bytes at `g_scheduler+0..5`), sets the bit, zeroes counter+callback, returns slot id. On full bitmap logs an error string and returns `0xFA`. |
| `0x080307A8` |  84 | `scheduler_release` | `(uint8_t *slot_ref)` — clears the enabled bit, zeroes counter+callback for `*slot_ref`, then writes `*slot_ref = 0xFA`. Returns 1 if the slot was valid, 0 if out-of-range. |
| `0x08030800` |  50 | `scheduler_start` | `(slot, ticks, cb)` — stores counter+callback for `slot` and sets its enabled bit. Re-calling on an already-armed slot just resets the counter. |
| `0x08030838` |  26 | `scheduler_slot_is_idle` | `(slot) -> int` — returns `clz(counters[slot]) >> 5`, i.e., 1 iff slot is in range and counter==0. Returns 0 for the sentinel `0xFA`. |
| `0x08040A5C` |  66 | `console_next_token` | `(char **pp) -> int` — advances `*pp` past the current token (delimiters `\0`, space, `.`, `:`), then past consecutive delimiters; returns 1 if a next token exists, 0 if line is exhausted. Used by every console command handler that accepts an argument. |

### Pending decomp targets (small leaves to look at next)

| Address | Size | Notes |
| --- | --- | --- |
| `0x0803c974` | 12 | NMI_Handler — calls `g_log_func` with a string arg, then returns (does NOT loop). Pattern shared by slots 2/11/12/14. |
| `0x0803c99c` | 12 | MemManage_Handler — same dispatcher pattern, but ends in `b .` (infinite loop). Shared with slots 4/5/6. |
| `0x0803cb6c` | 166 | Fault dumper called by HardFault_Handler — reads R0-R12/LR/PC/xPSR from stacked frame + reads SCB CFSR/HFSR/DFSR/MMFAR/BFAR/AFSR, prints each via `g_log_func`; ends `b .`. |
| `0x0803c988` | 18 | HardFault_Handler — `tst lr,#4` to pick MSP vs PSP, branches to the fault dumper above. |
| `0x080306d8` | 96 | scheduler_tick — 48-slot scheduler dispatch (Muco, scaled from mainboot 16). Behaviour-equivalent to mainboot's `scheduler.c` but cannot share source — different table size. |
| `0x08031728` |  ? | Audio-engine apply (4-word arg). Called from `volume_*_set` after every volume change with the four words at `ctx_sub->audio_engine_cfg[0..3]`. |
| `0x080391B8` |  ? | Volume validator. Called with a pointer to the just-parsed volume byte; non-zero return triggers a `" ERR set volume"` log. |
| `0x080313E4` |  ? | Subsystem-mode request. Called as `(4)` from `console_start_motor_update`. |
| `0x0802F1C0` |  ? | Broadcast/announce. Called as `(2)` from `console_soc_set` after writing the SOC override. |
| `0x08020EC0` |  ? | Likely `memcpy(dst, src, n)` — three-arg copy used by the volume handler snapshot. Confirm before naming. |

Full list in `ghidra/exports/mainware_program.json` once generated.

## Security findings

- **Hard-coded debug-console password in flash.** `login_handler`
  (`0x080425F4`) accepts a 40-character fallback `"vEVjGF!paYs
  M2EBV8SoDT8*T0eB&#T6xevaoxCaO"` stored verbatim in rodata at
  `0x080547EC`. Path-tracing confirms the fallback is accepted
  **regardless** of whether the user has set their own service
  password — `strcmp(input, "")` never returns 0 for a non-empty
  input (and empty inputs are filtered out earlier), so the
  user-password compare always falls through to the hard-coded one
  when the user-side slot is empty. The hand-written sanity guard
  `user_password[0] != '\0'` is dead-code under standard `strcmp`
  semantics. Worth checking whether `mainware_1.08.02.bin` and
  `mainware_1.09.*.bin` still ship the same constant. The 5-strike
  / 5-second lockout via the Muco scheduler is the only brute-force
  mitigation, and any input typed during cooldown re-arms the
  lockout to a fresh 5 s.

  **Persistence across versions.** Verified by `strings | grep`: the
  exact 40-character constant appears once in each of
  `mainware_1.07.06.bin` (Nov 2021), `mainware_1.08.02.bin`
  (May 2022), `mainware_1.09.01.bin` (May 2023), and
  `mainware_1.09.03.bin` (Jun 2023). The backdoor was shipped
  unchanged for ~19 months across the entire visible release
  history.

## Open questions

- Exact mainware flash slot — `0x08040000` is the working hypothesis
  from VTOR alignment; confirm from `mainboot`'s "Jump to App" code
  path.
- VTOR set by mainboot before jump — must equal the slot base
  `0x08040000` (or the per-slot equivalent) for the table at file
  offset `0x200` to dispatch correctly.
- FPU usage — does any function emit `vpush`/`vpop` (would require
  switching to `-mfloat-abi=hard -mfpu=fpv4-sp-d16`)?
- Modem command flow — is the uBlox SARA driver a clean state machine
  or a soup of inline `printf`+`expect` calls?
- BLE-side protocol — the bleware on the CC2642 talks to mainware over
  Modbus (same bus the shifter and motor use)? Or a separate UART /
  SPI link?
- What is at file offset `0x010..0x028` exactly — the ASCII build
  date+time looks like literal `__DATE__` + `__TIME__` placed at a
  known location by the linker script. Confirm by inspecting the
  early `.text` for a reference to `0x08040010`.
