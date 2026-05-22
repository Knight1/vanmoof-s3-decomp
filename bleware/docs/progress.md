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

| Slot | Ptr | Handler | Status |
| --- | --- | --- | --- |
| 0 | `0x0001A969` | `cmd_firmware_update` | decoded |
| 1 | `0x0000ABD9` | `cmd_extflash_verify` | decoded earlier |
| 2 | `0x0001699D` | TBD | pending split/name |
| 3 | `0x0000C2D5` | `cmd_log_dump` | decoded |
| 4 | `0x00016DCD` | TBD | pending split/name |
| 5 | `0x0000E191` | `cmd_log_inject` | decoded |
| 6 | `0x0000F19D` | TBD | pending split/name |
| 7 | `0x0001CE3D` | TBD | pending split/name |
| 8 | `0x0001522D` | TBD | pending split/name |
| 9 | `0x0000CAE1` | TBD | pending split/name |
| 10 | `0x0000C615` | TBD | pending split/name |
| 11 | `0x0001406D` | TBD | pending split/name |
| 12 | `0x00011D4D` | `cmd_pack_list` | decoded |
| 13 | `0x00010555` | `cmd_pack_delete` | decoded |
| 14 | `0x000116AD` | TBD | pending split/name |
| 15 | `0x00007A59` | `cmd_ble_info` | decoded |
| 16 | `0x0001B3C5` | TBD | pending split/name |
| 17 | `0x0001E8B9` | TBD | pending split/name |
| 18 | `0x0001DCD1` | TBD | pending split/name |
| 19 | `0x0000F6A1` | TBD | pending split/name |
| 20 | `0x00010079` | TBD | pending split/name |
| 21 | `0x0001D8E5` | TBD | pending split/name |
| 22 | `0x0001B441` | `cmd_info_ver` | decoded |
| 23 | `0x00014839` | TBD | pending split/name |
| 24 | `0x00013BE9` | `cmd_help` | decoded |

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
| `0x00003E78` | `gatt_handle_backoffice_message_data` | GATT-backoffice handler — decrypts a 16-byte-multiple ciphertext blob with the M-Key (via ROM AES jump table at `_DAT_100001FC + 0x20`), upserts the resulting records via `secrets_upsert_keyed_batch`, ensures M-ID, rebuilds the 6-byte BLE address. **OEM name confirmed via embedded symbol string at `0x0002B4B4`.** Source file: `source/xs3_gatt_backoffice.c` (path string at flash `0x00004140`). |
| `0x00020848` | `block_dispatch_queue_post` | TI-RTOS-style queue post: packages `(key_buf, block_len, src, src_alias)` into a message and hands it to `FUN_000275E8` → `FUN_000125C4` → ROM jump table |
| `0x00026504` | `byte_to_hex_chars` | writes 2 ASCII hex chars from a byte; hex table at flash `0x0002B46C` = `"0123456789ABCDEF"` |

### `provisioning.c` — backoffice key-import handler (`xs3_gatt_backoffice.c`)

**Decoded** — `src/provisioning.c`. Two functions land here, both touching the Manufacturing Key:

- **`manufacturing_key_get_or_init_default` @ `0x00019570`** — fetches slot 126. On read-failure (factory-fresh device) it materialises a deterministic per-device fallback **in RAM only**: 12 ASCII hex chars of the chip's BLE MAC (read from FCFG1 at `0x500012E8`), `"MOOF"` tag at payload offset +12, `"MKEY"` tag at offset +24, CRC at +28. The fallback is *not* written back to flash — only cached for the current boot, so a factory tool can still detect the unprogrammed state and run the full provisioning flow.

- **`secrets_provisioning_blob_apply` @ `0x00003E78`** — sub-command dispatcher for the GATT backoffice characteristic. Decoded path: the **"bulk encrypted record import"** sub-command. Each 16-byte ciphertext block of the input blob is paired with the M-Key and posted to the CryptoCC26X2 ROM AES task via `block_dispatch_queue_post` → `FUN_000275E8` → `FUN_000125C4` → indirect call through `_DAT_100001FC + 0x20`. Decrypted records bulk-upsert into the secrets store; slot 124 (M-ID) is reaffirmed; a 6-byte BLE address is composed from M-ID + a stack-supplied counter. Other tbh-dispatched sub-commands (status reply, single-record write, etc.) are still TBD.

**Why no AES S-box in bleware:** AES lives in CC2642R1F ROM. App-side calls are pure function-pointer indirect-calls through ROM jump tables (e.g., `_DAT_100001FC[+0x20]` in the bleware base pointer table). This explains the earlier dead-end byte-pattern search. Any future crypto investigation on CC26X2 SimpleLink images should chase ROM jump tables, not S-box bytes.

**No direct xref to `secrets_provisioning_blob_apply` in the binary.** The address `0x00003E78` (or `0x3E79` Thumb-mode) does not appear as a 32-bit literal anywhere — neither in a service-attribute table nor in code. The function is most likely registered with the GATT stack at runtime via a vtable-population helper that takes the address as an immediate (`adr` / `add pc, …`) rather than a literal pool entry. Identifying the registration site is a follow-up — it needs the GATT-service decode.

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
