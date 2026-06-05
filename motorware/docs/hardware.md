# Hardware & control model — motorware (TMS320F28054F)

What the motor-controller firmware drives, and how its state is organised.
Device facts are from SPRS797F/SPRUHE5 (see `memory-map.md`); everything tagged
**(image)** is observed in the disassembly (`build/ida/motorware.lst`),
**(inferred)** is a reasoned hypothesis not yet line-proven.

## MCU at a glance

TI **TMS320F28054F** — C2000 Piccolo, 60 MHz C28x fixed-point DSP. **No FPU,
no VCU.** A CLA exists on the `F` part, but its program RAM (L3, `0x9000`) is
used here as the application's data RAM, so the **CLA is most likely unused**
(image). All arithmetic is fixed-point **IQmath**.

Relevant on-chip peripherals (SPRS797F): 7× ePWM, 1× eCAP, 1× eQEP, 1× 12-bit
ADC (single S/H), up to 7 analog comparators with DACs, 3× SCI, 1× SPI, 1× I2C,
1× eCAN, 3× CPU timers, watchdog. Register-frame bases in `memory-map.md`.

## Fixed-point math: IQ24 / IQmath **(image, verified)**

The control code is dense with the C28x IQmath multiply idiom — e.g. in the
`0x3F7000` region:

```
movl  XT,  ACC
qmpyl ACC, XT, @XAR7     ; signed 32×32 -> high 32 of 64
impyl P,   XT, @XAR7     ; low 32 of 64
asr64 ACC:P, #12         ; (or lsl64 #8) -> Q-shift to realign the product
```

`qmpyl`+`impyl`+a 64-bit shift is exactly `_IQmpy` (Q-format fixed-point
multiply). The `.cinit` defaults decode cleanly as **IQ24** (`0x00333333`=0.2,
`0x0001EB85`≈0.0075, `0x0067AB5F`≈0.405) with a few IEEE-754 **float** entries
(`0x42C80000`=100.0f), so the global Q is **IQ24** with selected float params.

## Control / parameter state in L3 RAM `0x9000`–`0x96xx` **(image)**

The application's working set is one large structure block in **L3 DPSARAM**.
DP-relative access counts from the listing (top pages):

| Page | accesses | Page | accesses | Page | accesses |
| --- | --- | --- | --- | --- | --- |
| `0x9000` | 230 | `0x9180` | 55 | `0x9140` | 16 |
| `0x9040` | 81 | `0x9080` | 24 | `0x9200` | 12 |
| `0x9580` | 78 | `0x9600` | 12 | `0x9500` | 8 |

The same range is what the **`.cinit`** table initialises at boot: 97 records
spanning `0x9001`–`0x9651` (regenerate with
`python3 tools/bootstream.py --cinit <image>`). This is the firmware's
configuration + live-control object — consistent with a TI **MotorWare /
InstaSPIN-FOC** CTRL/EST/USER layout **(inferred)**: a parameter block (motor
constants, current/voltage scaling, PI gains — the IQ24/float `.cinit` values)
plus live estimator/controller state. Peripheral registers are reached by
pointer (XARn-indirect), not DP, so the hot path manipulates this struct while
init/library code touches hardware.

The recovered field names are in **`src/motor_state.h`** (33 fields, 22 verified
from code) — the HAL pointer `0x903E`, status flags `0x9017`, the FOC
measurement struct `0x9580`, setpoints `0x95A0/0x95A4`, the comm ring/queue, etc.

### Notable `.cinit` defaults (IQ24 unless noted)

| L3 addr | value | reading |
| --- | --- | --- |
| `0x9048` | `0x00334052` | 0.2002 |
| `0x904A` | `0x0001EB85` | 0.0075 |
| `0x904C` | `0x0067AB5F` | 0.4050 |
| `0x9052` | `0x00800000` | 0.5000 |
| `0x9064` | `0x01BC79F0` | 1.7362 |
| `0x9074` | `0x42C80000` | **100.0** (float32) |
| `0x90A6` | `0xDEADBEEF` (+`0x16A1`) | sentinel + version (22) stamped into RAM |

