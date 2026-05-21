# bleware — decomp progress

Target binary: `bleware_1.4.01.bin` (181884 B, ARM Cortex-M4F + BLE 5.2,
TI CC2642R1F). The newer `bleware_2.4.01.bin` exists too (217884 B) but
is deferred; the 1.4.01 build is the initial decomp target because
it's smaller and matches bleboot 1.0.0's Apr 2020 build cadence.

See `docs/hardware.md` for the canonical binary identity (size, SHA
hashes, OAD-NVM1 header layout) and `bleboot/docs/progress.md` for
the BIM that this image was built to be promoted by.

## Decomp scope policy

**Decode only VanMoof-custom code.** Functions that are byte-for-byte
copies of canonical vendor sources (TI SimpleLink SDK 3.40 — driverlib,
BLE stack, kernel, OAD framework) are *recognised* and marked
`vendor-stock` — no separate C translation, no source file in `src/`.
The byte-equivalent build will later pull these in from a vendored copy
of the SimpleLink SDK, but the decomp work itself focuses on the
bespoke parts of the bleware application — the GAP/GATT services, the
S3 protocol bridge, the inter-module Modbus relay, key-storage,
peripheral wiring, anything that's specific to VanMoof.

## Summary

| Count | Status |
| --- | --- |
| ?? | pending (TBD as functions are categorised) |
| 0 | vendor-stock (recognised; no decomp needed) |
| 0 | in-progress |
| 0 | decomp (asm or c) |
| 0 | named (rename in Ghidra, no source yet) |

`function_count` to be populated from `ghidra/exports/bleware_program.json`
once the per-program dump script is set up.

## Boot path (observed from the OAD header alone)

The cold-boot chain has two phases, both run on the same CC2642R1F MCU
but from different flash regions:

1. **bleboot (BIM)** — fully decoded in `bleboot/`. Reset vector goes
   into the BIM at flash `0x00056000`. BIM's `bim_quick_scan_and_launch`
   walks the 44 OAD slots on internal flash, finds the slot whose
   `imgCpStat` byte is `0xFE` (= "promoted"), and jumps to its
   `prgEntry`.
2. **bleware** — once the BIM has promoted this image into internal
   flash, `prgEntry = 0x00000090` is stripped (header copied to a
   metadata region; image bytes start at flash `0x00000000`). Vector
   slot 1 = `Reset_Handler` at flash `0x0001F590`.

So bleware's effective entry point is `0x0001F590` (file offset
`0x90 + 0x0001F590 = 0x1F620` in the raw OAD bin).

## Per-module decomp log

### `startup.c` — Reset_Handler chain

OEM `Reset_Handler` at flash `0x0001F590` (70 B body). One large function
that fuses what bleboot splits into two (`Reset_Handler` +
`ResetISR_body`). Body, in order:

1. Enable FPU coprocessors: `SCB->CPACR |= 0xF00000` (CP10+CP11).
2. Materialise initial MSP: `SP = (_stack_base + _stack_size) & ~7`
   where `_stack_base = 0x20013A00` (pool word at `0x0001F5D8`) and
   `_stack_size = 0x00000600` (`0x0001F5DC`). Result: `MSP = 0x20014000`
   — same SRAM top as bleboot.
3. Save the resolved MSP to a runtime global at `0x20005B30`
   (pool `0x0001F5E0`).
4. Test the cinit-handler function pointer at pool word `0x0001F5E4`
   (value `0x00027773` = Thumb-set of `0x00027772`). On this build
   the pointer is non-null, so the path always taken is:
   - `MOV LR, 0x0001F5C9` (return address after the indirect call)
   - `BX 0x00027772` — that's a 4-byte thunk that jumps to
     **`SetupTrimDevice_candidate` @ `0x0001878C`** (TI driverlib
     `SetupTrimDevice` mirror, the same silicon trim routine
     bleboot calls from its own `Reset_Handler`)
   - When trim returns, control resumes at `0x0001F5C8`.
5. `BL cinit_walker @ 0x00017EF0` — the canonical TI C-runtime init.
   Two-pass walker: first walks the cinit (BSS-fill / .data-copy)
   table at `[DAT_F80, DAT_F84]`, dispatching each entry to a
   handler from the table at `DAT_F88`; then walks the auto-init
   constructor table at `[DAT_F78, DAT_F7C]` with an optional
   pre-pass hook at `DAT_F8C`.
6. `BL main @ 0x00026474` — TI-RTOS application main. After modest
   setup it calls `BIOS_start()` (a thunk to ROM `0x1002EAA4`) which
   never returns.
