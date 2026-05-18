# Hardware notes — VanMoof S3 e-Shifter (MM32F031F6U6)

Running notes on what the firmware reveals about the board itself: GPIO
pin assignments, peripheral usage, and the SRAM globals the OEM uses
for shared state. Grows as decompilation progresses. For the flash /
SRAM region layout see `memory-map.md`; for peripheral on/off status
see `peripherals.md`.

## Confirmed GPIO pins

The MM32F031F6U6 is LQFP-20 — only ~16 pins are bonded out. Entries
below are confirmed from the OEM decomp (literal-pool resolution and
register-access patterns).

| Pin  | Direction | Role | Source |
| ---- | --------- | ---- | ------ |
| PA0  | Input (digital read) | **Position-encoder line.** Mirrored into `G_STATE_13B` (`0x2000013B`) at boot and re-synced on every edge by `pos_encoder_tick`: on each toggle the gear-position counter `G_STATE_115` is incremented (`G_DRIVE_DIR == 0x0F`) or decremented (`G_DRIVE_DIR == 0xF0`). Also sampled together with PA1 by Modbus `cmd 0x5B` (`cmd_5b_selftest`) which encodes `{PA0,PA1}` as a 4-state bus-readable status byte → confirms the OEM treats this pair as a debuggable input. | `input_pa0` @ `0x0800325C`, `pos_encoder_tick` @ `0x080032A4`, `cmd_5b_selftest` @ `0x08003BC4` |
| PA1  | Input (digital read) | Gates `G_STATE_FC` 2→1 demotion and the latch-into-2 path in `main`. Sampled together with PA0 by Modbus `cmd 0x5B` (see above). Likely the second of a pair with PA0 — possibly the other phase of a 2-line gear sensor. | `input_pa1` @ `0x080033CC`, `cmd_5b_selftest` @ `0x08003BC4` |
| PB6  | Output, AF0 | USART1 TX. Configured by `uart1_init` for output 50 MHz alt-push-pull (CRL nibble = 0xB). | `uart1_init` @ `0x08004130` |
| PB7  | Input, AF0 | USART1 RX. Configured for floating input (CRL nibble = 0x4). | `uart1_init` @ `0x08004130` |
| PA9  | Output (digital write) | "Forward" half of the motor H-bridge. Driven via BSRR/BRR by `motor_h_bridge_set`. Forward drive sets PA9 HIGH and PA10 LOW; reverse drive sets PA9 LOW and PA10 HIGH; brake sets both HIGH. | `motor_h_bridge_set` @ `0x080032FA` |
| PA10 | Output (digital write) | "Reverse" half of the H-bridge — pair-driven with PA9. | same |

PA0 / PA1 are the only digital inputs read in code so far. The e-Shifter
typically uses a multi-position sensor for current-gear detection — PA0
/ PA1 are very plausibly two of its lines, but a schematic or board
trace-out is needed to be certain.

