# motorware — decomp progress

**Target:** `motorware_S.0.00.22` (VanMoof S3 motor controller),
TI **TMS320F28054F** (C2000/C28x DSP). Older `S.0.00.15` also on hand.

**Status:** `foundation + 356 disassembled + protocol & control-loop mapped` —
container byte-exact, memory map and boot flow verified, image in IDA at true
word-addresses; the **mainware serial-link stack reconstructed to C**
(`src/comm.c`) with the full **command/register map** (`protocol.md`); and the
**control architecture mapped** — `HAL_init`, ePWM phases, **InstaSPIN-FOC ROM**,
8 PIE ISRs (`hardware.md`). A byte-equivalent rebuild needs TI's `cl2000`
codegen (no GCC backend for C28x); the target is **behaviour-faithful C**.

**Link protocol (verified):** SLIP framing + CRC-16/Modbus (poly `0xA001`, init
`0xFFFF`) over **SCI-A** — point-to-point, no slave address. The earlier
"Modbus-RTU slave `0xA1`" inference was **disproven** by the disassembly (it's
SLIP, not Modbus framing). See `protocol.md` / `src/comm.c`.

## Why this ware is different

The C28x is not ARM: word-addressed (1 unit = 16 bits, `sizeof(int)==1`),
little-endian, fixed-point IQmath, no FPU. **Stock Ghidra 11.2 has no C28x
processor module**, so this ware is analysed with **IDA 7.0** (native
`tms32028`) driven headless from WSL — not the GhidraMCP flow the ARM wares use.
See `../ida/README.md` for the pipeline. The "program JSON" equivalent here is
`build/ida/funcmap.json` (call graph + per-function peripheral tags) and
`build/ida/motorware.lst` (full listing).

## Verified foundation (done)

| Item | Status | Evidence |
| --- | --- | --- |
| VanMoof wrapper header (magic/version/CRC/size/date) | ✅ verified | `tools/bootstream.py` — CRC recomputed byte-exact, both images |
| C28x boot-ROM data-stream parse | ✅ verified | 6 blocks tile flash, **byte-exact round-trip** rebuild |
| Flash image reconstruction (true word addrs) | ✅ done | `build/image/region_*.bin` + `manifest.json` |
| F28054F memory / peripheral map | ✅ verified | SPRS797F / `F28054.cmd` (see `memory-map.md`) |
| Boot flow `0x3F7FFE → wd_disable → _c_int00` | ✅ verified | `LB 0x3F4C19` decode + xref `0x3F4C1F→_c_int00` |
| `.cinit` table (97 records → L3 RAM globals) | ✅ verified | matches TI COFF negated-length format; `--cinit` |
| IQ24/float param defaults | ✅ verified | `.cinit` values (`0x00333333`=0.2, `0x42C80000`=100.0f) |
| IDA C28x disassembly (356 functions) | ✅ done | `build/ida/motorware.lst`, `functions.json`, `funcmap.json` |
| Module identity `0xA1`, YMODEM update path | ✅ verified | header + VanMoof tool console (`um`/`motorupdate`) |

## Function inventory (356 total)

All discovered functions live in the main region `0x3EE000`–`0x3F4C58`.
260 are leaves, 84 are ≤3-insn stubs (IQ helpers / accessors). The
control-heavy orchestrators (many L3-param accesses) are the priority targets.
Full data: `build/ida/funcmap.json`.