The `0xDEADBEEF` sentinel and the version word (`0x16A1` = patch 22) being
written into RAM at init is a build/identity marker — a useful liveness probe.

## Motor drive — InstaSPIN-FOC (verified architecture)

This is a fixed-point **InstaSPIN-FOC** drive, confirmed in the image:

* **`HAL_init` = `sub_3F0675`** (`0x3F0675`) does the full peripheral bring-up,
  storing each handle into the HAL object (pointer at L3 `*(0x903E)`):
  ADC OTP calibration (`0x3D7A18`), PLL/clock (`SysCtrl 0x7010`), flash
  wait-states (`0xA80`), analog subsystem / comparators (`0x6400`),
  **ADC** (`AdcResult 0xB00`), **GPIO** (`0x6F80`), **PIE** (`0xCE0`),
  **SPI-A** (`0x7040`), **ePWM1/2/3/4** (each via `EPWM_setup = sub_3F3228`),
  **SCI-A/SCI-C**, and **CPU-Timer0** (`0xC00`).
* **ePWM1/2/3** drive the **3-phase inverter** (`sub_3F3228` per module);
  **ePWM4** is the 4th (likely the ADC-SOC / ISR timebase). The F28054 has 7
  ePWM total.
* **InstaSPIN-FOC ROM** — the firmware makes **60+ calls into the on-chip
  library ROM** at `0x3F8000`–`0x3FBxxx` (the F2805x**F** FAST estimator +
  fixed-point math). So flux/angle/speed estimation and the heavy math run from
  ROM; the flash code is the FOC application layer (Clarke/Park/PI/iPark/SVGEN
  glue + supervisory logic), all in **IQ24**.
* **eCAP1** (`0x6A00`) is configured — rotor position/speed capture (Hall or
  sensored assist alongside the sensorless FAST estimator).
* **The FOC runs in the background main loop**, not a hard-real-time ISR: the
  main control function **`sub_3EE894`** calls the **FOC core `sub_3F0A5F`**
  (1618 insns, **58 InstaSPIN ROM calls** — the FAST estimator + controller),
  paced by the ePWM4 ISR timebase below.
* **8 PIE ISRs** (`0x3F2507`–`0x3F2845`, installed via the HAL `PieVectTable`
  pointer at HAL `+0x78`, plus one RAM vector `0x8A00`), identified by the HAL
  handle each fetches at entry:

  | ISR | HAL off | role |
  | --- | --- | --- |
  | `0x3F261C` | `0xC0` Scia | **SCI-A RX** — buffers inbound SLIP bytes (mainware bus) |
  | `0x3F25F7` | `0xC0` Scia | **SCI-A TX** — drains the 64-byte TX ring |
  | `0x3F252C` | `0xD2` Scic | **SCI-C RX** — ASCII debug console (`'f'`,`'H'`,`'P'`…) |
  | `0x3F2507` | `0xD2` Scic | **SCI-C TX** |
  | `0x3F2826` | `0x82` EPwm4 | **ePWM4 timebase** — increments tick `0x9009`, clears flag, acks PIE; the FOC pace |
  | `0x3F2717` | — | housekeeping — 4 digital inputs (`sub_3F3B0C`), refreshes read-reg-13 telemetry |
  | `0x3F2652`, `0x3F2845` | `0x90`/`0xBE` | two further handlers (one likely ADC / CPU-timer) |

  The SLIP decode + command dispatch are **polled from the main loop**, not done
  in the RX ISR (see `protocol.md`). Remaining: confirm the ADC sampling path +
  channel→phase map, and the Clarke→Park→PI→SVGEN steps inside `sub_3F0A5F`.