PA9 / PA10 are the motor-drive outputs (a single H-bridge driving the
gear actuator). With `motor_h_bridge_set` now decomp'd, the exact
bit mapping is confirmed:
- `0xF0` → PA9 HIGH, PA10 LOW = forward drive
- `0x0F` → PA9 LOW, PA10 HIGH = reverse drive
- `0xFF` → PA9 HIGH, PA10 HIGH = brake (both H-bridge halves high
  shorts the motor through the bridge's high-side switches)
- anything else → no GPIO change (callers won't pass other values)

Drive cases also kick `pos_encoder_tick` (`0x080032A4`), which is
where the gear-position counter (`G_STATE_115`) advances and PA0 is
re-sampled into `G_STATE_13B`. The direction of the bump comes from
`G_DRIVE_DIR` (`0x20000114`, valued `0xF0`/`0x0F` to mirror the
H-bridge mask), which the round-robin task helpers latch when they
queue a shift and `state_flags_reset` clears at the tail.

### Motion-complete: sensor or timeout?

Interesting reverse-engineering finding: `G_MOTION_REACHED` is the
signal `motor_drive_step` waits on to stop the motor, but it's set
by **two** independent paths — once by the H-bridge driver itself
as a **stall timeout**, and once by something else that consumes
`G_STATE_115` (presumably a comparator against a per-task target
gear position, likely fired from `FUN_08003608`/`sched_task_alpha`
or a still-undecomped EXTI on PA0). The timeout limits inside
`motor_h_bridge_set` are:
- 200 ticks of `G_TICK_B` if the active round-robin task is `#2`
  (state-task 2 is presumably the "active shift" task);
- 2000 ticks otherwise.

So even if the position sensor on PA0 misses an edge, the motor is
guaranteed to be braked after at most ~2 seconds (assuming
`G_TICK_B` runs at ~1 kHz, which is the typical SysTick on this
MCU). That gives the protocol a hard upper bound on how long a
shift command can keep the bridge energised.

## Confirmed peripheral instances

| Peripheral | Base | Used for | Source |
| ---------- | ---- | -------- | ------ |
| TIM2       | `0x40000000` | Boot-configured for a 1 kHz periodic update IRQ (prescaler = `HCLK/100000 - 1`, ARR = 99). NVIC IRQ 15, priority 1. Brought up by `tim2_init_periodic` from `main`'s boot prologue. Clock gated on via RCC APB1ENR bit 0. | `tim2_init_periodic` @ `0x08004048` |
| USART1     | `0x40013800` | Modbus RTU link to main module (9600 baud, 8-N-1). RX IRQ at IRQ27, priority 3. | `uart1_init` |
| GPIOA      | `0x48000000` | Digital inputs PA0, PA1 (above). | `gpio_idr_test` callers |
| GPIOB      | `0x48000400` | USART1 TX/RX. Clock gated on via RCC AHBENR bit 18. | `uart1_init` |
| RCC        | `0x40021000` | SYSCFG enabled (APB2ENR bit 0); USART1 clock (APB2ENR bit 14); GPIOB clock (AHBENR bit 18); CRC clock (AHBENR bit 6). | `main` + `uart1_init` |
| CRC        | `0x40023000` | Hardware CRC32 for image-validation in `image_verify_crc`. | `crc_reset`, `crc32_word`, `crc32_words` |
| FLASH (controller) | `0x40022000` | OTA staging — page erase + word program. Status codes 1=BUSY 2=PGERR 3=WRPRTERR 4=READY. | `flash_*` family |
| SCB (Cortex-M0) | `0xE000ED00` | `SYSRESETREQ` after a validated OTA, via `AIRCR = 0x05FA0004`. | `modbus_tx_finalize` |
| SYSCFG     | `0x40010000` | Boot prologue writes `MEM_MODE` field to 3 (MM32-specific encoding) before enabling interrupts. Clock gated on via RCC APB2ENR bit 0. | `syscfg_set_mem_mode` @ `0x080052E8` |

## MCU register-layout note

The MM32F031 uses **STM32F1-style** layouts for some peripherals where
the speculative pre-decomp headers assumed STM32F0:

- **USART** (rebuilt as `usart_t` in `mm32f031.h`): TDR @ 0x00, RDR @
  0x04, SR @ 0x08, ISR @ 0x0C, IER @ 0x10, ICR @ 0x14, CCR @ 0x18,
  FCR @ 0x1C, BRR_INT @ 0x20, BRR_FRA @ 0x24. RX-ready flag is
  ISR bit 1 (mask 0x02). Confirmed via OEM decomp.
- **GPIO**: CRL @ 0x00, CRH @ 0x04 (4 bits per pin = MODE+CNF, STM32F1
  style), IDR @ 0x08, ODR @ 0x0C, BSRR @ 0x10, BRR @ 0x14, AFRL @
  0x20, AFRH @ 0x24. The speculative `gpio_t` struct in `mm32f031.h`
  still has the STM32F0 layout (separate MODER/OTYPER/OSPEEDR/PUPDR
  @ 2 bits/pin); plan-2 will fix this. OEM-confirmed accessors
  currently use raw byte offsets.

## RAM globals (SRAM @ `0x20000000` – `0x20000FFF`)

Sorted by address. Names are descriptive of observed behaviour, not
ground truth. Every entry has been seen across the decomp; entries
without a comment are still single-purpose unknowns.

### Bus / dispatch state

| Addr | Type | Name in C | Behaviour |
| ---- | ---- | --------- | --------- |
| `0x200000C4` | `uint32_t` | `MODBUS_TICK_CTR` | Inter-byte timeout countdown; decremented by `modbus_tick`. Resets to 0 on each RX byte (see `USART1_IRQHandler`). |
| `0x200000C8` | `uint8_t[8]` | `G_RX_BUF` | Short-frame validated buffer (8 B = 6 PDU + 2 CRC). Filled by `modbus_rx_poll` after CRC passes. Read by case handlers in `modbus_dispatch_pdu`. |
| `0x200000D0` | `uint32_t` | `G_TICK_B` | "Slow" tick counter, advanced by an ISR (likely SysTick). The super-loop compares `G_TICK_A != G_TICK_B` to detect a fresh tick. |
| `0x200000D4` | `uint32_t` | `G_TICK_D4` | A second tick counter, cleared together with `G_TICK_B` at the 2000-tick rollover. |
| `0x200000D8` | `uint8_t` | `G_REQ_PENDING` | 1 = a PDU has been validated and is awaiting dispatch; cleared at the end of `modbus_dispatch_pdu`. The dispatcher's own entry guard also clears it if it sees anything but 1. |
| `0x200000D9` | `uint8_t` | `G_RX_FRAME_MODE` | 0 = expecting short PDU (8 B); 1 = long PDU (45 B, OTA). Switched to 1 by case 0x95 (ERASE) and back to 0 by `modbus_rx_poll`'s OTA-timeout / OTA-CRC-fail paths. |
| `0x200000DC` | `uint32_t` | `G_RX_WAIT_CTR` | End-of-frame timeout counter consumed by `modbus_rx_poll`. Compared against `0x00249F00` (≈ 2.4 M). Reset to 0 on each RX byte. |
| `0x200000E0` | `uint32_t` | `G_OTA_WRITE_PTR` | OTA staging write pointer (starts at flash slot `0x08001800`). |
| `0x200000E4` | `uint32_t` | `G_RX_HEAD` | Count of bytes received this frame (0..0x2D). Index used by `USART1_IRQHandler` to append into `G_RX_SCRATCH`. |
| `0x200000E6` | `uint32_t` | `G_LOOP_IDX` | Throwaway loop index used during scratch→validated-buffer copies. |
| `0x200000E7` | `uint8_t` | `G_CRC_LO` | Output of `modbus_crc16_compute` — low byte. |
| `0x200000E8` | `uint8_t` | `G_CRC_HI` | Output of `modbus_crc16_compute` — high byte. |
| `0x200000E9` | `uint8_t` | `G_IMG_STATUS` | Cached return of `image_verify_crc`. Set by `image_apply`, read by `report_image_status`. |
| `0x200000EA` | `uint8_t` | `G_5A_TARGET` | Inbound shift command (`cmd 0x5A` payload byte): `0 = forward`, `1 = reverse`. The motor servoing step (`motor_drive_step`) reads this each iteration and latches it to `2` once `G_MOTION_REACHED` fires, marking the move complete. |

### Image / OTA scratch

| Addr | Type | Name | Behaviour |
| ---- | ---- | ---- | --------- |
| `0x200000DA` | `uint8_t` | `G_IMG_OK_FLAG` | Latched to 1 by `image_apply` on a clean validation; consumed by `modbus_tx_finalize` to gate the SYSRESETREQ-after-7-byte-TX. |
| `0x2000015C` | `uint8_t[45]` | `G_LONG_BUF` | Long-frame validated buffer (45 B = 43 PDU + 2 CRC). Same role as `G_RX_BUF` but for OTA payloads. |
| `0x20000189` | `uint8_t[33]` | `G_OTA_HEADER_BUF` | Staged per-packet copy of `G_LONG_BUF[0xb..0x2b]`. The first packet's header (containing the LE16 total size at `[0xc..0xd]`) and each subsequent payload chunk land here before `ota_commit_chunk` programs them to flash via `flash_program_range`. |
| `0x200001A9` | `uint8_t[8]` | `MODBUS_TX_BUF` | Module-local TX assembly buffer. Built by `report_image_status` and `modbus_reply_passthrough`; transmitted by `modbus_tx_finalize`. |
| `0x200001B2` | `uint8_t[45]` | `G_RX_SCRATCH` | The IRQ-filled inbound byte buffer (max 45 B). First byte must be `0x20` for `modbus_rx_poll` to accept the frame; otherwise the head counter is reset and the frame is dropped. |
| `0x200001E0` | `uint16_t[16]` | `G_OTA_STAGE_HW` | 32-byte halfword reformatter used by `ota_stage_chunk` — repacks the byte stream from `G_OTA_HEADER_BUF` into halfwords (`lo | (hi<<8)` per pair), then `flash_program_range` writes the whole batch to flash in one call. |

### Application state

| Addr | Type | Name | Behaviour |
| ---- | ---- | ---- | --------- |
| `0x200000F8` | `uint32_t` | `G_COUNTER` | Incremented in cmd 0x14 (when `G_MOTOR_RUNNING == 0`). Emitted big-endian by cmd 0x0F via `cmd_0f_report_u32` → `FUN_08003C68` → `FUN_08003C1C`. Reset to 0 by `sched_idle_reset` whenever the state machine transitions through `G_STATE_FC == 0`. |
| `0x200000FC` | `uint8_t` | `G_STATE_FC` | Master state byte; values 0..6 select which round-robin task runs each tick. Read by `sched_pick_task`. Promoted 0→1 by `sched_idle_reset` (case-0 self-exit); demoted 2→1 by `motor_drive_step` after motion-reached; latched to 2 by `main`'s pre-loop sync the first time PA1 reads low. |
| `0x20000100..02` | `uint8_t[3]` | `G_5C_REGS` | 3-byte register block written by cmd 0x5C / len 0x0F (from `G_RX_BUF[2,4,5]`); read back by cmd 0x5C / len 3. |
| `0x20000104` | `uint32_t` | `G_TICK_A` | "Compare" tick counter advanced in lockstep with `G_TICK_B` by the super-loop. |
| `0x20000108` | `uint32_t` | `G_TICK_PREV_B` | Last-iteration snapshot of `G_TICK_B`; used for the 2000-tick rollover detector. |
| `0x20000110` | `uint32_t` | `G_5C_DEADLINE_BASE` | Tick value captured when the 5C-busy latch goes high; the consumer fires when `G_TICK_B - this == 0x32`. |
| `0x20000114` | `uint8_t` | `G_DRIVE_DIR` | Active drive-direction byte, encoded as the H-bridge mask (`0xF0` = forward, `0x0F` = reverse, `0x00`/anything else = idle). Written by the round-robin task helpers (`sched_task_alpha/beta/extra`, `FUN_08003538`) when they queue a shift; consumed by `pos_encoder_tick` via the tri-state decoder `drive_dir_code` to know which direction each PA0 edge represents; cleared by `state_flags_reset` at end-of-task. |
| `0x2000010C` | `uint32_t` | `G_MOTOR_RUN_START` | `G_TICK_B` snapshot at the moment the H-bridge was last energised. Used by `motor_h_bridge_set` to time the stall-timeout fallback. |
| `0x20000115` | `uint8_t` | `G_STATE_115` | **Gear-position counter.** Latched to 1 during pre-loop sync the first time PA1 reads low; reset back to 1 by `sched_idle_reset` whenever the state machine cycles through `G_STATE_FC == 0`; thereafter incremented (reverse drive) or decremented (forward drive) by `pos_encoder_tick` on each PA0 edge. The "motor arrived" path presumably compares this against a target value. |
| `0x20000116` | `uint8_t` | `G_FLAG_116` | Snapshot of `G_FLAG_117` in the "extra task" branch of the round-robin. |
| `0x20000117` | `uint8_t` | `G_FLAG_117` (a.k.a. `G_14_FLAG_A`) | Set to 1 by cmd 0x14 (when `G_MODE == 0`). |
| `0x20000118` | `uint8_t` | `G_TASK_ID` | Stamped to one of {2, 4, 5, 7, 8} by the round-robin case handlers — a "what task ran this tick" marker. |
| `0x2000011C` | `int32_t` | `G_SHIFT_ATTEMPT_CTR` | Per-shift retry counter. Incremented after each motion-reached cycle in `sched_task_beta`; when it hits 3 the OEM demotes `G_STATE_FC` to 6 (a give-up state). Cleared by the PA1-low home-reset branch of `sched_task_beta`. Signed because the OEM compares with `cmp ; blt`. |
| `0x20000128` | `uint32_t` | `G_OTA_TOTAL_SIZE` | Total firmware-payload byte count, set on the first cmd 0x82 packet from the LE16 in `G_OTA_HEADER_BUF[0xc..0xd]`. |
| `0x2000012C` | `uint32_t` | `G_OTA_REMAINING` | Bytes still to flash; seeded as `G_OTA_TOTAL_SIZE - 0x20` and decremented by 0x20 per full chunk. When `< 0x20` the next packet is the final partial. |
| `0x20000130` | `uint8_t` | `G_MOTOR_RUN_LATCH` | 1 once `G_MOTOR_RUN_START` has been captured this run; set by `motor_h_bridge_set` on its first call after energising. Cleared by `state_flags_reset` at the end of each state-task. |
| `0x20000134` | `uint32_t` | `G_SETTLE_TICK_BASE` | `G_TICK_B` snapshot at the moment `sched_alpha_match_3538` first arms its per-gear settling-time window. Compared against `G_TICK_B` on each subsequent call until the per-(gear, direction) threshold elapses (23/30/35-or-25/40 ms at 1 kHz). |
| `0x20000138` | `uint8_t` | `G_SETTLE_ARMED` | 1 once `G_SETTLE_TICK_BASE` is valid; cleared by `sched_alpha_match_3538` after the settling window elapses and the H-bridge has been braked. |
| `0x20000131` | `uint8_t` | `G_5C_LATCH_BYTE` | 1 once the 5C-consumer deadline has been captured; set by `main`'s post-loop bookkeeping, cleared after `FUN_080031E6` fires. |
| `0x20000139` | `uint8_t` | `G_MOTOR_RUNNING` | 1 = H-bridge driving (set by `motor_h_bridge_set`), 0 = braked/idle. Gates cmd 0x14 and cmd 0x5A handlers (so the bike can't preempt an active shift) and the 2000-tick rollover in `main`. |
| `0x2000013A` | `uint8_t` | `G_MOTION_REACHED` | The "motor has arrived" signal that `motor_drive_step` waits on. Set from **two** independent paths: (a) `motor_h_bridge_set`'s stall-timeout fallback (200 ticks in task #2, 2000 otherwise), and (b) some not-yet-decomp'd consumer of the position sensor (likely an EXTI ISR). Cleared by `state_flags_reset`. |
| `0x2000013B` | `uint8_t` | `G_STATE_13B` | Mirror of the PA0 level used as the edge-detection reference. Seeded from `input_pa0()` at boot and re-synced by `pos_encoder_tick` after every counted edge. |
| `0x2000013C` | `uint8_t` | `G_5C_BUSY` | 1 = the 0x32-tick 5C-consume countdown is active in `main`'s post-loop bookkeeping. |
| `0x2000013D` | `uint8_t` | `G_FLAG_13D` (a.k.a. `G_14_FLAG_B`) | Set/cleared by cmd 0x14 depending on `G_MODE`; gates `sched_task_beta` in several round-robin cases. |
| `0x2000013E` | `uint8_t` | `G_FLAG_13E` | Set to 1 by the "extra task" branch of the round-robin (case 1 epilogue). |
| `0x2000013F` | `uint8_t` | `G_OTA_FIRST_PACKET` | 0 = next cmd 0x82 frame is the first packet (decode header + total-size), 1 = subsequent. Cleared by `image_apply`'s erase-path and by `ota_commit_chunk` on the final chunk. Was `G_OTA_OFF` lo half in the speculative phase. |
| `0x20000140` | `uint8_t` | `G_OTA_FLUSH_FLAG` | 1 = the next `ota_commit_chunk` is the final partial chunk (`G_OTA_REMAINING < 0x20`). Armed by `cmd_82_fw_page`, consumed + cleared by `ota_commit_chunk`. Was `G_OTA_OFF` hi half in the speculative phase. |
| `0x20000141` | `uint8_t` | `G_VERSION_BYTE` (a.k.a. `G_0F_SUBID` in the cmd 0x0F path) | Bits 1..7 of a big-endian uint16 lifted out of `G_RX_BUF[4..5]`. Written by `image_apply`, `cmd_5c_write3` (with `G_5C_REGS[0]`), and `cmd_0f_report_u32`. Emitted as PDU byte 2 by both `report_image_status` (7-byte) and `emit_counter_status_pdu` (9-byte). |
| `0x20000142..43` | `uint8_t[2]` | `G_PKT_BYTES` (also the first 2 bytes of `G_0F_VALUE_BE` in the cmd 0x0F path) | Pair of bytes emitted as PDU bytes 3 and 4 by `report_image_status`. Written by `image_apply` (`[0] = 0`, `[1] = G_IMG_STATUS`) or by `cmd_5c_write3` with the trailing two bytes of `G_5C_REGS`. Clobbered by `cmd_0f_report_u32` (which writes a 4-byte BE32 value spanning `0x20000142..0x20000145`). |
| `0x20000144..45` | `uint8_t[2]` | tail of `G_0F_VALUE_BE` | Upper two bytes of the BE32 value staged by `cmd_0f_report_u32`. Not used by the image-status path. Emitted as PDU bytes 5 and 6 of the 9-byte cmd 0x0F response. |
| `0x20000148` | `uint32_t` | `G_HCLK_HZ` | **HCLK frequency in Hz** (typically 48,000,000). Set during boot bring-up (`boot_init_periphs_b`, pending) and read by `main` (to derive the TIM2 prescaler: `psc = HCLK/100000 - 1`) and by `boot_init_periphs_a` (the SysTick init wrapper, also pending: SysTick LOAD = HCLK/1000 - 1). The pre-decomp model labelled this `G_HASH_SEED_PTR`; that name was an artifact of mis-identifying the consuming functions. |

The 0xC0-byte block at `0x20000000` is `.data`, copied by `main` from
flash `0x08004828`. Many of the entries above sit inside it.

## Persistent storage (on-chip flash)

The MM32F031F6U6 has 32 KB of internal flash (`0x08000000..0x08008000`)
split as follows in shifterware:

| Range | Use | Owner |
| ----- | --- | ----- |
| `0x08000000..0x080017FF` (≈6 KB) | shifterboot loader | _out of scope_ |
| `0x08001800..0x080037FF` (12 KB) | OTA receive slot | erased by `flash_erase_pages(0x08001800, 12)` (cmd 0x95); validated by `image_verify_crc`; latched on success by `image_apply` |
| `0x08003800..0x080077FF` | shifterware code + read-only data | this image |
| **`0x08007800..0x08007BFF`** (1 KB) | **settings page (`FLASH_SETTINGS_PAGE`)** | written by `flash_settings_commit` via `settings_set_halfword`; holds 8 halfwords (16 B used) carrying `G_STATE_FC` + BE32 `G_COUNTER` + `G_5C_REGS[0..2]` |
| `0x08007C00..0x08007FFF` (1 KB) | **calibration page (`NVM_BASE`)** | speculative — read/written by `nvm.c`; **not yet observed in OEM image** |

Notes:
- The settings page uses one halfword per byte of payload (high byte
  stays at `0x00` after erase = `0xFFFF`, then programmed to the data
  byte | `0x0000`). That's 2:1 waste, but matches the OEM byte sequence.
- Every call to `settings_set_halfword` does its own page erase before
  reprogramming the full 8-halfword record, so a single
  `flash_settings_commit` triggers **8 consecutive page erases** of
  `0x08007800`. Flash-endurance hostile but verbatim from OEM.
- `nvm.c`'s use of the last page (`0x08007C00`) is scaffolding from a
  pre-decomp guess at how calibration would be stored; the OEM appears
  to put everything bus-writable through the `0x08007800` page instead.
  Don't take `nvm.c`'s layout as load-bearing.

## Booted-but-unidentified resources

These are touched by the OEM but not yet placed:

- `0x200000F9..0xFB` — bytes adjacent to `G_COUNTER` that may form
  part of a larger telemetry struct. `G_COUNTER` itself is the 32-bit
  value emitted by `cmd_0f_report_u32` and persisted big-endian by
  `flash_settings_commit`. Byte `0x200000FC` is `G_STATE_FC` (the
  operating-mode state byte; values 0..2), persisted as halfword 0
  of the settings page.
- `0x2000015C` and the surrounding 45 B are the long-frame buffer,
  but the OTA payload format inside it (after the 4-byte Modbus
  header) hasn't been mapped.
- `0x20000148` points at *something* in flash — the boot-time hash
  loops over 100,000 bytes from there. 100,000 > available flash
  from `0x08001800`, so the pointer likely targets an out-of-image
  region (shifterboot's resources, or RAM-resident data).

## Sources

The numbers above come from the literal pools at:
- `0x080041FC..0x0800426F` — the dispatcher / RX-FSM region
- `0x0800454C..0x080045B7` — `main`'s pool
- `0x08003944..0x08003958` — `sched_pick_task` and friends
- `0x080045A4..0x080045B4` — `USART1_IRQHandler` and `modbus_tick`

Use `~/ghidra_scripts/PeekBytes.java 0xADDR N` to re-derive any entry
when the decomp shape changes.