7. `BL 0x00027A20` — local `_exit` thunk to ROM `0x1002F7B0` (TI's
   `_exit` handler).
8. `B .` — trap if anything returns up to here (unreachable in
   practice).

### `main` chain — confirmed entry point

`Reset_Handler @ 0x0001F590` ultimately calls `main` via the canonical
TI-CGT main trampoline:

- **`main_trampoline` @ `0x00026474`** (18 B) — reads `argc`/`argv`
  from the global pointer at `0x00026488` (clamping `0xFFFFFFFF` to
  null) and tail-calls the actual main via `b.w 0x0001CFEC`.
- **`main` @ `0x0001CFEC`** — the real main. About 130 B; standard
  TI-RTOS / SimpleLink BLE 5 startup sequence:
  1. `FUN_0001BAE4(argc, argv, p3, p4)` — board / power-management
     init (touches AON, ROM HAPI via `ROM_API_TABLE_PTR[2]`)
  2. `FUN_00027328(DAT_0001D048)` — single-store helper (sets a
     globally-visible kernel pointer)
  3. **`tirtos_modules_init` @ `0x000215FC`** — calls 13 sub-init
     functions (TI-RTOS module initialisers); halts on failure
  4. Two ROM-API indirect calls through `ROM_API_TABLE_PTR` at
     `0x100001D8` (slots `[0]` and `[1]`) — likely
     `OsalLink_init` / `OsalPort_init` or the SimpleLink BLE-stack
     `ICall_init` pair
  5. `FUN_00023A58()` — returns something, stashed into a kernel
     struct slot — likely `Hwi_construct` or `Swi_construct` for an
     ISR object
  6. `FUN_00024DE0()` — zeros some kernel state, calls `FUN_00019E60`
     (BLE stack init helper?)
  7. `FUN_00024058()` — copies a 16-byte param struct, calls
     `FUN_00018210(&struct, 1)` (another init/register call)
  8. **`create_bluetoothtask` @ `0x000235D0`** — constructs the
     `bluetoothtask`: prio 3, stack at `0x200082B0`, stack size
     `0x708` (1800 B), entry **`bluetoothtask_main` @ `0x000067C8`**
  9. **`thunk_EXT_FUN_1002EAA4`** → ROM `0x1002EAA4` —
     **`BIOS_start()`**, the TI-RTOS scheduler entry. Never returns
     in practice (but main is structured to handle a return —
     epilogue is `movs r0, #0; pop {r3, r4, r5, pc}`).

### `bluetoothtask_main` @ `0x000067C8` — the BLE event loop

Embedded path string at `0x00006A23` confirms: **`source/tasks/bluetoothtask.c`**.

Body shape:
- Initial setup (allocate a few service objects via `FUN_000271E2` / `FUN_000203E0` / `FUN_0002183C` / `FUN_000232B8(1)` / `FUN_0002654C` / etc.)
- Read shifter-tool-style identity fields (the GAP device name / advertisement payload — `FUN_0001AC6C(0x10, DAT_*, ...)` looks like a "post log message" helper, several calls with format-string-like args)
- Enter the event-loop: `Event_pend(event_handle, 0, 0xC0000000, 0xFFFFFFFF)` via ROM thunk `0x1002C0A4`
- Per event:
  - Dispatch via `FUN_00023ED0(&local_2c, local_2a, &local_30)` — likely `ICall_fetchServiceMsg` (reads an inbound BLE-stack message into local storage)
  - If `local_2c == 0x10` AND `local_2a[0]` matches the task ID:
    - **Event class `0x91`** (msg byte 1): sub-codes `0x0E`, `0x0F` (`FUN_2777E`), `0x10` (hardware error → `FUN_00006D90(...)` formats a `"Hardware error <d>"` log line), `0x3E`
    - **Event class `0xB0`** (msg byte 2 at `local_30[2]`): sub-codes `0x7F` (`FUN_27648`), `0x1E` (`FUN_2777A`), `0x7E` (drained but no handler — see also `thunk_EXT_FUN_10025E28`)
    - **Event class `0xD0`** (msg byte 1): sub-codes `0x00`, `0x05` (`FUN_23E00`), `0x06` (`FUN_256A2` — sends a `1` to log first, then calls), `0x07` (`FUN_24B64`), `0x11` (`FUN_2211C`)
  - Bit 30 of the event flag (`0x40000000`) — drain the user-msg queue (`FUN_26198`), dispatch on first byte:
    - `0x02` — connection event (5-arg call to `FUN_00025FFC` with addr fields at +6/+8/+9/+0xC of the inner struct)
    - `0x03` — `FUN_00014E10(byte0, halfword2, byte4)` — a smaller BLE event
    - `0x04` — connection/disconnect: if inner struct is null, logs `"Force disconnect all pending con..."`; otherwise calls `FUN_0001AC6C(0x10, DAT_*, halfword2, ...)`
    - `0x32` — `FUN_26736(byte0, dword4)` — another event
  - Free the message buffer (`FUN_00021B88`)

