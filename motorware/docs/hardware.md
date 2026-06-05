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
* **8 PIE ISRs** (`0x3F2507`, `0x3F252C`, `0x3F25F7`, `0x3F261C`, `0x3F2652`,
  `0x3F2717`, `0x3F2826`, `0x3F2845`) are installed into the PIE vector table
  (via the HAL `PieVectTable` pointer at HAL `+0x78`) plus one RAM vector
  (`0x8A00`). The control work runs in ISR context: the slower ISR at
  `0x3F2717` reads 4 digital inputs (`sub_3F3B0C`) and updates the scaled
  measurements that read-register 13 reports; the fast current loop writes the
  ePWM compares. Mapping each ISR to its exact interrupt (ePWM/ADC/timer) and
  the per-step Clarke→Park→PI→SVGEN chain is the remaining control-loop work.
* **eCAN** (`0x6000`) is set up separately by `sub_3F1CE0` — role TBD (telemetry
  or a second channel).

The control/parameter state for all of the above is the L3 struct at `0x9000`+
(below).

## Communication with mainware

The S3 modules sit on a shared inter-module bus. motorware's **module type is
`0xA1`** (the header version byte), placing it alongside batteryware (`0xAA`)
and powerbankware (`0xB2`). By analogy with the other modules the link is
Modbus-RTU-over-UART with the module type as the slave address (slave `0xA1`)
**(inferred)** — to be confirmed by locating the SCI byte-level RX/TX handler
in the image and cross-checking the mainware side. See `protocol.md`.