| addr | insns | callers | callees | L3-acc | role (inferred) |
| --- | --- | --- | --- | --- | --- |
| `0x3F4799` | — | 1 | — | — | `_c_int00` (TI rts2800 startup) ✅ identified |
| `0x3F4C19` | — | 1 | 1 | — | `wd_disable` (codestart target) ✅ identified |
| `0x3F4B20` | 10 | 1 | 0 | — | `modbus_crc16_byte` ✅ **decomp-c** (`src/comm.c`) |
| `0x3F4B2C` | 14 | 2 | 1 | — | `modbus_crc16` ✅ **decomp-c** |
| `0x3F4294` | ~25 | 4 | 1 | 3 | `sci_tx_byte` (SCI-A TX ring) ✅ **decomp-c** |
| `0x3F3310` | 61 | 2 | 2 | 0 | `slip_tx_frame` (SLIP+CRC encode) ✅ **decomp-c** |
| `0x3F33E6` | 104 | 1 | 3 | 8 | SLIP RX state machine — partial (front half) |
| `0x3F3472` | 46 | 4 | — | 4 | link service / **command dispatch** (opcode 5/6/7) ✅ mapped |
| `0x3EE47C` | 117 | 1 | 3 | — | **read registers** 10–13 ✅ mapped (`protocol.md`) |
| `0x3EE50E` | 364 | 1 | 11 | — | **write registers** 20–25 ✅ mapped |
| `0x3F32C9` | — | 2 | 1 | — | enqueue response (8×13 ring @`0x9440`) ✅ |
| `0x3F345C` | 21 | 1 | 0 | — | opcode-5 ack (clear queue entry) ✅ |
| `0x3F0675` | 184 | 1 | many | — | **`HAL_init`** ✅ mapped (ePWM1-4/ADC/PIE/SPI/SCI/timer) |
| `0x3F3228` | — | 4 | — | — | `EPWM_setup(base)` — per phase (1/2/3) + ePWM4 |
| `0x3F2444` | 139 | 1 | 5 | 32 | **8 PIE ISRs** (`0x3F2507`…`0x3F2845`) — control/comm/fault |
| `0x3F1CE0` | 414 | 1 | 5 | — | eCAN setup |
| `0x3EE894` | 1190 | 1 | 68 | 287 | top control orchestrator (main-init / control build) |
| `0x3F1FCE` | 692 | 2 | 8 | 231 | control/estimator state update |
| `0x3F0A5F` | 1618 | 2 | 8 | 0 | large compute (no L3 — math/transform) |
| `0x3EF57E` | 1525 | 1 | 8 | 0 | large compute |
| `0x3F2933` | 791 | 8 | 9 | 4 | shared (8 callers) |
| `0x3F36E0` | 293 | 14 | 6 | 2 | common utility (14 callers) |
| `0x3F3CC7` | 220 | 12 | 7 | 0 | common utility (12 callers) |
| `0x3EE07C` | 210 | 1 | 1 | 62 | param init/copy |

## Open work (priority order)

1. **Comm/protocol** — ✅ link layer (`src/comm.c`) + **command dispatch &
   register map** done: opcodes 5(ack)/6(read 10–13)/7(write 20–25), reliable
   response queue @`0x9440` (`protocol.md`). Remaining: name the `0x9000`+ L3
   fields the registers touch, and classify SCI-C / eCAN.
2. **Control loop** — ✅ architecture mapped: `HAL_init` (`0x3F0675`), ePWM1-3
   phases, **InstaSPIN-FOC ROM** (60+ calls to `0x3F8xxx`–`0x3FBxxx`), 8 PIE
   ISRs (`hardware.md`). Remaining: which ISR is the fast current loop, the
   exact Clarke→Park→PI→SVGEN steps, and the ADC channel→signal map.
3. **L3 struct typing** — name the `0x9000`+ globals from `.cinit` + usage, and
   emit `src/params.*` (defaults already extracted, see `src/`).
4. **Secondary regions** — give IDA entry-point hints for the function-pointer
   -reached code at `0x3F4C5A` and `0x3F7000` (currently not split into funcs).
5. **Per-function C** — translate functions bottom-up (leaves first) into
   `src/*.c`, flipping rows here to `decomp-c` as the other wares do.

## Reproduce

```
make -C motorware verify     # container CRC + byte-exact round-trip
make -C motorware image      # reconstruct flash regions for the disassembler
# then build the IDA database + listing + call graph (see ida/README.md)
```