`FUN_0001AC6C(0x10, ...)` is clearly a **structured-log emit** — the leading `0x10` is the log severity / verbosity; many call sites pass varargs that line up with embedded format strings (e.g. `"Hardware error <d>"`). The `FUN_00006D90` calls are similar but include source-file / line-number for error logs.

### Audio task

A second VanMoof task lives in `source/tasks/audiotask.c` (string at
`0x0000B2E0`). The constructor isn't called directly from `main` —
it's created from somewhere inside the module-init chain. To
identify in a follow-up pass; the audiotask is the BLE-audio-streaming
companion to the bluetoothtask.

### Functions renamed in Ghidra this turn

| Address | Name | Notes |
| --- | --- | --- |
| `0x00026474` | (still `main` in Ghidra) | should be `main_trampoline`; the Ghidra body fuses both functions because of the `b.w` tail call |
| `0x0001CFEC` | `main` (Ghidra's auto-tagged) | the real main body |
| `0x000215FC` | `tirtos_modules_init` | calls 13 sub-init functions |
| `0x000235D0` | `create_bluetoothtask` | TI-RTOS Task_construct for the BLE task |
| `0x000067C8` | `bluetoothtask_main` | the BLE event-loop body |

### Boot-flow correspondence with bleboot

| Stage | bleboot symbol | bleware symbol |
| --- | --- | --- |
| Silicon trim | `SetupTrimDevice` @ `0x0005667C` | **`SetupTrimDevice`** @ `0x0001878C` ✓ confirmed byte-equivalent |
| C runtime init | `_auto_init_table` (manual, see `auto_init.c`) | `cinit_walker` @ `0x00017EF0` |
| User entry | `main()` (in `bim.c`) | `main` @ `0x00026474` |
| Exit / trap | `_exit` (stub in `panic.c`) | thunk @ `0x00027A20` → ROM `0x1002F7B0` |

### `setup_trim.c` — `SetupTrimDevice` @ `0x0001878C` (vendor-stock)

106 B body, **algorithmically identical** to bleboot's `SetupTrimDevice` @ `0x0005667C` (108 B). Confirmed step-for-step against `bleboot/src/setup_trim.c`:

1. Read `FCFG1_REVISION` at `0x5000131C`; clamp `0xFFFFFFFF` to `0`.
2. `bim_chip_assert_supported()` — spins forever if not CC13x2/CC26x2 HwRev20+.
3. `*(0x42600484) = 0` — clear `FLASH_FSM_ACK` (bit 1 of `FLASH+0x24` via bit-band alias).
4. Call `((void**)ROM_HAPI_TABLE_PTR)[18]()` — ROM HAPI cold-reset hook.
5. If `*(0x43280180) != 0`: read `*(0x43200580)` (side effect), then `bim_setup_after_cold_reset_cfg1(fcfg1_rev)`.
6. `*(0x4008218C) = 0` — clear AON_PMCTL register.
7. Mask `*(0x40032048)`: clear bits [27:17], OR in `0x01390000`.
8. If bits [13:12] of `*(0x40090028) == 1`: pulse bit `0x20000` (write `masked|0x20000`, then `masked`).
9. Spin while `*(0x4268000C) != 0` (VIMS mode-change in progress).

Compiler-side codegen differences (CGT/armcc on bleware vs GCC on bleboot):

- bleware shares the pool word `0x42600494` (used by `bim_setup_after_cold_reset_cfg1`) and derives `0x42600484` for step 3 via `subs r0, #0x10`. bleboot emits both as independent pool words.
- bleware uses a single `bfc r2, #0x11, #0xb` (bit-field clear) for the bit-mask in step 7; bleboot emits the equivalent `and` against a `0xF801FFFF` pool word. Same end result.

Helper functions now named in bleware Ghidra (all vendor-stock TI driverlib, same role as in bleboot):

| Address | Name | Role |
| --- | --- | --- |
| `0x0001878C` | `SetupTrimDevice` | silicon trim entry from Reset_Handler |
| `0x000266C8` | `bim_chip_assert_supported` | family/HW-rev check (CC13x2 fam 4, rev ≥ 0x14) |
| `0x00025D24` | `bim_chip_family` | reads FCFG1 family byte |
| `0x00021BCC` | `bim_chip_hw_revision` | reads FCFG1 hw-rev byte |
| `0x000173E8` | `bim_setup_after_cold_reset_cfg1` | cold-reset trim cfg helper (called when `BB_AON_GATE` is set) |

The architectural difference: bleboot is built with TI's GCC tooling
and uses straightforward inline calls. bleware is built with what
looks like **TI's CGT/ARM Compiler 5** chain — the cinit table
walker, the indirect-call-via-DAT_5E4 pattern for the trim hook, and
the auto-init constructor pass after cinit all match the
`startup_cc26x2_armcc.s` / `lnk_armcc.cmd` upstream pattern. So
where bleboot writes "call SetupTrimDevice" directly, bleware
writes it as a function pointer the linker resolves into a thunk —
same end result, slightly different code shape.

## VanMoof source-file inventory (from embedded path strings)

Strings sweep on bleware 1.4.01 surfaces **22 VanMoof source files**.
This is far more code than the ~30 KB initial estimate; the binary
ships full BLE-app + audio + monitor-console + OAD pipelines.

| Path | Purpose (inferred) |
| --- | --- |
| `source/tasks/bluetoothtask.c` | BLE event loop (= `bluetoothtask_main` @ 0x67C8) |
| `source/tasks/audiotask.c` | BLE-audio streaming task |
| `source/xs3_app.c` | VanMoof S3 application top-level |
| `source/xs3_statemachine.c` | top-level state machine |
| `source/xs3_hci.c` | low-level BLE controller (HCI) interface |
| `source/xs3_gap_adv.c` | GAP advertising |
| `source/xs3_gap_bondmanagement.c` | pairing/bonding (security manager) |
| `source/xs3_gatt_config.c` | GATT service config |
| `source/xs3_gatt_read.c` | GATT read handlers |
| `source/xs3_gatt_write.c` | GATT write handlers |
| `source/xs3_gatt_backoffice.c` | GATT "back-office" service — likely the bike's commercial/admin features over BLE |
| `source/protocols/ssp.c` | "SSP" protocol — candidate for the inter-module Modbus bridge |
| `source/filetransfer.c` | file transfer over BLE (firmware images?) |
| `source/oad/oad.c` | OAD service (over-the-air update) |
| `source/oad/pakfs.c` | "Pack-FS" — custom file system used by OAD |
| `source/monitor/monitor.c` | debug-console dispatcher |
| `source/monitor/cmd_audio.c` | monitor `audio` command |
| `source/monitor/cmd_ble.c` | monitor `ble` command |
| `source/monitor/cmd_exit.c` | monitor `exit` command |
| `source/monitor/cmd_extflash.c` | monitor `extflash` command (external SPI flash) |
| `source/monitor/cmd_help.c` | monitor `help` command |
| `source/monitor/cmd_info.c` | monitor `info` command |
| `source/monitor/cmd_log.c` | monitor `log` command (probably reads the structured-log buffer) |
| `source/monitor/cmd_os.c` | monitor `os` command (kernel introspection?) |
| `source/monitor/cmd_packfs.c` | monitor `packfs` command |

Logging is structured: `FUN_0001AC6C(0x10, fmt, ...)` is the level-10
log emit; `FUN_00006D90(file, line, func, level, fmt, ...)` is the
"location-aware" emit used in error paths.

## Open questions

- Which SimpleLink SDK version? `SetupTrimDevice` byte-equivalence to
  bleboot's confirms the **driverlib** API matches bleboot's SDK 3.40,
  but the **toolchain** is different (CGT/armcc on bleware vs GCC on
  bleboot). The BLE-stack version is still pending — likely BLE5
  Stack 3.x given the Apr 2020 build cadence.
- How does the bluetoothtask talk to mainware? `source/protocols/ssp.c`
  is the most likely answer — `ssp` could be "Shared Serial Protocol"
  or a derivative of the SimpleLink "Serial Bootloader" protocol.
  Decoding it will reveal whether it's Modbus-style RTU or something
  else.
- Where is the audiotask created? Not directly from `main`'s call
  chain — must be inside one of the TI-RTOS module-init helpers
  (`FUN_00024058`, `FUN_00024DE0`, or one of the 13 calls in
  `tirtos_modules_init`).
- Inter-module bus pinout on the CC2642 side: which DIO carries UART
  TX vs RX? GPIO config is in the CCFG (resides inside the BIM page,
  so still under `bleboot/` decomp scope).