* **Gate driver = TI DRV8301** (confirmed by mainware error 46 "Motor Driver
  DRV8301 over-current"). This ties the peripherals together: **ePWM1/2/3** →
  the DRV8301's three half-bridge gate drivers → motor; the DRV8301's
  **current-sense amplifiers** → **ADC** phase currents; its **nFAULT/nOCTW**
  status pins → GPIO → the `0x9017` digital-input fault bits (`0x0020`/`0x0040`);
  and **SPI-A** (`0x7040`, set up in `HAL_init`) is the DRV8301's SPI
  configuration interface. The startup **current/voltage offset calibration**
  (`sub_3EE2FD`) zeroes the current-sense reading and validates it — mainware
  errors 49/50 if it deviates. See `docs/protocol.md` for the full fault→error map.
* **ADC signal chain (verified by structure + the offset faults / error codes):**
  the ADC samples the **phase currents** (from the DRV8301 current-sense amps),
  **voltages** (phase + DC-bus), and a **temperature** sense. Results land in an
  `adcData` struct that the FOC loop reads — it takes two phase currents into the
  Clarke transform. At startup the **OFFSET module** (`OFFSET_init` ×2 in
  `HAL_init`; `OFFSET_setBeta/InitCond/Offset` in `sub_3F0497`) measures the
  zero-current / zero-voltage offsets; `sub_3EE2FD` then validates them against a
  window → **current-offset fault `0x0200` = error 49**, **voltage-offset fault
  `0x0400` = error 50**; the 3-threshold temperature check (`sub_3EE842`) →
  **`0x1000` = error 51 derating**. The literal **ADCINx-pin ↔ signal map**
  (which `ADCRESULTn` is phase A/B/C / DC-bus / temp) lives in the ADC `SOCxCTL`
  channel-select config, reached through the ADC *control* handle (a HAL struct
  field — the ADC control base `0x7100` is never an immediate), so static
  extraction is not possible. **Emulation was attempted** (`tools/c28emu.py`, a
  C28x interpreter over the IDA listing): running `HAL_init` stalls in the
  `OFFSET_init`/PLL region because faithful branch emulation needs precise
  ST0/ST1 status-flag modelling and the init spins on hardware-ready bits that
  require a peripheral model — heuristics to force past them would yield
  untrustworthy channel numbers. And even a perfect emulation returns ADC
  *channel numbers*, which still need the **board schematic** (DRV8301 CSA →
  ADCIN wiring) to map to physical motor phases. So the firmware-only ceiling is
  the ADC *signal structure* above; the literal pin map needs the schematic.
* **eCAN** (`0x6000`) is set up separately by `sub_3F1CE0` — role TBD (telemetry
  or a second channel).

The control/parameter state for all of the above is the L3 struct at `0x9000`+
(below).

### InstaSPIN init — `sub_3F0A5F` (the FOC setup)

The FOC core `sub_3F0A5F` (called once from `sub_3EE894`) is the **InstaSPIN
setup**: it makes **43 distinct ROM-setter calls** (each once), loading the
motor/controller parameters as **IEEE-754 floats**, converting them to **IQ24**
via the C28x float runtime (flash helpers `sub_3F43EE`/`sub_3F4AE8`/`sub_3F436B`/
`sub_3F4A72`/`sub_3F4BD9`), and writing them into the EST/CTRL/USER objects.

The float constants it builds inline (**verified**) are the standard MotorWare
`USER_*` scaling parameters **(meanings inferred from InstaSPIN convention)**:

| value | likely USER parameter |
| --- | --- |
| `100.0` | `IQ_FULL_SCALE_CURRENT_A` (±100 A full-scale) |
| `60.0` | `IQ_FULL_SCALE_VOLTAGE_V` (60 V FS — a 48 V battery system) |
| `20.0` | `PWM_FREQ_kHz` (20 kHz) |
| `1000.0` | `IQ_FULL_SCALE_FREQ_Hz` / a freq scale |
| `6.2812` (≈2π) | the rad↔Hz conversion constant |
| `1.0`, `0.1992`, `0.3594`, `0.6992`, `0.8594` | per-unit gains / filter coefficients |

So this is a ~250 W, ~48 V e-bike PMSM drive at 20 kHz with 100 A / 60 V
full-scale sensing.

With the ROM symbols applied, `sub_3F0A5F`'s 44 named setters resolve to the
full **EST (FAST estimator) setup** — in order: `EST_{Angle,Iab,Idq,I,
OneOverDcBus,Dir,EPL,Flux,Flux_ab,Flux_dq,Freq,Ls,Rr,Rs,RsOnLine,TRAJ,Vab,Vdq,
Vback_ab,Vback_dq}_setParams`, the **full-scale** writes `EST_setFullScale{Current,
Freq,Resistance,Voltage}` (= 100 A / 1000 Hz / R / 60 V), the motor model
`EST_setMotorParams` + `EST_Rs_setRs` + `EST_Ls_setLs_d`/`_q` + `EST_Rr_setRr` +
`EST_Flux_computeOneOverFlux_qFmt` (flux build `0x3F333333` = **0.7**), and the
flags `EST_setFlag_enableRsOnLine`/`enableRsRecalc`/`updateRs` (online stator-R
tracking **on**). The motor electricals (Rs, Ls_d, Ls_q, poles) are read from a
params struct at byte offsets **142 / 136 / 138 / 53** — their exact numeric
values are the one remaining FOC-config detail to chase through that struct's
initialiser.

> **ROM symbols applied.** The F2805x InstaSPIN ROM symbol library
> (`2805x_OnlyFastSpinROMSymbols`, 346 names, library base **`0x3F8808`**) is
> loaded into the IDA database (`build/ida/rom_symbols.txt`, applied by
> `build_db.py`), so every `lcr 3Fxxxxh` now reads as its InstaSPIN name
> (`EST_run`, `EST_getAngle_pu`, …). Per the InstaSPIN architecture only the
> closed **FAST observer (`EST_*`)** lives in ROM; CLARKE/PARK/IPARK/SVGEN/PID
> `_run` are inline in flash. motorware uses the **open-source style** — it
> calls `EST_run` (not `CTRL_run`) and does the controller math itself.

### Per-cycle FOC current loop (verified)

The current loop (reached via a function pointer near `0x3F1376`; FAST observer
call at `0x3F16C1`) runs, in order:

1. **ADC → Iab** — `Iab[p] = (raw_ADC[p] − offset[p]) × current_gain` in IQ24
   (the `subl … ; impyl/qmpyl ; lsl64 #8` sequence at `0x3F16A7`+). Raw phase
   currents come from the ADC-result handle; the DC-bus voltage is read too.
2. **`EST_run(estHandle, Iab, Vab, dcBus)`** — the closed FAST sensorless
   observer → flux / **angle** / **speed**.
3. `EST_getIab_pu`, **`EST_getAngle_pu`** (rotor electrical angle), `EST_getFm_pu`
   (mechanical freq) — read back.
4. **`EST_doSpeedCtrl`** / **`EST_doCurrentCtrl`** — gate the speed/current loops
   by the tick dividers.
5. Inline IQ24 **Park → PI(Id,Iq) → iPark → SVGEN** (the controller math, in
   flash) using the estimated angle.
6. **`EST_getOneOverDcBus_pu`** — DC-bus normalisation of the SVGEN output.
7. Write the three **ePWM1/2/3 compare** registers (the phase duties).

So the sensorless angle/speed come from the FAST ROM observer; the transforms,
the Id/Iq + speed PI loops, and SVGEN are motorware's own IQ24 flash code,
paced by the ePWM4 tick. Read-register 12 returns **`EST_getSpeed_krpm`**
(`0x3F96B3`) — i.e. the telemetry "speed" is the FAST-estimated motor speed.

## Communication with mainware

The S3 modules sit on a shared inter-module bus. motorware's **module type is
`0xA1`** (the header version byte), placing it alongside batteryware (`0xAA`)
and powerbankware (`0xB2`). By analogy with the other modules the link is
Modbus-RTU-over-UART with the module type as the slave address (slave `0xA1`)
**(inferred)** — to be confirmed by locating the SCI byte-level RX/TX handler
in the image and cross-checking the mainware side. See `protocol.md`.
