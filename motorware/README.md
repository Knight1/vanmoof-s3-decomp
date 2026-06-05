# motorware

VanMoof S3 **motor-controller** firmware, for the TI **TMS320F28054F** — a
C2000 Piccolo fixed-point DSP (**C28x core, not ARM**). This directory holds the
reverse-engineering of `motorware_S.0.00.22` (the version shipped in the current
`.pak`; an older `S.0.00.15` is also analysed).

## Status

Analysis foundation **complete and verified**; per-function C reconstruction is
the ongoing work. See [`docs/progress.md`](docs/progress.md).

* **Container** — VanMoof header + TI C28x boot-ROM data stream; CRC recomputed
  byte-exact and a **byte-exact round-trip** rebuild, both images
  ([`docs/container.md`](docs/container.md)).
* **Image** — reconstructed at true word-addresses; flash `0x3EE000`–`0x3F530F`
  (main) + `0x3F7000` + the `0x3F7FFE` codestart ([`docs/memory-map.md`](docs/memory-map.md)).
* **Boot** — verified `reset → 0x3F7FFE → wd_disable(0x3F4C19) → _c_int00(0x3F4799)`.
* **Disassembly** — **356 functions** via IDA's native C28x
  ([`ida/README.md`](ida/README.md)); state lives in an L3-RAM IQ24 control
  struct (`0x9000`+), initialised by the decoded `.cinit` table.
* **Identity** — module type `0xA1`; updated over YMODEM ([`docs/protocol.md`](docs/protocol.md)).

A byte-equivalent rebuild is out of reach without TI's `cl2000` codegen (no GCC
backend exists for C28x); the target is behaviour-faithful C, like batteryware.

## Layout

```
motorware/
├── docs/        container.md, memory-map.md, hardware.md, protocol.md,
│                wire-protocol.md (talk to mainware as the motor), progress.md
├── tools/       bootstream.py (container/boot-stream codec), motor_sim.py
│                (build/decode SCI-A frames — forge motor telemetry),
│                c28emu.py (C28x interpreter — analysis aid)
├── ida/         build_db.py, probe.py, README.md — C28x disassembly pipeline
├── src/         reconstructed C: comm.c (SLIP+CRC link), registers.c (telemetry
│                read + motor-command write + dispatch), foc.c (FOC current loop),
│                motor_state.h (L3 map + fault bits), params.c (.cinit defaults)
└── build/       generated: image/ (regions), ida/ (db, listing, call graph)  [git-ignored]
```

## Use

```bash
make -C motorware verify     # container CRC + byte-exact round-trip (gate)
make -C motorware info       # full report: header, block map, regions, round-trip
make -C motorware image      # reconstruct build/image/region_*.bin for the disassembler

python3 motorware/tools/bootstream.py --cinit <oem.bin>   # dump L3 param defaults
```

Disassembly (IDA 7.0, headless): see [`ida/README.md`](ida/README.md).

OEM images are **not** committed (`.gitignore`); point `OEM_IMAGE=` at your own
extracted `motorware_S.0.00.22.bin`.
