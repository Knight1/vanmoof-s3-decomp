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

### Monitor (debug console) — architecture sketched

The monitor console exposes ~19 user-facing commands across 8-9
`cmd_*.c` source files. Command names live as NUL-terminated strings
in flash:

| Address | Strings |
| --- | --- |
| `0x0002A5C4` | `cmd_play_audio_file`, `cmd_dump_audiofiles`, `cmd_upload_audio_file`, `cmd_upload_audio_file2`, `cmd_set_all_audio_volume` |
| `0x0002AA44` | `cmd_list_pack_file`, `cmd_upload_pack_file`, `cmd_delete_pack_file`, `cmd_process_packfile` |
| `0x0002AB94` | `cmd_extflash_dump`*, `cmd_extflash_erase`*, `cmd_extflash_verify`, `cmd_extflash_upload`* |
| `0x0002B081` | `cmd_log_flush`, `cmd_log_count`, `cmd_log_inject` |
| `0x0002B1F8` | `cmd_ble_info`, `cmd_ble_set_random_static_address` |
| `0x0002B448` | `cmd_rtos_stats`, `cmd_rtos_nvm_compact` |
| `0x0002B7E0` | `mon_help`, `cmd_help` |
| `0x0002BA58` | `cmd_exit` |

Strings marked `*` are dead `.rodata` — the OEM source defined those
helpers, but the linker dropped the bodies. Both a 32-bit literal-pool
scan and a MOVW-immediate scan over the entire image confirm zero
references. Per-TU `.rodata.str` survives whole when at least one
sibling function in the same TU is kept (e.g. `cmd_extflash_verify`
keeps `source/monitor/cmd_extflash.c`'s string blob alive). For
`cmd_extflash.c` specifically, only `extflash-verify` is reachable
from a registered monitor command — see `src/monitor/cmd_extflash.c`.

**Monitor command-handler ABI** — every `cmd_*` handler exposes
one entry point with the universal signature `int cmd(int verb,
void *p2, void *p3, uint32_t p4)` and dispatches on `verb`:

  - `0` **PRINT_HELP** — calls `monitor_print_help_line(name,
    description)` (OEM `0x00021244`, the printf-style helper that
    emits `"    %-33s - %s\r\n"` via `monitor_log`). Walked by
    `cmd_help` over the static command table.
  - `1` **FILL_NAME** — calls `memcpy(p2, name, 0x10)` to copy the
    command name into a caller-provided 16-byte buffer. Used by the
    monitor's tab-complete / introspection path.
  - `2` **EXECUTE** — runs the command with user-input string at `p2`.
  - default — return `1`.

  Return value: `0` on success; non-zero values have command-
  specific meaning (`monitor_dispatch_loop` treats a non-zero return
  as "not my command — try the next").

**The registry IS the static table at `0x0002A0BC`** — 27+ function
pointers, NULL-terminated. No startup registration walker exists.
The table is walked by:
  - `cmd_help` (OEM `0x00013BE8`) with verb=0
  - `monitor_dispatch_loop` (OEM `0x00024B38`) with verb=2

**ABI history note** — early in the decomp the function at OEM
`0x00021244` was misidentified as `monitor_register_command` and
`0x00018654` was misidentified as `monitor_print_help_line`. The
strings at `0x00021258` (`"source/monitor/cmd_help.c"`) and
`0x00021278` (`"    %-33s - %s\r\n"`) together with the unique
many-callers fingerprint of `0x00018654` (it's `memcpy`) corrected
both. The verb selector is correspondingly reordered: there is no
"register" verb in this binary.

**`cmd_help` @ `0x00013BE8`** — full universal command handler.
Decoded into `src/monitor/cmd_help.c`. Verb 1 writes the name
literal `"help\0"` into the caller's buffer (the OEM compiler inlines
this as a word + byte store — 5 bytes total, not the 16-byte memcpy
seen in longer-named handlers like `extflash-verify`); verb 0 emits
the help row (`help` / `show all monitor commands`); verb 2 self-matches
the user input via `monitor_command_matches`, logs the two banner lines
from `source/monitor/cmd_help.c`, then iterates the static command table
at flash `0x0002A0BC` and calls each handler with verb 0.

Note: Ghidra initially placed the `cmd_help` function entry at
`0x00013C20`, but the real prologue (`push {r2,r3,r4,r5,r6,lr}`) lives
at `0x00013BE8`. The 24-byte gap is the verb=1/0/!2 dispatch
preamble. Table entry 24 (`0x00013BE9`, thumb) confirms the corrected
entry. The Ghidra function has been recreated at the right address.

**Command table @ `0x0002A0BC` decoded.** The table is a packed,
Thumb-set function-pointer list with the first NULL after 25 entries:

| Slot | Ptr | Handler | Name | Effect (verb=2) |
| --- | --- | --- | --- | --- |
| 0  | `0x0001A969` | `cmd_firmware_update` | `firmware_update` | calls OAD entry `FUN_0000D444` |
| 1  | `0x0000ABD9` | `cmd_extflash_verify` | `extflash_verify` | scans ext-flash CRC |
| 2  | `0x0001699D` | `cmd_log_count` | `log_count` | logs `log_block_count_get()` value |
| 3  | `0x0000C2D5` | `cmd_log_dump` | `log_dump <start_idx> <n>` | dumps N 16 B log blocks |
| 4  | `0x00016DCD` | `cmd_log_flush` | `log_flush` | erases the 128 KB log region (`FUN_000230D8`) and restarts the log writer (`FUN_00017B24`) |
| 5  | `0x0000E191` | `cmd_log_inject` | `log_inject <n>` | submits N fake `logblock <%d>` entries |
| 6  | `0x0000F19D` | `cmd_audio_play` | `audio_play <index>` | calls `FUN_000275B8(index & 0xFF)`; rejects index ≥ 0x7B |
| 7  | `0x0001CE3D` | `cmd_audio_stop` | `audio_stop` | calls `FUN_00027630(1)` |
| 8  | `0x0001522D` | `cmd_audio_dump` | `audio_dump` | opens ext-flash, loops 0..0x7A calling `FUN_0000D5CC` to dump each clip |
| 9  | `0x0000CAE1` | `cmd_audio_upload` | `audio_upload <index>` | writes `0x200000 + index*0x80000` into a global, YModem-receives via `FUN_000101B0(target, 0x300000)`, then `module_forward_async(0x5571, index)` |
| 10 | `0x0000C615` | `cmd_audio_volume_set_all` | `audio_volume_set_all <level>` | `module_publish_command(0x5572, payload[12], 0xC)` where payload[+level*4..+level*4+4] = 0xFFFFFFFF, others 0 (per-channel mask) |
| 11 | `0x0001406D` | `cmd_pack_upload` | `pack_upload` | `module_forward_async(0x10E, 0)`, YModem-receives `FUN_000101B0(0x80000, 0x180000)`, processes PACK on success / forwards `(0x10E, 4)` on fail |
| 12 | `0x00011D4D` | `cmd_pack_list` | `pack_list` | walks PACK archive at ext-flash `0x80000`, logs name/size rows |
| 13 | `0x00010555` | `cmd_pack_delete` | `pack_delete` | erases 0x180 sectors × 0x1000 from `0x80000` |
| 14 | `0x000116AD` | `cmd_pack_process` | `pack_process` | calls `FUN_00016F2C` (PACK ingest) and logs `Processing pakfs, expect a small …` |
| 15 | `0x00007A59` | `cmd_ble_info` | `ble_info` | reports BLE connections (rider-app flag, timeout, latency, interval, peer addr) |
| 16 | `0x0001B3C5` | `cmd_ble_disconnect` | `ble_disconnect` | `FUN_00021030(0xFFFD, 4)` — force-disconnect all (`0xFFFD` is the TI-stack "all connections" sentinel; reason code 4 = peer-terminated) |
| 17 | `0x0001E8B9` | `cmd_ble_erase_all_bonds` | `ble_erase_all_bonds` | calls `FUN_000265C4(0x20)` — same helper as `oad_status_notify` but with command code 0x20 ("erase bonds"), so that helper is really a general "post control event" dispatcher; OAD's use is just one client |
| 18 | `0x0001DCD1` | `cmd_shutdown` | `shutdown` | `FUN_00026FF4()` (state save) then `FUN_0001D404(0, 0)` (power-down path) |
| 19 | `0x0000F6A1` | `cmd_rtos_statistics` | `rtos_statistics` | every 500 ms loops `FUN_00025208` and logs `total size / total free size / total largest free size` (TI-RTOS heap stats); exits on key-press via 50 ms timer probe |
| 20 | `0x00010079` | `cmd_rtos_nvm_compact` | `rtos_nvm_compact` | logs `Free space before compaction`, calls `FUN_00025904(0)` (SNV compact), logs result |
| 21 | `0x0001D8E5` | `cmd_reset` | `reset` | software reset via `FUN_0001F7F8` (NVIC SYSRESETREQ path) |
| 22 | `0x0001B441` | `cmd_info_ver` | `info`/`ver` | calls `FUN_000054D8` (firmware-info printer) |
| 23 | `0x00014839` | `cmd_exit` | `exit` | logs 5 magic ANSI bytes (0x1B, 0x5B, 0x31, 0x34, 0x7E — Esc [ 1 4 ~ = the F4 keycode) to signal the host terminal to detach |
| 24 | `0x00013BE9` | `cmd_help` | `help` | universal verb-0 walker |

New monitor handlers translated in this pass:

- `cmd_firmware_update` (`0x0001A968`) — command `firmware_update`; on
  execute calls the OAD firmware-update entry (`FUN_0000D444`).
- `cmd_log_dump` (`0x0000C2D4`) — command `log_dump <start_index> <n>`;
  validates the requested range against the current log-block count,
  formats each selected block into a 16-byte stack buffer, logs it, and
  sleeps/yields 4 ticks between rows.
- `cmd_log_inject` (`0x0000E190`) — command `log_inject <n>`; logs the
  current count, allocates 0x18-byte fake log blocks, writes
  `logblock <%d>`, submits each block on channel 0x1D, then frees it.
- `cmd_pack_list` (`0x00011D4C`) — command `pack_list`; opens the PACK
  archive at external-flash base `0x80000`, logs a scan banner, walks
  entries, formats each entry size, and logs name/size rows.
- `cmd_pack_delete` (`0x00010554`) — command `pack_delete`; opens
  external flash, erases 0x180 sectors of 0x1000 bytes starting at
  `0x80000`, logs progress percentage, then logs `Done`.
- `cmd_ble_info` (`0x00007A58`) — command `ble_info`; reports active BLE
  connections out of 3, per-connection rider-app flag, timeout, latency,
  interval and peer address, then logs the local device address.
- `cmd_info_ver` (`0x0001B440`) — command row `info/ver`; execute matches
  either `info` or `ver` and calls the firmware-info printer at
  `FUN_000054D8`.

**All 25 command table entries are now identified AND translated to C** (this pass: 16 new handler names + the 16 new C bodies). Translation files:

- `src/monitor/cmd_log.c` — added `cmd_log_count`, `cmd_log_flush` (4 commands total now)
- `src/monitor/cmd_audio.c` — **new file**, all 5 commands (`play`, `stop`, `dump`, `upload`, `volume_set_all`)
- `src/monitor/cmd_packfs.c` — added `cmd_pack_upload`, `cmd_pack_process` (4 commands total now)
- `src/monitor/cmd_ble.c` — added `cmd_ble_disconnect`, `cmd_ble_erase_all_bonds` (3 commands total now)
- `src/monitor/cmd_os.c` — **new file**, all 4 commands (`rtos_statistics`, `rtos_nvm_compact`, `shutdown`, `reset`)
- `src/monitor/cmd_exit.c` — **new file**, 1 command
- `src/monitor/table.c` — full 25-entry registry in OEM order

Source-file mapping inferred from `monitor_log` path strings:

| Source file | Handlers |
| --- | --- |
| `source/monitor/cmd_help.c` | `cmd_help` |
| `source/monitor/cmd_log.c` | `cmd_log_count`, `cmd_log_dump`, `cmd_log_inject`, `cmd_log_flush` |
| `source/monitor/cmd_audio.c` | `cmd_audio_play`, `cmd_audio_stop`, `cmd_audio_dump`, `cmd_audio_upload`, `cmd_audio_volume_set_all` |
| `source/monitor/cmd_packfs.c` | `cmd_pack_list`, `cmd_pack_delete`, `cmd_pack_upload`, `cmd_pack_process` |
| `source/monitor/cmd_ble.c` | `cmd_ble_info`, `cmd_ble_disconnect`, `cmd_ble_erase_all_bonds` |
| `source/monitor/cmd_os.c` | `cmd_rtos_statistics`, `cmd_rtos_nvm_compact`, `cmd_shutdown`, `cmd_reset` |
| `source/monitor/cmd_exit.c` | `cmd_exit` |
| `source/monitor/cmd_extflash.c` | `cmd_extflash_verify` |
| `source/monitor/cmd_firmware.c` | `cmd_firmware_update` |
| `source/monitor/cmd_version.c` | `cmd_info_ver` |

Operational notes from the new handlers:
- **`audio_upload`** writes its target offset (`0x200000 + index*0x80000`) into the YModem global, so the audio-clip region starts at ext-flash `0x200000` with **0x80000 (512 KiB) per slot**, mirroring the OAD slot stride. Max index 0x7A = 122, so up to 123 clips. Forwards `module_forward_async(0x5571, index)` to motorware after the YModem completes — that's the cue for the motor MCU to commit/play the new clip.
- **`pack_upload`** YModem-receives 1.5 MiB (`0x180000` bytes) into the PACK region at `0x80000..0x200000`. Sends `module_forward_async(0x10E, 0)` to all modules at the start ("PACK upload begin") and `(0x10E, 4)` on failure ("PACK upload abort"). Modbus cmd id `0x10E` is the bike-wide pack-coordination channel.
- **`cmd_ble_erase_all_bonds` calls `FUN_000265C4(0x20)`.** This is the helper I earlier named `oad_status_notify` — wrong. It's actually a **general "post control event"** dispatcher that takes a 1-byte control code; OAD just happens to be one client (its codes were 0x12/0x13/0x14/0x16/0x17). 0x20 is the "erase bonds" code, handled elsewhere in the BLE-stack thread. Need to rename it to something like `bleware_control_event_post`.
- **`cmd_ble_disconnect`** calls `FUN_00021030(0xFFFD, 4)`. `0xFFFD` is the TI BLE-stack "all connections" sentinel (`LINKDB_CONNHANDLE_ALL`); reason code 4 = HCI "remote user terminated".
- **`cmd_exit`** emits the ANSI keycode for F4 (`Esc [ 1 4 ~`) — this is what the OEM terminal driver listens for to detach the debug console. Not a real shell exit.

Two more function pointers (`0x0001E311`, `0x00019A81`) follow the table's null terminator — these are **not** monitor commands. They're the `pfnReadAttrCB` / `pfnWriteAttrCB` fields of a `gattServiceCBs_t` struct registered with TI's `GATTServApp_RegisterService` for service `0x5560`. **Decoded** in `src/gatt_svc_5560.c`:

| OEM | Name | Role |
| --- | --- | --- |
| `0x0001E310` | `svc_5560_read_attr_cb` | TI-stack ReadAttrCB; resolves attr → char-idx via `svc_5560_char_uuid_to_index`, then delegates to the central read dispatcher via vtable offset +8 |
| `0x00019A80` | `svc_5560_write_attr_cb` | TI-stack WriteAttrCB; CCCD writes (attr type-tag `0x02`, desc UUID `0x2902`) → CCCD path (vtable +4) + `cccd_write_validate`; everything else → normal write dispatcher (vtable +0) |
| `0x00012FA8` | `svc_5560_char_uuid_to_index` | 9-entry 128-bit UUID table comparator at flash `DAT_0001309C + 0xC..+0x8C` (stride 0x10); returns 0..8 or 0xFF |
| `0x0001F9AE` | `cccd_write_validate` | rejects unsupported notify/indicate bits (returns 0x80), commits new CCCD value via `cccd_write_store` when changed |
| `0x00026D94` | `cccd_read` | per-conn CCCD value getter |
| `0x00024D90` | `cccd_write_store` | per-conn CCCD value setter |

**Central-dispatch vtable struct at RAM `0x20003F84`** (`*DAT_0001E370 == *DAT_00019B0C` — both shims dereference the same global):

| Offset | Slot | Function |
| --- | --- | --- |
| +0x00 | normal-char write | `xs3_gatt_process_write_event` (or a thin wrapper) |
| +0x04 | CCCD write | CCCD dispatcher (wraps `cccd_write_validate`) |
| +0x08 | read | `xs3_gatt_process_read_event` (or a thin wrapper) |

**Architectural correction.** Every service has its own such shim pair — they all delegate to the central dispatchers via a per-service vtable. Earlier passes implied the TI stack called the central dispatcher directly via the registry; that's wrong. The actual flow is TI-stack → per-service shim → vtable → central dispatcher → per-(svc, idx) handler.

#### All 9 non-backoffice shim pairs (located by `movw r2, #0x55XX` byte-pattern search)

| svc | write shim | read shim | char-uuid matcher | vtable literal |
| --- | --- | --- | --- | --- |
| 0x5510 | `svc_5510_write_attr_cb` @ `0x00019840` | `svc_5510_read_attr_cb` @ `0x0001E11C` | `FUN_0001D744` | `DAT_000198CC` → RAM `0x20005188` |
| 0x5520 | `svc_5520_write_attr_cb` @ `0x000192A0` | `svc_5520_read_attr_cb` @ `0x0001DD98` | `FUN_0001AB6C` | `DAT_0001932C` → RAM `0x20005188` |
| 0x5530 | `svc_5530_write_attr_cb` @ `0x00019720` | `svc_5530_read_attr_cb` @ `0x0001E054` | — | — |
| 0x5540 | `svc_5540_write_attr_cb` @ `0x000194E0` | `svc_5540_read_attr_cb` @ `0x0001DF28` | — | — |
| 0x5560 | `svc_5560_write_attr_cb` @ `0x00019A80` | `svc_5560_read_attr_cb` @ `0x0001E310` | `svc_5560_char_uuid_to_index` @ `0x00012FA8` | RAM `0x20003F84` |
| 0x5570 | `svc_5570_write_attr_cb` @ `0x000199F0` | `svc_5570_read_attr_cb` @ `0x0001E1E4` | — | — |
| 0x5590 | `svc_5590_write_attr_cb` @ `0x000197B0` | `svc_5590_read_attr_cb` @ `0x0001E0B8` | — | — |
| 0x55A0 | `svc_55a0_write_attr_cb` @ `0x00019330` | `svc_55a0_read_attr_cb` @ `0x0001DDFC` | `FUN_0001ABEC` | `DAT_000193BC` → RAM `0x20005188` |
| 0x55C0 | `svc_55c0_write_attr_cb` @ `0x00019690` | `svc_55c0_read_attr_cb` @ `0x0001DFF0` | — | — |

Every shim is structurally identical to the `svc_5560` pair already decoded in `src/gatt_svc_5560.c` — 140 B write, 94 B read, only three things vary per service:
1. the hardcoded svc UUID literal,
2. the per-service char-UUID-to-index function (different table layouts, different number of chars),
3. the vtable pointer (sampled: services 0x5510/0x5520/0x55A0 share vtable at RAM `0x20005188`; svc 0x5560 uses RAM `0x20003F84` — so there are at least **two** distinct vtables; the other services need spot-checking to see which they bind to).

**Backoffice service `0x5500` is the only outlier** — it does NOT have a shim of this shape. Its writes go through the central write dispatcher's special-case branch (`cVar1 == 4` → `gatt_handle_backoffice_message_data`); its reads use the central read dispatcher's special-case `svc=0x5500, idx=0` producer.

**Total registry surface:** 9 mirror-image shim pairs + 1 backoffice = **10 services**, not 11 as the earlier registry note implied. The earlier "11" was inferred from the per-service table size; the actual count is 10, with the 11th slot likely being a null terminator.

**`monitor_dispatch_loop` @ `0x00024B38`** — **Decoded** —
`src/monitor/dispatcher.c`. 36 B body + 4 B literal pool. Walks
the static command table at `0x0002A0BC`, calling each handler
with `verb=2` and `p2 = user-input string`. Returns 1 if any
handler returned 0 (matched & executed), or 0 if the table is
exhausted / empty. GCC produces the same total size as the OEM
CGT/armcc build (36 B body); only register allocation differs
(OEM uses an explicit `r5` return-value flag, GCC uses literal
`movs r0, #0` / `movs r0, #1` exits).

The table head at `0x0002A0BC` resolves to addresses like
`0x0001A969`, `0x0000ABD9`, `0x0001699D`, `0x0000C2D5`, `0x00016DCD`,
etc. — Thumb-set function pointers, one per cmd module.

### Monitor dispatcher — still elusive

The main monitor dispatcher (the function that reads user input,
parses the command name, looks it up, and calls the handler) was
not pinned down this turn. Two structural reasons:

- **CGT/armcc indirect references**. Bleware is compiled with TI's
  ARM Compiler, which uses MOVW/MOVT instruction pairs for 32-bit
  constants rather than literal-pool loads. Ghidra's auto-analysis
  doesn't always trace those into proper xrefs, so string-to-
  function references go invisible.
- **Help-table is a NULL-terminated function-pointer list** — not
  the same structure as the runtime command-lookup table the
  dispatcher walks. The user-facing names (`"play_audio_file"`,
  etc., stored as `"cmd_*"` symbol names) are referenced by
  another table or by direct argument passing into a registration
  function.

Two viable next paths:

1. **Find the monitor task creation.** Just as the bluetoothtask is
   created by `create_bluetoothtask` @ `0x000235D0`, there should be
   a `create_monitor_task` somewhere — probably called from inside
   `tirtos_modules_init` (`0x000215FC`) or one of its 13 sub-init
   helpers. Walking that chain would identify the task body, and
   the body's first action is the dispatcher loop.
2. **Trace `cmd_exit` (`0x0002BA58`)** — it's a single-name string,
   probably referenced by one small registration function that
   we can xref-to-find the registry. Tracing the registry gives
   the dispatcher.

### Audio task

A second VanMoof task lives in `source/tasks/audiotask.c` (string at
`0x0000B2E0`). The constructor isn't called directly from `main` —
it's created from somewhere inside the module-init chain. To
identify in a follow-up pass; the audiotask is the BLE-audio-streaming
companion to the bluetoothtask.

### `secrets.c` — external-SPI-flash secrets store @ flash `0x005A000`

**Decoded** — `src/secrets.c`. Two functions cover the entire
CRC-protected 128-record table at external-flash offset `0x005A000`:

- **`secrets_record_read` @ `0x00020BB8`** — bounds-checks `index` to
  `[0, 127]`, reads 32 B at `0x5A000 + index*0x20` via `extflash_read`,
  CRC-32s the first 28 B and compares against the stored CRC at offset
  `0x1C`. On success and a non-NULL `out_record`, copies all 32 B to
  the caller's buffer. Returns 1 on valid, 0 otherwise. GCC builds to
  78 B vs OEM 90 B (the OEM's compiler emits an explicit 8×word copy
  for the success-path memcpy; GCC inlines the same 32 B copy more
  tightly).

- **`secrets_record_write_verify` @ `0x00020C06`** — writes a 32 B
  record via `extflash_sector_write` (read-modify-write at the 4 KB
  sector boundary), reads back, `memcmp`-verifies. Up to 4 retries
  on mismatch. **Notably does NOT bounds-check `index`** — preserved
  as an OEM quirk; treat any "fix" as a potential ABI break. GCC
  builds to 70 B, ~matching OEM.

Helpers identified along the way (renamed in Ghidra this turn):

| Address | Name | Role |
| --- | --- | --- |
| `0x0001C5A4` | `extflash_read` | semaphore-serialised external-flash read (uses TI-RTOS `Semaphore_pend/post` thunks at `0x1002BFB0` / `0x1002CD20`) |
| `0x000193C0` | `extflash_sector_write` | 4 KB-sector read-modify-write (mask `0xFFFFF000` from `DAT_0001944C`); handles cross-sector spans via a loop |
| `0x00025198` | `crc32_le` | CRC-32 reflected, polynomial `0xEDB88320` (zlib/Ethernet/PNG), no final XOR. Seed-as-arg lets the caller compute the standard ZLIB CRC by passing `0xFFFFFFFF` and XORing the result with `0xFFFFFFFF` afterwards — but the secrets store stores and compares the *raw* (non-final-XORed) value, so callers don't bother with that XOR. |
| `0x00026534` | `crc32_step_byte` | inner shift-XOR helper (no table; computed on the fly via 8 iterations against `DAT_00026548 = 0xEDB88320`) |
| `0x00025490` | `memcmp` | standard byte-by-byte memcmp |
| `0x000251BE` | `monitor_command_matches` | (also renamed this turn; previously `monitor_streq`, originally mis-labeled `nvs_open` by Ghidra's auto-analysis — see ABI-history note above) |

Known slot assignments (cross-checked with `VanMooof-Module` Go tool):

| Slot | Sector offset | Flash address | Length | Field |
| --- | --- | --- | --- | --- |
| 0     | `0x000` | `0x005A000` | 16 B | BLE Authentication Key (first 16 B of payload) |
| 0..123 | varies | varies     | 32 B | User-keyed records: `payload[+16]` = uint32 key, `payload[+24]` = ASCII tag (e.g. `"UKEY"`); written via `secrets_upsert_keyed_record` |
| 124   | `0xF80` | `0x005AF80` | 32 B | **M-ID directory record** — CRC-protected, tag `"M-ID"` at `payload[+24]`, `payload[+16]` = unknown uint32 (counter / cursor?) |
| 125   | `0xFA0` | `0x005AFA0` | 32 B | likely M-ID/M-KEY continuation (Go tool reads 60 B at `0x5AF80` spanning slots 124+125 as raw bytes — the firmware's CRC-API view may differ from the Go tool's view) |
| 126   | `0xFC0` | `0x005AFC0` | 16 B | Manufacturing Key (first 16 B of payload) |

The 60-byte "M-ID/M-KEY" view used by `VanMooof-Module`'s `ReadSecrets`
slams slots 124+125 as raw bytes. The firmware itself treats slot 124
as a normal CRC-protected record (see `secrets_ensure_mid_record`).
Slot 125's framing is still TBD — likely also CRC-protected per
record, but no decoded code path touches it yet.

Keyed-record API (decoded this turn — same `src/secrets.c`):

| Address | Name | Role |
| --- | --- | --- |
| `0x00022BAA` | `secrets_find_by_key` | linear scan of slots 0..123 for `payload[+16] == key`; copies the matching 32 B record on hit, returns slot index or `-1` |
| `0x00026034` | `secrets_count_free_slots` | counts slots 0..123 whose CRC fails — the slots an upsert can claim |
| `0x0001CA68` | `secrets_upsert_keyed_record` | given a 24 B caller payload, looks up by key (`payload[+16]`); on match overwrites that slot, else fills the first free slot. Stamps the `"UKEY"` tag at `payload[+24]`, recomputes CRC, writes via `secrets_record_write_verify`. Returns 0 on "no room" |
| `0x0001F0BE` | `secrets_upsert_keyed_batch` | upserts an array of N 32 B records: counts (matches + free) up front; refuses with `-1` if it would over-fill the keyed range; returns `-2` on any per-record failure, `0` on full success |
| `0x00021328` | `secrets_ensure_mid_record` | reads slot 124; if CRC valid returns `*(uint32*)(payload+16)`; if invalid initialises the slot as `{0…0, tag="M-ID", CRC}` and returns 0. Effectively a "boot-time initialise the M-ID directory if it's missing" call. Caller at `0x0000419E` is not yet inside a recognised function — likely from an inline raw-call site near `_c_int00` |

**Tag values** observed at payload offset `+24`:

| Tag (LE uint32) | ASCII | Where written |
| --- | --- | --- |
| `0x44492D4D` | `"M-ID"` | `secrets_ensure_mid_record` → slot 124 |
| `0x59454B55` | `"UKEY"` | `secrets_upsert_keyed_record` → any free slot in 0..123 |

The keyed-record table is **content-addressed** — there is no separate
index/directory; lookups linear-scan the 124 slots. Free slots are
identified by *CRC failure*, not by a marker byte — so an erased
sector (`0xFF`-fill) naturally appears as 124 free slots.

**Search note**: the base address `0x0005A000` is encoded in Thumb-2
as `add.w r0, r0, #0x5a000` modified-immediates (e.g. at `0x00020BD2`,
`0x00020C0E`), *not* as a 32-bit literal pool entry. Byte-pattern
searches for `00 A0 05 00` will return zero hits. Future SPI-flash
address discovery needs to walk MOVW/MOVT pairs and `add.w` immediates.

### Functions renamed in Ghidra this turn

| Address | Name | Notes |
| --- | --- | --- |
| `0x00026474` | (still `main` in Ghidra) | should be `main_trampoline`; the Ghidra body fuses both functions because of the `b.w` tail call |
| `0x0001CFEC` | `main` (Ghidra's auto-tagged) | the real main body |
| `0x000215FC` | `tirtos_modules_init` | calls 13 sub-init functions |
| `0x000235D0` | `create_bluetoothtask` | TI-RTOS Task_construct for the BLE task |
| `0x000067C8` | `bluetoothtask_main` | the BLE event-loop body |
| `0x00013BE8` | `cmd_help` | re-created at the correct entry (Ghidra had it at `0x00013C20`, missing the 24 B verb-dispatch preamble) |
| `0x000251BE` | `monitor_command_matches` | was `nvs_open` (wrong) → `monitor_streq` (this session, transient) → `monitor_command_matches` (canonical, matches the project source) |
| `0x00020BB8` | `secrets_record_read` | with prototype `int(int, void*)` |
| `0x00020C06` | `secrets_record_write_verify` | with prototype `int(int, void*)` |
| `0x0001C5A4` | `extflash_read` | external-flash read with internal TI-RTOS semaphore lock |
| `0x000193C0` | `extflash_sector_write` | 4 KB-sector RMW writer; loops across cross-sector spans |
| `0x00025198` | `crc32_le` | reflected CRC-32 with seed arg; no final XOR |
| `0x00026534` | `crc32_step_byte` | shift-XOR helper for one byte (8 iterations against `0xEDB88320`) |
| `0x00025490` | `memcmp` | byte-by-byte memcmp |
| `0x00022BAA` | `secrets_find_by_key` | with prototype `int(uint32_t, void*)` |
| `0x00026034` | `secrets_count_free_slots` | with prototype `int(void)` |
| `0x0001CA68` | `secrets_upsert_keyed_record` | with prototype `int(void*)` |
| `0x0001F0BE` | `secrets_upsert_keyed_batch` | with prototype `int(void*, unsigned int)` |
| `0x00021328` | `secrets_ensure_mid_record` | with prototype `unsigned int(void)` |
| `0x00019570` | `manufacturing_key_get_or_init_default` | reads slot 126 (M-Key); falls back to in-RAM key derived from BLE MAC + "MOOF"/"MKEY" tags. **Authoritative reference to `secrets_record_read(0x7E, ...)`** — earlier "no slot 126 reader" finding was wrong; the function was undefined at search time. |
| `0x00003E78` | `gatt_handle_backoffice_message_data` | GATT-backoffice fragment handler — decrypts each 16-byte AES block with the M-Key (via ROM AES jump table at `_DAT_100001FC + 0x20`), reassembles into a 0x90-byte plaintext, CRC-16/Modbus-validates, then dispatches one of 9 sub-commands (record upsert / M-Key write / delete-by-key / no-op / module-forward async + sync / secrets sector erase / bulk record import / reserved). Reply notified on GATT channel 4 (0xF0 bytes). **OEM name confirmed via embedded symbol string at `0x0002B4B4`.** Source file: `source/xs3_gatt_backoffice.c` (path string at flash `0x00004140`). |
| `0x00020848` | `block_dispatch_queue_post` | TI-RTOS-style queue post: packages `(key_buf, block_len, src, src_alias)` into a message and hands it to `FUN_000275E8` → `FUN_000125C4` → ROM jump table |
| `0x00026504` | `byte_to_hex_chars` | writes 2 ASCII hex chars from a byte; hex table at flash `0x0002B46C` = `"0123456789ABCDEF"` |

### `provisioning.c` — backoffice GATT message handler (`xs3_gatt_backoffice.c`)

**Decoded** — `src/provisioning.c`. Two functions land here.

- **`manufacturing_key_get_or_init_default` @ `0x00019570`** — fetches slot 126. On read-failure (factory-fresh device) it materialises a deterministic per-device fallback **in RAM only**: 12 ASCII hex chars of the chip's BLE MAC (read from FCFG1 at `0x500012E8`), `"MOOF"` tag at payload offset +12, `"MKEY"` tag at offset +24, CRC at +28. The fallback is *not* written back to flash — only cached for the current boot, so a factory tool can still detect the unprogrammed state and run the full provisioning flow.

- **`gatt_handle_backoffice_message_data` @ `0x00003E78`** — the GATT-backoffice characteristic's write callback. Each call carries one fragment of a longer message; framing is `{flag[0], cursor[1], ciphertext[2..]}` where the ciphertext length is a non-zero multiple of 16. Every 16-byte block is queued for in-place AES decryption (key = M-Key, via `block_dispatch_queue_post` → CryptoCC26X2 ROM jump table at `_DAT_100001FC + 0x20`). The decrypted bytes are accumulated at `g_backoffice_reassembly + 0x10` starting at the fragment's cursor; total reassembly limited to 0x90 bytes. Only the fragment with `flag == 0x01` triggers dispatch.

  On the final fragment the function checks the BLE connection is still active (`ble_connection_is_active`), validates the assembled payload's trailing CRC-16/Modbus (poly `0xA001`, seed `0xFFFF`), and dispatches on the 16-bit BE sub-command at `reasm[0x14..0x15]`:

  | Sub-cmd | Action |
  | --- | --- |
  | 1 | `secrets_upsert_keyed_record(rec24)` — payload@+0x17 (16 B), key@+0x27 (4 B), tag@+0x2B (4 B) |
  | 2 | `mkey_record_write_slot126(rec24)` (FUN_000222AC) — writes slot 126 with tag forced to `"MKEY"` (stored at flash `0x000222E8`) and a fresh CRC |
  | 3 | `secrets_delete_by_key(key_be32)` (FUN_00024D14) — looks up by key and overwrites the matching slot with 0xFF |
  | 4 | `backoffice_ack_noop()` (FUN_0002774A) — always returns 1; heartbeat / capability probe |
  | 5 | `module_forward_async(0x5562, byte)` (FUN_00024508) — forwards one byte to inter-module bus target 0x5562 |
  | 6 | `secrets_sector_erase()` (FUN_00026C30) — erases the 4 KB secrets sector at ext-flash `0x005A000` (factory reset) |
  | 7 | Bulk import — N = `(payload_len-9)/0x18` source records of 24 B each, re-laid into 0x20-byte slots and bulk-upserted via `secrets_upsert_keyed_batch` |
  | 8 | `module_forward_sync(u16_cmd, ptr, len)` (FUN_000177E8) — synchronous request/reply over the inter-module bus with ~2.5 s timeout; reply status mapped into the backoffice enum |
  | 9, 10 | reserved — always return status 5 |
  | other | unknown sub-cmd — status 3 |

  Reply is 0xF0 bytes notified on GATT channel 4 (`gatt_notify_channel`): the first 9 bytes carry `{context_be[0..3], M-ID[4..7], status[8]}`; the remaining 0xE7 bytes are whatever was last in the reassembly buffer (intentional debug echo for the service tool). Quirk preserved verbatim: the BE serialisation of the context word and the M-ID uses `>> 18` for the second byte rather than `>> 16` — a 2-bit bleed that the receiver is built to match.

**Why no AES S-box in bleware:** AES lives in CC2642R1F ROM. App-side calls are pure function-pointer indirect-calls through ROM jump tables (e.g., `_DAT_100001FC[+0x20]` in the bleware base pointer table). This explains the earlier dead-end byte-pattern search. Any future crypto investigation on CC26X2 SimpleLink images should chase ROM jump tables, not S-box bytes.

**No direct xref to `gatt_handle_backoffice_message_data` in the binary.** The address `0x00003E78` (or `0x3E79` Thumb-mode) does not appear as a 32-bit literal anywhere. The wiring comes via the GATT-config item table (see next section) and a numeric callback index, not a literal function-pointer pool.

### `xs3_gatt_config.c` — service & characteristic registry

GATT services and characteristics are described by two static tables in flash. The **service table** at `0x0002A2F8` holds 11 × 8-byte entries (one per BLE service):

```
struct gatt_service_entry {
    uint16_t service_short_id;   // low half of UUID base (e.g. 0x5500)
    uint16_t item_count;         // # of characteristics under this service
    struct gatt_item_entry *items; // -> 28-byte item array
};
```

| Idx | svc id | items | items_ptr |
| --- | --- | --- | --- |
|  0 | `0x5500` |  5 | `0x0002A1C0` **← backoffice** |
|  1 | `0x5510` |  3 | `0x0002AAEC` |
|  2 | `0x5520` |  4 | `0x0002A634` |
|  3 | `0x5530` | 11 | `0x000298CC` |
|  4 | `0x5540` | 18 | `0x000296D4` |
|  5 | `0x5560` |  9 | `0x00029D30` |
|  6 | `0x5570` |  4 | `0x0002A784` |
|  7 | `0x5580` |  4 | `0x0002A714` |
|  8 | `0x5590` |  2 | `0x0002B18C` |
|  9 | `0x55A0` |  4 | `0x0002A6A4` |
| 10 | `0x55C0` |  3 | `0x0002AA98` |

These are the *short forms* of the 128-bit UUIDs against the VanMoof base `6ACC0000-E631-4069-944D-B8CA7598AD50` — each populates bytes [2..3] of the UUID. So service `0x5500` is `6ACC5500-E631-4069-944D-B8CA7598AD50`.

Per-item entries are 28 bytes:

```
struct gatt_item_entry {
    uint16_t char_short_id;  // e.g. 0x5505
    uint16_t flags_a;
    uint32_t value_size;     // bytes
    uint32_t value_ram_ptr;  // 0 = no static storage (callback-only)
    uint32_t props_perms;    // composite property/permission word
    uint32_t reserved[2];
    uint32_t callback_pair;  // 0xFF = no callback; else (write_id, read_id)
};
```

#### Backoffice service `0x5500` — characteristics

| Char id | UUID | size | RAM buf | role |
| --- | --- | --- | --- | --- |
| `0x5501` | `6ACC5501-E631-4069-944D-B8CA7598AD50` | 16 | `0x2000ACE0` | notify-only — channel 0 |
| `0x5502` | `6ACC5502-E631-4069-944D-B8CA7598AD50` | 20 | *(none)* | **AUTH handshake** — derives 16-byte session key from 4-byte seed at payload[+0x18..+0x1B] and stores it on the connection; then runs `backoffice_auth_session_init` (`0x0001A218`) |
| `0x5503` | `6ACC5503-E631-4069-944D-B8CA7598AD50` | 16 | `0x2000ACC0` | notify channel 2; writes forwarded as `FUN_00021884(0x5503, u24)` |
| `0x5504` | `6ACC5504-E631-4069-944D-B8CA7598AD50` | 16 | `0x2000ACD0` | notify channel 3 (no write path in dispatcher) |
| `0x5505` | `6ACC5505-E631-4069-944D-B8CA7598AD50` | 240 | `0x20009F24` | **BACKOFFICE REQUEST + RESPONSE — `gatt_handle_backoffice_message_data` fires on write; replies notify on the same characteristic via `gatt_notify_channel` channel 4** |

Cross-check: the channel-4 buffer pointer baked into `gatt_notify_channel` (`DAT_0001B59C = 0x20009F24`) matches `0x5505`'s `value_ram_ptr`, and the notify size `0xF0` matches its `value_size`. Channels 0/2/3 line up with `0x5501/0x5503/0x5504` the same way.

The dispatcher routes writes by characteristic *index within the service* (`event[+6]`), not by short UUID. For svc `0x5500` indices 0..4 map to chars `0x5501..0x5505`; the dispatcher only handles indices 1 (auth), 2 (forward), and 4 (backoffice). Indices 0 and 3 are notify-only.

#### Helpers in `xs3_gatt_config.c`

| Addr | Name | Role |
| --- | --- | --- |
| `0x00011228` | `gatt_config_lookup_item` | `(svc_id, item_idx) -> *gatt_item_entry` — linear scan over the service table. Source path: `source/xs3_gatt_config.c`. |
| `0x0001508A` | `gatt_service_notify_dispatch` | `(svc_id, channel, buf, len, flag)` — internal app-side notify-publish dispatcher. For svc `0x5500` routes to `gatt_notify_channel` (4-way sub-channel by `channel`); each other svc id has a dedicated per-service notify helper. |

#### Bench-test target

For backoffice provisioning the same characteristic carries both directions:

- **Write (request) and Notify (response):** `6ACC5505-E631-4069-944D-B8CA7598AD50` (subscribe to notifications first, then write fragments)
- **Auth handshake required first:** write a 20-byte challenge to `6ACC5502-E631-4069-944D-B8CA7598AD50` — derives a session key on the connection that gates the other 0x55XX characteristics' write paths (each one calls `ble_connection_get_session_key` and rejects with `-1` if no key is set when the auth-required flag at `FUN_00025A84()` is set).

### `protocols/ssp.c` — System Service Protocol transport

**Decoded** — `bleware/src/protocols/ssp.c`. The inter-module bus the OEM calls "Modbus" in my earlier notes is actually **SSP** — confirmed via embedded `monitor_log` strings:
- `"SSP no init"` at flash `0x00015DCC`-ish (logged when the master queue handle is NULL)
- `"SSP Out of memory"` (logged when frame alloc fails)
- Source path: `source/protocols/ssp.c`

**Frame structure** (header = 0x27 B + payload up to 0x100 B):

| Offset | Field | Notes |
| --- | --- | --- |
| +0x0C | u32 ctx1 | caller context — usually 0 |
| +0x10 | u32 ctx2 | caller context — usually 0 |
| +0x14 | u32 timeout | always 5 |
| +0x18 | u32 retry | always 100 |
| +0x20 | u8 frame_type | always 2 (PUBLISH/FETCH share this) |
| +0x21 | u8 priority | 7 = publish, 6 = fetch (RX-urgency higher) |
| +0x22 | u8 sequence | auto-incremented from `g_ssp_sequence` |
| +0x23 | u16 cmd_id | the SSP command/opcode |
| +0x25 | u16 payload_len | 0 for FETCH |
| +0x27 | u8[len] payload | only on PUBLISH |

**Three primitives** (now real C in `ssp.c`, no longer weak stubs):

| OEM | Name | Behaviour |
| --- | --- | --- |
| `0x00024508` | `module_forward_async(cmd_id, byte)` | wraps 1 B in a publish frame, enqueues |
| `0x000244D8` | `module_publish_command(cmd, payload, len)` | N B publish |
| `0x0001EEF4` | `module_publish_sync_with_timeout(module_idx, cmd, ms)` | sends FETCH via `ssp_signal_fetch`, pends per-module reply semaphore for `ms * 100` µs ticks. If a prior fetch timed out (stuck-flag bit at record+0x79), this call's timeout is bumped to 500 ms and the flag is cleared (one-shot grace). |

**Internal helpers** also decoded:

| OEM | Name | Role |
| --- | --- | --- |
| `0x000180D0` | `ssp_queue_publish_frame` | core PUBLISH enqueue (header build + sequence + alloc + mailbox post) |
| `0x00015D24` | `ssp_publish_fetch_frame` | core FETCH enqueue (smaller header, priority 6, no payload, log paths) |
| `0x00025B04` | `ssp_signal_fetch` | LED-pulse wrapper around `ssp_publish_fetch_frame` |
| `0x00025A84` | `ble_authenticated_connection_count` | iterates 3 conn slots, counts how many have a session key — gates the activity-LED pulse on every TX |
| `0x00027004` | `ble_activity_led_pulse` | drives DIO 0xD high via the IOC GPIO writer (`FUN_00022E08`) |

**Global state**:

| RAM addr | Symbol | Role |
| --- | --- | --- |
| `0x20003104` | `g_ssp_master` | singleton SSP master struct (+0x00 tx_queue, +0x20 mailbox); same address held by 3 flash literals (`DAT_00024504`, `DAT_00024534`, `DAT_00025B20`) |
| `0x20004158` | `g_ssp_modules[]` | per-module reply-state array, 0x7C B per module: +0x44 reply semaphore, +0x54 u16 pending cmd (0xFFFF = idle), +0x79 u8 stuck-flag |
| (RAM, via `*DAT_0001816C`) | `g_ssp_sequence` | 1-byte counter, post-incremented on every enqueue |

**Behavioural notes**:
- Every PUBLISH/FETCH consults `ble_authenticated_connection_count()` and pulses the LED on DIO 0xD if any BLE connection has completed the session-key handshake. Useful for triage: the LED tells you whether any traffic on the bus is BLE-initiated.
- The "Modbus" framing label I'd been using everywhere should now be read as "SSP" — same on-the-wire idea (cmd_id + payload + sequence + reply correlation), but it's the OEM's own protocol, not a CRC-16/Modbus RTU clone. The CRC-16/Modbus we observed is used elsewhere (backoffice payload validation), not for SSP framing.
- The "module_idx" parameter to the sync primitive maps to a slot in `g_ssp_modules[]`. Each SSP slave (motorware, mainware, etc.) has its own record; conn_handle is reused as module_idx by the GATT read dispatcher in a slightly confusing overload.

**Decoded** — `xs3_gatt_process_write_event` @ `0x00004DB0` (868 B body; previously undefined in Ghidra's auto-analysis). Event struct layout from the dispatcher:

```
event[+2 : u16]   connection handle
event[+4 : u16]   service short id (0x5500..0x55C0)
event[+6 : u8]    characteristic index within the service (0-based)
event[+8 : u16]   payload length
event[+10..]      payload bytes
```

The dispatcher's flow:

1. `gatt_config_lookup_item(svc, idx)` — fetches the 28-byte item entry from `xs3_gatt_config.c`'s tables.
2. **Special path for char `0x5502` with len 0x14:** `auth_derive_session_key(seed_u32)` (`0x00018B1C`) produces a 16-byte key; if the connection already has a session key (`ble_connection_get_session_key`) and `FUN_00025A84() > 0` (auth-required flag), a mismatch returns `-1`. Otherwise the derived key is stored via `ble_connection_set_session_key`.
3. Length range check against item's value-size band (`psVar4[1]` low, `psVar4[2]` high).
4. **Property byte `psVar4[13]` decoded as a 4-bit composite**:
   - bit 2 set → input is N × 16-byte AES blocks; submit each via `block_dispatch_queue_post(session_key, 16, src, dst)` (decrypt-in-place) and `memcpy(payload, decrypted, len & ~0xF)` — the same crypto path used by the backoffice handler. Requires a session key, fails `-1` if absent.
   - bit 1 set → `FUN_00024740(scratch, payload, len)` — alternative scrambling/decrypt helper.
   - bit 3 set → expects a 2-byte sequence-number prefix; compares against per-connection counter (`FUN_00022970`), bumps via `FUN_00023114`, strips the prefix and shifts `psVar11` by 2.
   - bit 0 set → mandatory session-key check; fails `5` if absent.
5. If `item->value_ram_ptr != 0` → memcpy the payload there.
6. CRC check (`FUN_00026050`) against `item->cb_lo` mask — mismatch → status 2, reply via `FUN_00021030(conn, status)`.
7. Optional trailing zero-padding check using `item->cb_hi`.
8. **Per-service write dispatch** — switch on `svc_id`, then on `char_idx`.

#### Per-service write callback map

| Svc | Idx (char id) | Action |
| --- | --- | --- |
| `0x5500` | 1 (`0x5502`) | `backoffice_auth_session_init(conn, derived_key)` — completes the auth handshake on the connection |
| `0x5500` | 2 (`0x5503`) | `FUN_00021884(0x5503, u24_from_payload[0..2])` — publish a 24-bit value on svc-id `0x5503` |
| `0x5500` | 4 (`0x5505`) | **`gatt_handle_backoffice_message_data(conn, payload, len)`** |
| `0x5510` | 0 / 1 / 2 | `oad_gatt_write_handler(conn, idx, payload, len)` — OAD update handler (see `oad.c` section); idx 0 = metadata, 1 = payload chunk, 2 = no-op |
| `0x5520` | 0, 2, 3 | `module_forward_async(svc + idx + 1, payload[0])` — forward 1-byte command on the internal Modbus bus |
| `0x5530` | 2, 4, 5, 7 | `module_forward_async(0x5530 + idx + 1, payload[0])` — chars `0x5533/5535/5536/5538` map 1:1 to Modbus cmd ids |
| `0x5530` | 3 | `module_publish_command(0x5534, payload, 2)` (`0x000244D8`) — typed publish |
| `0x5530` | 6 | `module_publish_command(0x5537, payload, 6)` |
| `0x5560` | 1, 3, 4, 5 | `module_forward_async(0x5560 + idx + 1, payload[0])` |
| `0x5560` | 6 | `FUN_00026CC0(u32_be from payload)`; then `FUN_00021884(0x5567, FUN_00027448())` — looks like a setter + read-back publish |
| `0x5570` | 0, 3 | `module_forward_async(0x5571, payload[0])` (both indices route to `0x5571`) |
| `0x5570` | 1 | `module_publish_command(0x5572, payload, 12)` |
| `0x5580` | 0 | `module_forward_async(0x5581, payload[0])` |
| `0x5580` | 1 | `module_publish_command(0x5582, payload, 3)` |
| `0x5580` | 3 | `FUN_00023204(0x5584, u16_at_payload+3)` — same publisher family, 2-byte arg |
| `0x5590` | 0, 1 | `module_publish_command(0x5590 + idx + 1, payload, len)` (chars `0x5591/0x5592`) |
| `0x55A0` | 0..3 | `module_publish_command(0x55A0 + idx + 1, payload, len)` (chars `0x55A1..0x55A4`) |
| `0x55C0` | 0 / 2 | `log_gatt_write_handler(conn, idx, payload, len)` — log-dispatch handler (see `log_gatt.c` section); idx 0 = control, 2 = readout. idx 1 has no write path. |

The clean pattern: GATT characteristic `svc + idx + 1` corresponds directly to **inter-module Modbus command id `svc + idx + 1`**. The dispatcher is effectively a BLE-to-Modbus bridge for most services. Three services break the pattern with bespoke logic worth its own decode pass:

- **`0x5500` backoffice** — auth handshake on `0x5502` (4-byte seed → 16-byte session key) and AES-decrypt + reassembly + CRC + 9-cmd dispatcher on `0x5505`. Decoded in `src/provisioning.c`. The dispatcher routes this via a special-case `char_idx == 4` branch (not a function pointer in the registry), which is why earlier passes framed it as "part of the dispatcher" rather than as its own service.
- **`0x5510` OAD** — dedicated handler `oad_gatt_write_handler` (`src/oad.c`).
- **`0x55C0` log** — dedicated handler `log_gatt_write_handler` (`src/log_gatt.c`).

The eight remaining services (`0x5520`, `0x5530`, `0x5540`, `0x5550`, `0x5560`, `0x5570`, `0x5580`, `0x55B0`) are pure Modbus bridges on writes. A handful have small bespoke **read** producers (≤10 lines) inlined into `gatt_read.c`'s switch (0x5520/1 conn-state, 0x5540/10,16,17 ECC+TRNG, 0x5560/6 RTC) — not enough to warrant separate TUs.

#### Helpers in `xs3_gatt_write.c`

| Addr | Name | Role |
| --- | --- | --- |
| `0x00004DB0` | `xs3_gatt_process_write_event` | central WriteAttrCB dispatcher |
| `0x000244D8` | `module_publish_command` | publish typed multi-byte command on the Modbus bus |
| `0x00018B1C` | `auth_derive_session_key` | 4-byte seed → 16-byte session key |
| `0x00023DCC` | `ble_connection_get_session_key` | per-connection session key fetch |
| `0x000231C8` | `ble_connection_set_session_key` | store derived key for a connection |
| `0x0001A218` | `backoffice_auth_session_init` | post-auth init hook for svc `0x5500` |
| `0x000267A4` | `oad_gatt_write_handler` | **Decoded** — `src/oad.c`. Per-service handler for svc `0x5510`. |
| `0x00014910` | `log_gatt_write_handler` | **Decoded** — `src/log_gatt.c`. Per-service handler for svc `0x55C0`. |

### `xs3_gatt_read.c` — central GATT read dispatcher

**Decoded** — `xs3_gatt_process_read_event` @ `0x000061C0` (680 B body; previously undefined in Ghidra's auto-analysis — same literal-pool-walk trick used to recover the write dispatcher). Lives in `bleware/src/gatt_read.c`. Read-side analogue of `xs3_gatt_process_write_event`; called by the TI BLE-stack ReadAttrCB for every characteristic in the 11-service registry.

**Auth / encryption gating.** Per-char `flags` byte at registry-entry `+0xC`:

| bit | meaning |
| --- | --- |
| 0 | outgoing payload AES-encrypted with the **manufacturing key** (`manufacturing_key_get_or_init_default`) |
| 1 | mfg key must be available; if not → return `2` |
| 2 | session key must be set (cleared by the `0x5502` handshake) AND outgoing payload AES-encrypted with the session key |
| 7 | always-deny |

A runtime "permission mask" (32-bit, source `runtime_permission_mask` @ `0x00026050` — reads `secrets-cache+0x14` after a semaphore-pend) is intersected against the entry's required mask at `+0x10`; any missing bit returns `2`.

**Modbus pre-fetch.** When `att_opcode == 0x0A` (ATT Read Request) AND the entry's `+0x1A` byte is non-zero AND the inter-module bus is idle (`module_bus_is_idle()` @ `0x00026594`), the dispatcher fires `module_publish_sync_with_timeout(conn, svc+idx+1, 150 ms)` (OEM `0x0001EEF4`) — same `svc + idx + 1` command-id convention as the write side. Timeout returns `0xE` (matches OEM `monitor_log("Failed in ssp synchronization u%d", cmd_id)`).

**Per-(svc, char_idx) producers** (anything not listed falls through to the Modbus-shadow copy already populated by the pre-fetch above):

| svc | idx | producer |
| --- | --- | --- |
| `0x5500` | 0 | `backoffice_status_u16(conn, &out16)` — 2 B backoffice status word (OEM `0x00022970`) |
| `0x5520` | 1 | `ble_conn_state_byte(conn, &out_buf[1])` — 1 B connection state at offset 1 (OEM `0x000228B0`) |
| `0x5540` | 10 | `memset(out, 0, 16); ecc_sign_with_factory_key(out, 16, &g_ecc_curve_params, …)` — ECC sign over zeroed challenge with curve params at `0x000064C0` (OEM `0x0002617E` → ROM thunk `0x1002F8B8`) |
| `0x5540` | 16 | same ECC sign at `out + 16`; uses `size_max` (entry `+4`) as length |
| `0x5540` | 17 | `memset(out, 0, 16); trng_fill_16(out)` — TRNG via ROM thunk `0x1002FDDA`, capped to `size_max` |
| `0x5560` | 6 | `timekeeper_read_be()` (OEM `0x00027448` → `0x0001EAF8`) — packs 4 BE bytes from the upper word of an 8-byte timestamp |
| `0x55C0` | 1 | `log_block_count_get()` (OEM `0x00020338`) — 4 BE bytes: number of 16 B log blocks ready to read |
| `0x55C0` | 2 | `((log_total_size_byte() & 0xFFF) << 4)` (OEM `0x000273DC`) — effective total log byte-size |

**Length finalisation.** Clamped to the ATT MTU, and for encrypted chars (`flags & 0x06`) rounded up to a 16 B boundary then trimmed back if it overshoots MTU or `size_max`. Out-of-range `[size_min, size_max]` returns `4`. Zero-length returns `1`.

**Encryption pass.** If `flags & 0x02` (mfg) or `flags & 0x04` (session), allocates a 255 B scratch via `gatt_scratch_alloc` (OEM `thunk_FUN_00013470(0xFF)`), AES-128-ECB encrypts each 16 B block with the relevant key (`aes128_block_encrypt` @ `0x00020898`), then `memcpy`s back. Alloc failure → `0x11`.

**Return-code map:** `0` ok, `1` empty, `2` denied, `4` length out of range, `0xE` Modbus fetch failed, `0x11` scratch alloc failed.

The clean takeaway: this confirms the dispatcher pair `(xs3_gatt_process_write_event, xs3_gatt_process_read_event)` is the complete BLE-side abstraction. Most chars are mechanical BLE↔Modbus bridges in both directions; only seven (svc, idx) pairs have bespoke read producers, and only two services (`0x5510`, `0x55C0`) have full custom write handlers.

### `oad.c` — over-the-air firmware update (`source/oad/oad.c`)

**Decoded** — `src/oad.c`. Three characteristics under svc `0x5510`:

| Char | UUID | size | role |
| --- | --- | --- | --- |
| `0x5511` | `6ACC5511-...AD50` | 16  | METADATA (one AES block, dispatcher-decrypted with the session key) |
| `0x5512` | `6ACC5512-...AD50` | 255 | PAYLOAD chunks (cleartext for data files; scrambled for the bootloader file via the same `FUN_00024740` the central write dispatcher uses for property-bit-1 chars) |
| `0x5513` | `6ACC5513-...AD50` | 16  | STATUS notify (notify-only — outgoing OAD progress / completion / error codes via `oad_status_notify`) |

Metadata frame (9 bytes, BE):

```
[0]     filetype  — 0 = bootloader app at 0x80000;
                    1..0x20 = data file at 0x180000 + filetype*0x80000
[1..4]  filesize  (u32)
[5..8]  expected CRC-32 of the full file
```

Slot allocation:

| filetype | ext-flash base | slot capacity |
| --- | --- | --- |
| 0  | `0x080000` | `0x180000` (the full lower window) |
| 1..0x20 | `0x180000 + ft*0x80000` | `0x80000` (512 KB each) |

Status enum (single-byte arg to `oad_status_notify`):

| Code | Meaning |
| --- | --- |
| 0x12 | OAD started — metadata accepted, slot erased |
| 0x13 | OAD complete — final CRC matched (bootloader file only) |
| 0x14 | block accepted — sent after every data-file block via `module_publish_command(0x14, &block_idx, 4)` (Modbus to mainware) |
| 0x16 | CRC mismatch on final block |
| 0x17 | flash open failed |
| 0xF8 | invalid filetype (> 0x20) |
| 0xFF | filesize > slot capacity |

CRC handling quirk: for data files (filetype != 0), the **first 12 bytes** of the payload are excluded from the running CRC — they're treated as a VanMoof PACK container header that lives outside the file's own checksum.

OAD session state (RAM struct at literal-pool entries `DAT_0000BA4C` = `DAT_0000BDC0`, same target):

```
+0x00 u16  conn_handle      (0xFFFF == idle)
+0x04 u32  lock_handle      (TI-RTOS Semaphore_t)
+0x4C u8   filetype
+0x50 u32  filesize
+0x54 u32  expected_crc
+0x58 u32  flash_base       (ext-flash destination start)
+0x5C u32  flash_cursor     (current write offset)
+0x60 u32  bytes_received
+0x64 u32  running_crc      (init 0xFFFFFFFF, final ~CRC)
```

Concurrency: single connection at a time. Metadata-write captures `conn_handle`; every subsequent payload chunk rejects with `-1` if `param_1 != state.conn_handle`. A TI-RTOS semaphore (`oad_state_lock` @ `0x00020098`) gates state mutations.

Helpers named in Ghidra:

| Addr | Name | Role |
| --- | --- | --- |
| `0x000267A4` | `oad_gatt_write_handler` | service `0x5510` write callback |
| `0x00020098` | `oad_state_lock` | Semaphore_pend wrapper for the OAD state |
| `0x000265C4` | `bleware_control_event_post` | general 1-byte "post control event" dispatcher; OAD uses it for its status codes (0x12–0x17), `cmd_ble_erase_all_bonds` uses it with code 0x20. Earlier named `oad_status_notify` — superseded once the bonds-erase site was decoded. |
| `0x00025060` | `oad_session_close` | tear-down (releases lock, resets conn_handle to 0xFFFF) |
| `0x00016A50` | `extflash_erase_range` | **Decoded** — `src/extflash.c`. 4 KB-sector erase loop, semaphore-locked, 3- or 4-byte addressing per chip capacity. `secrets_sector_erase` @ `0x00026C30` is a fixed-address wrapper around this. |
| `0x00015B9C` | `extflash_write` | **Decoded** — `src/extflash.c`. Page-Program (0x02) loop, splits writes at 256 B page boundaries (PP wraps within a page). Caller must pre-erase. |

### `extflash.c` — external SPI NOR-flash driver

**Partially decoded** — `extflash_erase_range` (`bleware/src/extflash.c`). Hand-rolled SPI driver, not a TI-SDK driver instance. Singleton state at OEM `g_extflash_state` (DAT_00016AFC) carries a `chip_info` pointer (capacity at `+0x00`) and a TI-RTOS bus mutex at `+0x0C`.

Standard SPI NOR opcodes (sourced indirectly from per-chip command tables so the driver ports across vendors):

| opcode | mnemonic | role |
| --- | --- | --- |
| `0x02` | PP   | page program — 256 B page; writes that cross a page boundary are split by `extflash_write` |
| `0x03` | READ | standard read (no dummy byte); one CS-framed burst, address auto-increments |
| `0x05` | RDSR | read status register, WIP = bit 0 (busy-wait loop in `extflash_wait_wip_clear`) |
| `0x06` | WREN | write-enable latch, framed by CS assert/deassert in `extflash_write_enable` |
| `0x20` | SE   | 4 KB sector erase — 3-byte address if `capacity ≤ 16 MiB`, 4-byte otherwise |

`extflash_read` (OEM `0x0001C5A4`) and `extflash_get_chip_info` (OEM `0x000273D0`) are also decoded — both in `src/extflash.c`. `extflash_get_chip_info` is a one-liner: returns `g_extflash_state.chip_info`.

#### Chip identification (REMS-based)

`extflash_open` runs early in the Bluetooth task: opens the SPI bus at a slow bitrate, sends `0xAB` (Release Power-Down / RDP), waits WIP clear, then calls `extflash_identify_chip` (OEM `0x0001A328`). The probe sends a 4-byte REMS burst (`0x90 FF FF 00` — opcode + 24-bit dummy address that selects which device byte comes out first), reads 2 bytes (mfgr, dev), and linearly scans an embedded vendor table for a match. On hit, the entry pointer is stored in `g_extflash_state.chip_info`. If the matched chip's capacity > 16 MiB, the driver follows up with a single-byte `0xB7` EN4B (Enter 4-Byte Addressing Mode) over CS so later READ/PP/SE opcodes can use 32-bit addresses.

If identification succeeds, `extflash_open` closes the slow-bitrate handle and reopens at the production bitrate.

#### `struct extflash_chip_info` (16 bytes)

| Offset | Field | Notes |
| --- | --- | --- |
| +0x00 | `u32 capacity` | bytes; > 0x01000000 ⇒ 4-byte addressing |
| +0x04 | `u8 jedec_mfgr` | REMS byte 1 |
| +0x05 | `u8 jedec_dev`  | REMS byte 2 (the "capacity-3" device id) |
| +0x06 | `u16 _pad`      | always 0x0000 |
| +0x08 | `const char *part_name` | 12-byte slot in the part-name table at flash `0x0002B038` |
| +0x0C | `u32 sector_size` | bytes; always 0x1000 in observed entries |

#### Driver-header layout (top 28 bytes of the table at flash `0x0002A450`)

| Offset | Field | Value |
| --- | --- | --- |
| +0x00 | `enter_4byte_mode_cmd` | `0xB7` (EN4B) |
| +0x01 | `rdsr_cmd` | `0x05` (RDSR) — sourced by `extflash_wait_wip_clear` |
| +0x02 | `wren_cmd` | `0x06` (WREN) — sourced by `extflash_write_enable` |
| +0x03 | `wrsr_cmd` | `0x01` (WRSR) |
| +0x05..+0x08 | REMS cmd burst | `90 FF FF 00` |
| +0x0C..+0x1B | SPI_Params template | TI-SDK SPI driver init values (16 B) |

#### Supported parts (5 entries + zero-terminator at flash `0x0002A46C`)

| Capacity | Mfgr/Dev | Part name | Notes |
| --- | --- | --- | --- |
| 64 MiB | `C2 / 19` | MX25L51245G | Macronix 512 Mb — the production S3 part; needs EN4B |
| 2 MiB  | `C2 / 15` | MX25R1635F  | Macronix 16 Mb ULP |
| 1 MiB  | `C2 / 14` | MX25R8035F  | Macronix 8 Mb ULP |
| 512 KiB | `EF / 12` | W25X40CL   | Winbond 4 Mb |
| 256 KiB | `EF / 11` | W25X20BV   | Winbond 2 Mb |

Only the MX25L51245G needs 4-byte addressing — the other four fit within 24-bit (16 MiB) addresses. The 64 MiB part is consistent with the OAD slot map (`0x80000 + ft*0x80000`, max ft ~5 in observed traffic) and the `0x03FDD000` log region offset.

`g_extflash_state` (RAM `0x20001B20`) layout, fields actually touched by decoded primitives:
- `+0x00` u8  open flag (1 = initialised)
- `+0x04` `chip_info *` (populated by `extflash_identify_chip`)
- `+0x08` SPI bus handle (TI-SDK)
- `+0x0C` TI-RTOS bus mutex (taken at the head of every read/write/erase)
- `+0x14` per-handle SPI transaction struct (init'd by `FUN_0001733C`)
- `+0x21..+0x22` REMS rx buffer (mfgr/dev bytes; used as match keys)

`extflash_erase_range(addr, len)` aligns `addr` down to 4 KB (`& 0xFFFFF000`), then erases `((addr+len) - aligned + 0xFFE) >> 12` sectors. Each iteration: pend bus mutex → wait WIP clear → WREN → emit SE cmd over CS-framed SPI burst → advance. Returns 1 on success, 0 on any sub-step failure.

Helpers exposed as weak stubs (until each lands in its own .c):

| Addr | Name | Role |
| --- | --- | --- |
| `0x00026F94` | `extflash_cs_assert` | CS = 0 (via `FUN_00022E08`, the IOC GPIO writer) |
| `0x00026F84` | `extflash_cs_deassert` | CS = 1 |
| `0x00024448` | `extflash_spi_tx` | SPI bus tx via `FUN_000274E8` (driver wrapper) |
| `0x00024418` | `extflash_spi_rx` | SPI bus rx |
| `0x00021764` | `extflash_wait_wip_clear` | busy-wait on RDSR until WIP clears |
| `0x00024478` | `extflash_write_enable` | one-byte WREN framed by CS |

### `log_gatt.c` — circular log readout (`svc 0x55C0`)

**Decoded** — `src/log_gatt.c`. Three characteristics:

| Char | UUID | size | role |
| --- | --- | --- | --- |
| `0x55C1` | `6ACC55C1-...AD50` | 16 | CONTROL — sub-command channel. `payload[0]==2` runs `log_seek_to_timestamp(BE u32 at payload[1..4])`; anything else forwards to Modbus as `module_forward_async(0x55C1, payload[0])`. Response notified on channel 0. |
| `0x55C2` | `6ACC55C2-...AD50` | 16 | (no write path; notify channel 1) |
| `0x55C3` | `6ACC55C3-...AD50` | up to 240 | READOUT — paginated log dump. `payload[0..3]` = starting log index (BE u32, 16-byte units), `payload[4]` = entry count (capped to MTU window). Reads N × 16 bytes via `log_read_entry` and notifies on channel 2. |

**Log storage:** 128 KB circular region on ext-SPI flash at `0x03FDD000..0x03FFCFFF` (wrap-mask `0x1FFFF`). 4 KB sector at `0x03FDC000` immediately preceding the log holds the persisted read-cursor (8-byte head/tail pair, written by `log_seek_to_timestamp`).

The log appears to contain ASCII text lines terminated by `'\n'`. `log_seek_to_timestamp` scans 256-byte windows from the persisted cursor looking for newlines and parses the leading ASCII integer of each line (Unix timestamp) — advancing the cursor until the next line's timestamp exceeds the requested target. The 16-byte fixed reads on `0x55C3` are pagination units; the client reassembles them to recover full ASCII lines.

Helpers named in Ghidra:

| Addr | Name | Role |
| --- | --- | --- |
| `0x00014910` | `log_gatt_write_handler` | svc `0x55C0` write callback |
| `0x0001E9D8` | `log_read_entry` | reads one 16 B entry; wraps via `(head + idx*16) & 0x1FFFF` |
| `0x00014C68` | `log_seek_to_timestamp` | scans for `\n` lines, parses leading ASCII timestamp, persists new cursor to 4 KB sector at `0x03FDC000` |
| `0x0001B9F4` | `log_gatt_notify_channel` | per-channel notify dispatch (channels 0/1/2 → chars `0x55C1/0x55C2/0x55C3`); channel 2 supports up to 0xF0 bytes (aligned down to 16) |
| `0x000061C0` | `xs3_gatt_process_read_event` | central ReadAttrCB dispatcher (`src/gatt_read.c`) |
| `0x0001EEF4` | `module_publish_sync_with_timeout` | synchronous Modbus fetch with semaphore-pend timeout |
| `0x00026594` | `module_bus_is_idle` | inter-module bus idle predicate (`*DAT_000265A8 != -1`) |
| `0x00026050` | `runtime_permission_mask` | 32-bit cap-set drawn from secrets cache `+0x14` |
| `0x00020338` | `log_block_count_get` | head/tail delta on the 128 KB circular log, in 16 B blocks |

**Tags / literals stashed in the binary:**
- `"MOOF\0"` at `0x000195E8`
- `"MKEY\0"` at `0x000195F0`
- pointer-to-FCFG1.MAC_BLE (`0x500012E8`) at `0x000195F8`
- pointer-to-RAM-working-buffer (`0x2000A3DC`) at `0x000195FC`
- `"0123456789ABCDEF"` at `0x0002B46C`
- Error message `"Invalid message length <%d>, should be a multiply of 16"` at `0x00004160`
- Source-file path `"source/xs3_gatt_backoffice.c"` at `0x00004140`

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
| `0x000266C8` | `bim_chip_assert_supported` | **Decoded** — `src/chipinfo.c`. Spin-traps unless family == CC13x2/CC26x2 and HwRev ≥ 20 (PG2.0). |
| `0x00025D24` | `bim_chip_family` | **Decoded** — `src/chipinfo.c`. Reads FCFG1.ICEPICK_DEVICE_ID (0x50001318); PARTNO at bits [27:12] == 0xBB41 → family 4, else -1. |
| `0x00021BCC` | `bim_chip_hw_revision` | **Decoded** — `src/chipinfo.c`. Returns HwRevision_t enum from FCFG1.ICEPICK_DEVICE_ID PG-rev nibble + FCFG1.MINOR_HW_REV byte (0x500010A0). PG2 base 11, PG3 base 21 (preserves OEM driverlib numbering quirk vs current SDK). |
| `0x000173E8` | `bim_setup_after_cold_reset_cfg1` | cold-reset trim cfg helper (called when `BB_AON_GATE` is set). **Decoded** — `src/setup_cold_reset.c`. Walks FCFG2/FCFG1 fuse words, pokes ADI3/DDI0/AON-shadow trim regs, drives the ROM HAPI cold-reset state machine (slots 0/1/2 of ROM_API_TABLE[28]), and sets the FLASH trim-done bit-band at 0x42600494. Byte-shape identical to bleboot's at flash 0x00056490. |
| `0x00023AF4` | `bim_setup_adi_step` | analog-config sequencer at MMIO 0x400C6000. **Decoded** — `src/setup_cold_reset.c`. Steps the live ADI mode toward target=2 via the 8-byte LUT at flash `0x0002BA64` (`01 02 00 03 02 00 01 03` — identical content to bleboot's LUT at flash `0x000571D8`). |

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
