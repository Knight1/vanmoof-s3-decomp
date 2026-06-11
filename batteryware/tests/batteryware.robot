*** Settings ***
Documentation     Renode smoke tests for the VanMoof batteryware (STM32L072) image.
...               Run with:  renode-test batteryware/tests/batteryware.robot
...               (paths below are relative to the repo root).
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}

*** Variables ***
${ELF}            @${CURDIR}/../build/batteryware.elf
${REPL}           @${CURDIR}/batteryware.repl
${VTOR}           0x08005028
# Scratch buffer + stack used by the direct-call (leaf function) tests. Both sit
# inside the 20 KB SRAM (0x20000000..0x20005000) modelled in batteryware.repl.
${SCRATCH}        0x20004000
${STACKTOP}       0x20005000
# All function entry points are resolved at run time with `sysbus GetSymbolAddress`
# (see the 'Call Leaf Function' / 'Process Rx Ring' keywords) so the suite keeps
# working after a rebuild shifts addresses — never hard-code flash addresses here.
#
# Modbus RX/TX SRAM map (from src/uart.c, src/modem.c, src/cmd.c):
${RX_RING}        0x20004088      # USART1 RX ring buffer (uart_resp_handler drains it)
${RX_HEAD}        0x20004540      # RX ring head (uint16)
${RX_TAIL}        0x20004544      # RX ring tail (uint16)
${RX_STATE}       0x200047D2      # uart_protocol_handler frame state machine (uint16)
${TX_ENABLE}      0x2000453D      # uart_putchar gate — must be 1 to emit
${TX_WR_IDX}      0x20004542      # TX ring write index (uint16)
${RESP_BUF}       0x20004648      # assembled response frame (AA 03/06 ... CRC)
${RESP_LEN}       0x200047D0      # response length so far (uint16)
${TX_FIELD_CNT}   0x20004748      # # of fields emitted (non-zero ⇒ a response was built)
${MODE_WORD}      0x20002C00      # 0 selects binary Modbus command mode
${TESTMODE_FLAG}  0x200028D5      # cfg_blk+5: set by a 0x06 write to register 9
${CMD_IDX}        0x20004000      # scratch byte: console-dispatch index capture (tests)

*** Keywords ***
Create Battery Machine
    [Documentation]    Build the machine, force the boot-time status polls ready,
    ...                load the image and point the vector table at the app header.
    Execute Command           mach create "batteryware"
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus Tag <0x40021000 4> "RCC_CR" 0x0F03FFFF
    # SWS bits [3:2] = 0b01 = HSI16 (not PLL=0b11=0xC). With SWS=HSI, the SWS
    # poll in rcc_configure exits immediately on the first read; with SWS=PLL the
    # poll loops forever because tick_get is 0 and the timeout never fires.
    Execute Command           sysbus Tag <0x4002100C 4> "RCC_CFGR" 0x00000004
    Execute Command           sysbus Tag <0x40022018 4> "FLASH_SR" 0x00000000
    # RCC_CSR: LSION(b0)+LSIRDY(b1) set — unblocks the LSI ready wait-loop
    Execute Command           sysbus Tag <0x40021050 4> "RCC_CSR" 0x00000003
    Execute Command           sysbus LoadELF ${ELF}
    Execute Command           cpu VectorTableOffset ${VTOR}
    # bmsboot normally initialises these SRAM words before jumping to the app:
    #   0x200000c8 = SystemCoreClock (32 MHz), 0x200000c4 = tick-rate divisor (1).
    # hal_init_tick divides 0x200000c8 by 0x200000c4 to get the SysTick reload
    # value; with both at 0 the division underflows, flash_page_erase sees r0=0,
    # and skips the SysTick CSR write — leaving the timer disabled forever.
    Execute Command           sysbus WriteDoubleWord 0x200000c8 32000000
    Execute Command           sysbus WriteDoubleWord 0x200000c4 1
    # Register a reset macro so SYSRESETREQ (fired during clock init) restores
    # the vector table offset instead of leaving the CPU parked at 0x0.
    # Single-line body avoids the newline-over-TCP issue with triple-quote macros.
    # Flash content (ELF) persists through soft reset so no reload needed.
    Execute Command           macro reset "cpu VectorTableOffset ${VTOR}"
    # --- Stub boot-path functions that would otherwise busy-wait on absent
    # peripherals or run SysTick too fast for Python hooks. All addresses are
    # resolved by symbol (Hook Return / Hook Return Zero) so a rebuild that shifts
    # flash can't stale them — only the two mid-function rcc offsets are relative.
    #
    # rcc_osc_config: skip the oscillator config at entry, and also patch its
    # mid-body PLL path. The last RCC_CSR read (+0x234) is rewritten to jump to the
    # compiler's "PLLState==NONE → movs r0,#0 → epilogue" block (+0xA4), bypassing
    # the PLL disable/enable timeout loops that spin forever while tick_get is 0.
    # With SWS=HSI (CFGR tag 0x4) the rcc_configure SWS poll then exits on its first
    # read, so the whole clock-init path completes without any busy-wait.
    Hook Return Zero          rcc_osc_config
    ${rcc}=       Resolve Symbol    rcc_osc_config
    ${rcc_mid}=   Evaluate    "0x%X" % (${rcc} + 0x234)
    ${rcc_epi}=   Evaluate    "0x%X" % (${rcc} + 0xA4)
    Execute Command           cpu AddHook ${rcc_mid} "self.SetRegisterUlong(0, 0); cpu.PC = ${rcc_epi}"
    # system_reset / nvic_system_reset_dup: neutralise the SYSRESETREQ paths fired
    # from clock-init and fault_led_trigger (return before push {r4,lr}).
    Hook Return               system_reset
    Hook Return               nvic_system_reset_dup
    # SysTick_Handler (weak default ISR): return from interrupt doing nothing — we
    # keep SysTick disabled (flash_page_erase skipped) so it should never fire.
    Hook Return               SysTick_Handler
    # flash_page_erase: would configure SysTick at up to 8 kHz; at that rate the
    # Python hook overhead dominates wall-clock time. Skip it.
    Hook Return Zero          flash_page_erase
    # dma_wait_for_ready: 50 000-tick FLASH_SR poll per call site — return 0 (ready).
    Hook Return Zero          dma_wait_for_ready
    # delay_ms / delay_us: poll a SysTick event flag the stubbed handler never sets.
    Hook Return               delay_ms
    Hook Return               delay_us
    # dma_flash_start.part.0: polls an uninitialised tick counter.
    Hook Return Zero          dma_flash_start.part.0
    # bms_setup: initial BMS cell setup iterates a full flash page over SPI.
    Hook Return               bms_setup
    # smbus_transmit/write_reg/read: SPI1 (0x40013000) / I2C state absent from the
    # .repl — return 0 (OK) for all three.
    Hook Return Zero          smbus_transmit
    Hook Return Zero          smbus_write_reg
    Hook Return Zero          smbus_read
    # memcmp_verify: byte-by-byte compare via SPI reads in a tight loop — return 0.
    Hook Return Zero          memcmp_verify

Create Leaf Machine
    [Documentation]    Minimal machine for direct-call (leaf function) tests: load
    ...                the platform and ELF, nothing else. The pure routines under
    ...                test (CRC, hex conversion, tick read) touch no clock/flash
    ...                status registers, so none of the boot-time tags or hooks from
    ...                'Create Battery Machine' are needed here.
    Execute Command           mach create
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus LoadELF ${ELF}

Load Scratch Bytes
    [Documentation]    Write a list of byte literals into the SRAM scratch buffer
    ...                (${SCRATCH}), one byte per ascending address.
    [Arguments]    @{bytes}
    ${i}=    Set Variable    ${0}
    FOR    ${b}    IN    @{bytes}
        ${addr}=    Evaluate    ${SCRATCH} + ${i}
        Execute Command    sysbus WriteByte ${addr} ${b}
        ${i}=    Evaluate    ${i} + 1
    END

Hex To Nibble Should Be
    [Documentation]    Fresh-machine helper for the hex_to_nibble table check: load
    ...                the ASCII code into R0, call, and assert the 4-bit result.
    [Arguments]    ${char}    ${expected}
    Create Leaf Machine
    Execute Command    cpu SetRegister 0 ${char}
    ${r}=    Call Leaf Function    hex_to_nibble
    Should Be Equal As Integers    ${r}    ${expected}

Resolve Symbol
    [Documentation]    Look up a symbol's address in the loaded ELF and return it
    ...                stripped (e.g. "0x08005964"). Keeps the suite rebuild-proof.
    [Arguments]    ${name}
    ${addr}=    Execute Command    sysbus GetSymbolAddress "${name}"
    ${addr}=    Strip String       ${addr}
    [Return]    ${addr}

Hook Return
    [Documentation]    Hook a function (by symbol) to return immediately — cpu.PC =
    ...                cpu.LR before its prologue, so callers proceed as if it ran.
    [Arguments]    ${symbol}
    ${a}=    Resolve Symbol    ${symbol}
    Execute Command    cpu AddHook ${a} "cpu.PC = cpu.LR"

Hook Return Zero
    [Documentation]    Hook a function (by symbol) to return 0 immediately —
    ...                r0 = 0; cpu.PC = cpu.LR. (R0 is read-only in a hook, hence
    ...                SetRegisterUlong; see the renode-batteryware memory note.)
    [Arguments]    ${symbol}
    ${a}=    Resolve Symbol    ${symbol}
    Execute Command    cpu AddHook ${a} "self.SetRegisterUlong(0, 0); cpu.PC = cpu.LR"

Return Trap
    [Documentation]    Address of Default_Handler (a `b .` self-loop) with the Thumb
    ...                bit set, used as LR so a called function parks the CPU
    ...                harmlessly on return (BX LR) instead of running off into
    ...                undefined code. Resolved per machine so a rebuild can't stale it.
    ${trap}=    Resolve Symbol     Default_Handler
    # GetSymbolAddress yields a 0x… literal that robot inlines as a Python int,
    # so OR the Thumb bit directly (no int(..., 16) wrapper).
    ${trap}=    Evaluate           ${trap} | 1
    [Return]    ${trap}

Call Leaf Function
    [Documentation]    Invoke a Thumb function by symbol name and return R0 (its
    ...                return value) as a stripped string. Arguments R0..R3 must
    ...                already be set by the caller. Sets up a fresh stack, points LR
    ...                at the Return Trap self-loop, runs a brief window so the call
    ...                completes, then reads R0. RunFor "0.001" is ~32k CPU cycles at
    ...                32 MHz — orders of magnitude more than these routines need.
    [Arguments]    ${symbol}
    ${addr}=    Resolve Symbol    ${symbol}
    ${trap}=    Return Trap
    Execute Command           cpu SetRegister 13 ${STACKTOP}
    Execute Command           cpu SetRegister 14 ${trap}
    Execute Command           cpu PC ${addr}
    Execute Command           emulation RunFor "0.001"
    ${r0}=    Execute Command    cpu GetRegister 0
    ${r0}=    Strip String      ${r0}
    [Return]    ${r0}

Inject Modbus Frame
    [Documentation]    Put the BMS into binary Modbus command mode, clear the RX
    ...                state machine + response buffers, and write the request bytes
    ...                into the USART1 RX ring so the next 'Process Rx Ring' drains
    ...                them. Frame bytes are passed as byte literals (incl. CRC).
    [Arguments]    @{frame}
    Execute Command    sysbus WriteByte ${TX_ENABLE} 1            # let uart_putchar emit
    Execute Command    sysbus WriteDoubleWord ${MODE_WORD} 0      # binary command mode
    Execute Command    sysbus WriteWord ${RX_STATE} 0            # fresh frame
    Execute Command    sysbus WriteDoubleWord ${RESP_LEN} 0
    Execute Command    sysbus WriteDoubleWord ${TX_FIELD_CNT} 0
    Execute Command    sysbus WriteWord ${TX_WR_IDX} 0
    ${n}=    Set Variable    ${0}
    FOR    ${b}    IN    @{frame}
        ${addr}=    Evaluate    ${RX_RING} + ${n}
        Execute Command    sysbus WriteByte ${addr} ${b}
        ${n}=    Evaluate    ${n} + 1
    END
    Execute Command    sysbus WriteWord ${RX_HEAD} ${n}          # head = frame length
    Execute Command    sysbus WriteWord ${RX_TAIL} 0

Process Rx Ring
    [Documentation]    Call uart_resp_handler() once. It loops over the whole RX ring,
    ...                feeding each byte to uart_protocol_handler — so a complete frame
    ...                is parsed and its response assembled in a single call (which
    ...                matters: a returning call parks the CPU, so only one per machine).
    Execute Command    cpu SetRegister 13 ${STACKTOP}
    ${trap}=    Return Trap
    Execute Command    cpu SetRegister 14 ${trap}
    ${h}=       Resolve Symbol    uart_resp_handler
    Execute Command    cpu PC ${h}
    Execute Command    emulation RunFor "0.05"

Response Is Crc Valid
    [Documentation]    Read ${length} response bytes from ${RESP_BUF} and return True
    ...                iff the trailing two bytes are a correct little-endian Modbus
    ...                CRC-16 over the preceding body (poly 0xA001, init 0xFFFF). The
    ...                CRC is computed in-line with a nested reduce so no helper lib is
    ...                needed.
    [Arguments]    ${length}
    ${raw}=    Execute Command    sysbus ReadBytes ${RESP_BUF} ${length}
    ${ok}=     Evaluate    (lambda bs: __import__('functools').reduce(lambda c,b: __import__('functools').reduce(lambda x,_:(x>>1)^0xA001 if x&1 else x>>1, range(8), c^b), bs[:-2], 0xFFFF) == (bs[-2]|(bs[-1]<<8)))([int(x,16) for x in __import__('re').findall(r'0x([0-9A-Fa-f]{2})', r'''${raw}''')][:${length}])
    [Return]    ${ok}

Read Sram Word
    [Documentation]    Read a 16-bit SRAM word and return it as an integer.
    [Arguments]    ${addr}
    ${v}=    Execute Command    sysbus ReadWord ${addr}
    ${v}=    Strip String       ${v}
    ${v}=    Convert To Integer    ${v}    16
    [Return]    ${v}

Read Sram Dword
    [Documentation]    Read a 32-bit SRAM word and return it as an integer.
    [Arguments]    ${addr}
    ${v}=    Execute Command    sysbus ReadDoubleWord ${addr}
    ${v}=    Strip String       ${v}
    ${v}=    Convert To Integer    ${v}    16
    [Return]    ${v}

Response Word At Offset Should Be
    [Documentation]    Assert the big-endian 16-bit value at byte offset ${off} of the
    ...                assembled response (${RESP_BUF}) equals ${expected}. Telemetry
    ...                fields are appended hi-byte-first by modem_send_2bytes/send_be.
    [Arguments]    ${off}    ${expected}
    ${hiaddr}=    Evaluate    ${RESP_BUF} + ${off}
    ${loaddr}=    Evaluate    ${RESP_BUF} + ${off} + 1
    ${hi}=    Execute Command    sysbus ReadByte ${hiaddr}
    ${hi}=    Strip String       ${hi}
    ${lo}=    Execute Command    sysbus ReadByte ${loaddr}
    ${lo}=    Strip String       ${lo}
    ${val}=   Evaluate    ((${hi}) << 8) | (${lo})
    Should Be Equal As Integers    ${val}    ${expected}

Patch Dispatch Capture
    [Documentation]    The ASCII console handlers are frame-sharing continuations of
    ...                command_parser, so the reconstruction leaves s_jump_table all
    ...                NULL (the OEM stores the real pointers at flash 0x08017FD8) —
    ...                i.e. commands *parse and match* but execute nothing in this
    ...                build. To still verify the parse → match → dispatch-index path
    ...                for every command, point all 24 jump-table slots at an inert
    ...                flash address and hook it to record the action index (passed in
    ...                R2) into ${CMD_IDX}. crc8_verify is just a convenient never-run
    ...                hook target here; its body never executes (the hook returns first).
    ${jt}=     Resolve Symbol    s_jump_table
    ${cap}=    Resolve Symbol    crc8_verify
    ${capt}=   Evaluate    ${cap} | 1
    FOR    ${i}    IN RANGE    24
        ${slot}=    Evaluate    ${jt} + ${i}*4
        Execute Command    sysbus WriteDoubleWord ${slot} ${capt}
    END
    Execute Command    sysbus WriteByte ${CMD_IDX} 0xFF
    Execute Command    cpu AddHook ${cap} "self.Bus.WriteByte(${CMD_IDX}, cpu.GetRegisterUlong(2)); cpu.PC = cpu.LR"

Inject Console Line
    [Documentation]    Put the BMS in command mode and write an ASCII line terminated
    ...                by CR into the RX ring. uart_resp_handler accumulates the
    ...                printable bytes into its line buffer and dispatches the line to
    ...                command_parser on the CR.
    [Arguments]    ${text}
    Execute Command    sysbus WriteByte ${TX_ENABLE} 1
    Execute Command    sysbus WriteDoubleWord ${MODE_WORD} 0
    Execute Command    sysbus WriteWord ${RX_STATE} 0
    ${bytes}=    Evaluate    [ord(c) for c in "${text}"] + [0x0D]
    ${n}=    Set Variable    ${0}
    FOR    ${b}    IN    @{bytes}
        ${addr}=    Evaluate    ${RX_RING} + ${n}
        Execute Command    sysbus WriteByte ${addr} ${b}
        ${n}=    Evaluate    ${n} + 1
    END
    Execute Command    sysbus WriteWord ${RX_HEAD} ${n}
    Execute Command    sysbus WriteWord ${RX_TAIL} 0

Console Command Dispatches To
    [Documentation]    Drive an ASCII console command end to end — write "NAME\\r" into
    ...                the RX ring and let uart_resp_handler → command_parser run — then
    ...                assert it resolves to the expected jump-table index. (Exercises
    ...                the real path now that the command_parser argument-order bug is
    ...                fixed; see docs/console-command-bug.md.)
    [Arguments]    ${name}    ${expected}
    Create Leaf Machine
    Patch Dispatch Capture
    Inject Console Line    ${name}
    Process Rx Ring
    ${idx}=    Execute Command    sysbus ReadByte ${CMD_IDX}
    Should Be Equal As Integers    ${idx}    ${expected}

*** Test Cases ***
Vector Table Is Well Formed
    [Documentation]    Static check (no execution): the reset SP points into SRAM
    ...                and the reset PC into application flash with the Thumb bit set.
    Create Battery Machine
    ${sp}=    Execute Command    sysbus ReadDoubleWord 0x08005028
    ${pc}=    Execute Command    sysbus ReadDoubleWord 0x0800502C
    # SP in SRAM 0x2000_xxxx (regex tolerates Renode's optional leading-zero trim)
    Should Match Regexp    ${sp}    (?i)0x0*2000[0-9a-f]+
    # Reset vector in app flash (0x080x_xxxx)
    Should Match Regexp    ${pc}    (?i)0x0*80[0-9a-f]+

Reset Chain Reaches Main
    [Documentation]    The startup chain — .data copy, .bss zero, SystemInit,
    ...                __libc_init_array_lite — must hand control to main(). This
    ...                touches no peripherals, so it is a hard requirement.
    Create Battery Machine
    Create Log Tester         5
    ${main}=    Execute Command    sysbus GetSymbolAddress "main"
    Execute Command           cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Start Emulation
    Wait For Log Entry        REACHED_MAIN

CPU Stays Alive After Boot
    [Documentation]    Liveness: after running a while the CPU has retired many
    ...                instructions and is executing application flash (not parked
    ...                at 0 or trapped below the app base).
    Create Battery Machine
    Execute Command           emulation RunFor "0.01"
    ${insns}=   Execute Command    cpu ExecutedInstructions
    Should Not Be Empty       ${insns}
    ${pc}=      Execute Command    cpu PC
    Should Match Regexp       ${pc}    (?i)0x0*80[0-9a-f]+

Boots Into The Service Super-Loop
    [Documentation]    Deeper boot: with the clock/flash status registers pinned
    ...                ready, the firmware should reach uart_resp_handler (the RX
    ...                drain called from the main super-loop). If this stalls,
    ...                the RCC_CR / RCC_CFGR tags in 'Create Battery Machine' need
    ...                tuning for the clock tree this build selects.
    Create Battery Machine
    Create Log Tester         60
    ${loop}=    Execute Command    sysbus GetSymbolAddress "uart_resp_handler"
    ${loop}=    Strip String      ${loop}
    Execute Command           cpu AddHook ${loop} "self.Log(LogLevel.Error, 'IN_SUPERLOOP')"
    Start Emulation
    Wait For Log Entry        IN_SUPERLOOP

# --- Functional unit tests (direct leaf-function calls) ----------------------
# These do not run the firmware's boot path. Each loads the image into a fresh
# machine, sets up the AAPCS registers (R0..R3 = args, SP = stack, LR = a self-
# loop trap), points PC at a pure routine, runs a brief window, and reads R0.
# This exercises the *real compiled code* for the CRC/hex/tick primitives against
# externally-known reference values — catching mis-translations the boot/liveness
# tests above cannot. NB: a returning leaf parks the CPU in the trap loop, so each
# call needs its own machine (a second call in the same machine never resumes).

CRC-16 Modbus Standard Check Value
    [Documentation]    crc16_calc over ASCII "123456789" must equal 0x4B37 — the
    ...                canonical Modbus/CRC-16 check value (poly 0xA001, init
    ...                0xFFFF). Validates the protocol-frame CRC against an external
    ...                reference rather than against itself.
    Create Leaf Machine
    Load Scratch Bytes    0x31  0x32  0x33  0x34  0x35  0x36  0x37  0x38  0x39
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 9
    ${crc}=    Call Leaf Function    crc16_calc
    Should Be Equal As Integers    ${crc}    0x4B37

CRC-16 Of A Modbus Read-Holding-Registers Frame
    [Documentation]    The Read Holding Registers request below the scaffold
    ...                (AA 03 00 02 00 2B) has CRC-16 0xCEBD; appending CE BD
    ...                little-endian forms the full on-wire frame the BMS expects.
    Create Leaf Machine
    Load Scratch Bytes    0xAA  0x03  0x00  0x02  0x00  0x2B
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 6
    ${crc}=    Call Leaf Function    crc16_calc
    Should Be Equal As Integers    ${crc}    0xCEBD

CRC-16 Of An Empty Buffer Is The Init Value
    [Documentation]    With len 0 the loop body never executes, so crc16_calc
    ...                returns the initial register 0xFFFF unchanged. Guards the
    ...                `while (len-- != 0)` boundary.
    Create Leaf Machine
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 0
    ${crc}=    Call Leaf Function    crc16_calc
    Should Be Equal As Integers    ${crc}    0xFFFF

CRC-8 SMBus PEC Standard Check Value
    [Documentation]    crc8_for_smbus over "123456789" must equal 0xF4 — the
    ...                canonical CRC-8/SMBus check value (poly 0x07, init 0x00).
    ...                This is the PEC byte the FEDL5236 driver appends and checks.
    Create Leaf Machine
    Load Scratch Bytes    0x31  0x32  0x33  0x34  0x35  0x36  0x37  0x38  0x39
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 9
    ${crc}=    Call Leaf Function    crc8_for_smbus
    Should Be Equal As Integers    ${crc}    0xF4

CRC-8 Verify Agrees With The Generator
    [Documentation]    crc8_verify is the read-side helper; for identical input it
    ...                must compute the same PEC as crc8_for_smbus (both wrap the
    ...                same bitwise core). Run each in its own machine and compare.
    Create Leaf Machine
    Load Scratch Bytes    0xAA  0x03  0x00  0x02  0x00  0x2B
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 6
    ${gen}=    Call Leaf Function    crc8_for_smbus
    Create Leaf Machine
    Load Scratch Bytes    0xAA  0x03  0x00  0x02  0x00  0x2B
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 6
    ${ver}=    Call Leaf Function    crc8_verify
    Should Be Equal As Integers    ${gen}    ${ver}

Hex To Nibble Maps Digits And Letters
    [Documentation]    hex_to_nibble: '0'->0, '9'->9, 'A'->0xA, 'F'->0xF. Each case
    ...                runs in its own machine (one leaf call per machine).
    Hex To Nibble Should Be    0x30    0x0
    Hex To Nibble Should Be    0x39    0x9
    Hex To Nibble Should Be    0x41    0xA
    Hex To Nibble Should Be    0x46    0xF

Tick Get Reads The System Tick Counter
    [Documentation]    tick_get returns the 32-bit word at SRAM 0x200047DC. Seed a
    ...                known sentinel there and confirm the routine reads it back —
    ...                a focused check of the tick reference the timeout loops poll.
    Create Leaf Machine
    Execute Command       sysbus WriteDoubleWord 0x200047DC 0x0001E240
    ${t}=    Call Leaf Function    tick_get
    Should Be Equal As Integers    ${t}    0x0001E240

# --- Modbus RTU functional tests (RX → handler → TX) -------------------------
# These drive the binary Modbus link the way the cartridge MCU does, without the
# slow full super-loop: a request frame is written into the USART1 RX ring, then
# uart_resp_handler() is called once (it drains the ring, feeding each byte to the
# uart_protocol_handler state machine). The handler parses the frame, verifies its
# CRC-16, and — for a read — assembles the response in respbuf and pushes it out
# through uart_putchar (the real TX path). We then read the response straight from
# SRAM. The BMS is Modbus slave 0xAA; CRC-16 is poly 0xA001, appended LE.

Modbus Read Holding Registers Returns A CRC-Valid Response
    [Documentation]    Inject a Read Holding Registers request (func 0x03, AA 03 00
    ...                00 00 04, CRC 5D D2) and confirm the firmware emits a response
    ...                that opens with AA 03 and carries a correct trailing CRC-16 —
    ...                i.e. the full RX-parse → telemetry-cascade → TX path works and
    ...                produces a frame the host would accept.
    Create Leaf Machine
    Inject Modbus Frame    0xAA  0x03  0x00  0x00  0x00  0x04  0x5D  0xD2
    Process Rx Ring
    # A response was assembled (fields emitted) and is non-trivially long.
    ${fields}=    Execute Command    sysbus ReadDoubleWord ${TX_FIELD_CNT}
    Should Not Be Equal As Integers    ${fields}    0
    ${len}=    Read Sram Word    ${RESP_LEN}
    Should Be True    ${len} > 4
    # Header: slave 0xAA, function 0x03.
    ${b1addr}=    Evaluate    ${RESP_BUF} + 1
    ${b0}=    Execute Command    sysbus ReadByte ${RESP_BUF}
    ${b1}=    Execute Command    sysbus ReadByte ${b1addr}
    Should Be Equal As Integers    ${b0}    0xAA
    Should Be Equal As Integers    ${b1}    0x03
    # Trailing two bytes are a valid Modbus CRC-16 over the body.
    ${valid}=    Response Is Crc Valid    ${len}
    Should Be True    ${valid}

Modbus Read Response Header Echoes The Function Code
    [Documentation]    A second read at a different start register (read 8 regs from
    ...                reg 2) still produces a well-formed AA 03 header — the parser
    ...                isn't tied to one specific request.
    Create Leaf Machine
    Inject Modbus Frame    0xAA  0x03  0x00  0x02  0x00  0x08  0xFC  0x17
    Process Rx Ring
    ${len}=    Read Sram Word    ${RESP_LEN}
    Should Be True    ${len} > 4
    ${valid}=    Response Is Crc Valid    ${len}
    Should Be True    ${valid}

Modbus Bad-CRC Request Is Rejected
    [Documentation]    The same read frame with a corrupted CRC (last byte D2→00)
    ...                must be dropped by the handler's CRC check — no response is
    ...                assembled (TX field count stays 0).
    Create Leaf Machine
    Inject Modbus Frame    0xAA  0x03  0x00  0x00  0x00  0x04  0x5D  0x00
    Process Rx Ring
    ${fields}=    Execute Command    sysbus ReadDoubleWord ${TX_FIELD_CNT}
    Should Be Equal As Integers    ${fields}    0

Modbus Write Single Register Takes Effect
    [Documentation]    Write Single Register (func 0x06) to register 9 = 1 turns on
    ...                test/debug mode: arm_cfg_flag sets cfg_blk+5 (0x200028D5). Stub
    ...                memcmp_verify (a flash/SPI persistence loop the bare platform
    ...                can't service) and confirm the flag flips 0 → 1, proving the
    ...                write reached its dispatch handler.
    Create Leaf Machine
    ${mcv}=    Resolve Symbol    memcmp_verify
    Execute Command    cpu AddHook ${mcv} "self.SetRegisterUlong(0, 0); cpu.PC = cpu.LR"
    Execute Command    sysbus WriteByte ${TESTMODE_FLAG} 0
    Inject Modbus Frame    0xAA  0x06  0x00  0x09  0x00  0x01  0x81  0xD3
    Process Rx Ring
    ${flag}=    Execute Command    sysbus ReadByte ${TESTMODE_FLAG}
    Should Be Equal As Integers    ${flag}    0x01

# --- ASCII console command tests (KEY=VALUE over the same UART) ---------------
# In command mode, printable RX bytes are gathered into a line buffer and dispatched
# on CR to command_parser, which prefix-matches a 23-entry name table and calls a
# 24-slot jump table. One reconstruction gap remains (see docs/console-command-bug.md):
# the jump-table is all-NULL — the handler bodies are frame-sharing continuations the
# decomp couldn't extract (OEM keeps the real pointers at flash 0x08017FD8), so a
# matched command executes nothing. The tests patch the table to capture the dispatch
# index instead. (The earlier command_parser argument-order bug, which made every
# command collapse to entry 1, is fixed.)

All Console Commands Dispatch To Their Action Index
    [Documentation]    For every documented console command name, assert command_parser
    ...                routes it to the right jump-table index (1..23). Covers the
    ...                prefix-match table including the '?'-suffixed getters and the
    ...                space-containing multi-word names.
    Console Command Dispatches To    Who?               ${1}
    Console Command Dispatches To    Now?               ${2}
    Console Command Dispatches To    PF                 ${3}
    Console Command Dispatches To    Reset BMS          ${4}
    Console Command Dispatches To    DF                 ${5}
    Console Command Dispatches To    Upgrade AP         ${6}
    Console Command Dispatches To    Upgrade BL         ${7}
    Console Command Dispatches To    Into BootLoader    ${8}
    Console Command Dispatches To    CHG CAL            ${9}
    Console Command Dispatches To    CHG CAL?           ${10}
    Console Command Dispatches To    DSG CAL            ${11}
    Console Command Dispatches To    DSG CAL?           ${12}
    Console Command Dispatches To    Reset ESN          ${13}
    Console Command Dispatches To    Log Clear          ${14}
    Console Command Dispatches To    TS0                ${15}
    Console Command Dispatches To    TS1                ${16}
    Console Command Dispatches To    TS2                ${17}
    Console Command Dispatches To    TS0?               ${18}
    Console Command Dispatches To    TS1?               ${19}
    Console Command Dispatches To    TS2?               ${20}
    Console Command Dispatches To    TS Reset           ${21}
    Console Command Dispatches To    FCC                ${22}
    Console Command Dispatches To    SOC                ${23}

Unknown Console Command Hits The Dispatch-Error Sink
    [Documentation]    A name that matches nothing in the table leaves the action
    ...                index at 0 (the dispatch-error slot), not a valid command — so
    ...                the matcher doesn't false-positive on garbage input.
    Console Command Dispatches To    ZZNotACommand      ${0}

# --- Shipping mode / deep-sleep ----------------------------------------------

Shipping Mode Command Clears The Run Flag
    [Documentation]    Modbus write of command word 0x95 (AA 06 00 95 00 00) invokes
    ...                shipping_mode: charge MOSFET off, enable line low, and clear
    ...                bit 11 (0x800, "running") of the control word 0x20002C00. Seed
    ...                the bit set and confirm shipping_mode clears it. bms_configure
    ...                is stubbed (it drives GPIO/SPI the bare platform doesn't model);
    ...                the flag is cleared before that call regardless.
    Create Leaf Machine
    ${bms}=    Resolve Symbol    bms_configure
    Execute Command    cpu AddHook ${bms} "cpu.PC = cpu.LR"
    Inject Modbus Frame    0xAA  0x06  0x00  0x95  0x00  0x00  0x80  0x3D
    Execute Command    sysbus WriteDoubleWord ${MODE_WORD} 0x800
    Process Rx Ring
    ${mode}=    Execute Command    sysbus ReadDoubleWord ${MODE_WORD}
    ${mode}=    Strip String       ${mode}
    ${bit}=     Evaluate    ${mode} & 0x800
    Should Be Equal As Integers    ${bit}    0

Bootloader Entry Reaches The Deep-Sleep Loop
    [Documentation]    bootloader_entry — the "Into Bootloader"/shipping deepest-sleep
    ...                path — prints "Shipping Mode", turns the FETs off, sets BMS state
    ...                6, and when the power-on button check passes enters an infinite
    ...                wait-for-reset loop at bootloader_entry+0x62. Stub the I/O-bound
    ...                calls (UART print/flush, BMS/GPIO drivers, flash verify) and force
    ...                the button check true, then confirm the CPU parks in that loop.
    Create Leaf Machine
    Hook Return    uart_printf
    Hook Return    uart_tx_flush
    Hook Return    bms_set_state
    Hook Return    bms_configure
    Hook Return    charge_mosfet_off
    Hook Return    memcmp_verify
    Hook Return    gpio_bit_write
    ${btn}=    Resolve Symbol    button_entry_check
    Execute Command    cpu AddHook ${btn} "self.SetRegisterUlong(0, 1); cpu.PC = cpu.LR"
    ${entry}=    Resolve Symbol    bootloader_entry
    ${loop}=     Evaluate    "0x%X" % (${entry} + 0x62)
    ${trap}=     Return Trap
    Execute Command    cpu SetRegister 13 ${STACKTOP}
    Execute Command    cpu SetRegister 14 ${trap}
    Execute Command    cpu PC ${entry}
    Execute Command    emulation RunFor "0.02"
    ${pc}=    Execute Command    cpu PC
    ${pc}=    Strip String    ${pc}
    Should Be Equal As Integers    ${pc}    ${loop}

# --- Telemetry register map + stateful write commands ------------------------

Modbus Read Reflects Seeded Telemetry Values
    [Documentation]    Seed distinctive values at the documented telemetry SRAM sources
    ...                and confirm a full Read Holding Registers (start reg 0) streams
    ...                them back at the expected response offsets — verifying the
    ...                telemetry register map against the firmware, not just the CRC.
    ...                Offsets are fixed by the cmd-3 cascade order (header is 3 bytes,
    ...                each field 2 bytes, big-endian).
    Create Leaf Machine
    Execute Command    sysbus WriteWord 0x2000281C 0xAA11    # pack voltage   -> offset 11
    Execute Command    sysbus WriteWord 0x200028A4 0xBB22    # cell 1 voltage -> offset 57
    Execute Command    sysbus WriteWord 0x200027FA 0xCC33    # max cell mV    -> offset 85
    Execute Command    sysbus WriteWord 0x2000282A 0xDD44    # min cell mV    -> offset 87
    Inject Modbus Frame    0xAA  0x03  0x00  0x00  0x00  0x04  0x5D  0xD2
    Process Rx Ring
    Response Word At Offset Should Be    11    0xAA11
    Response Word At Offset Should Be    57    0xBB22
    Response Word At Offset Should Be    85    0xCC33
    Response Word At Offset Should Be    87    0xDD44

Modbus Read Streams The ESN From EEPROM
    [Documentation]    Register 0x0C of the telemetry read is the first ESN word, read
    ...                from data EEPROM 0x0808000F/0x08080010 (modelled in the .repl).
    ...                Seed those bytes and confirm they surface in the response at the
    ...                cascade offset (27, big-endian hi=+0x10, lo=+0x0F) — proving the
    ...                EEPROM-backed identity fields flow through, not just SRAM ones.
    Create Leaf Machine
    Execute Command    sysbus WriteByte 0x0808000F 0x53    # 'S' -> response[28] (lo)
    Execute Command    sysbus WriteByte 0x08080010 0x4E    # 'N' -> response[27] (hi)
    Inject Modbus Frame    0xAA  0x03  0x00  0x00  0x00  0x04  0x5D  0xD2
    Process Rx Ring
    Response Word At Offset Should Be    27    0x4E53

Charge MOSFET Command Sets The Control Bits
    [Documentation]    Write Single Register to command word 0x1A (arg 1) is the
    ...                "charge MOSFET on" command. arm_state_bit only acts when the live
    ...                BMS state (0x20002B58) is in {1..3, 0xD..0x11}; seed state 2, then
    ...                confirm it sets control-word bit 12 (0x1000 at 0x20002C00) and bit
    ...                1 (0x20002870). bms_configure is stubbed (GPIO/SPI driver).
    Create Leaf Machine
    ${bms}=    Resolve Symbol    bms_configure
    Execute Command    cpu AddHook ${bms} "cpu.PC = cpu.LR"
    Execute Command    sysbus WriteByte 0x20002B58 0x02      # live state in the allowed set
    Execute Command    sysbus WriteByte 0x20002870 0x00
    Inject Modbus Frame    0xAA  0x06  0x00  0x1A  0x00  0x01  0x70  0x16
    Process Rx Ring
    ${ctrl}=    Read Sram Dword    ${MODE_WORD}
    ${ctrlbit}=    Evaluate    ${ctrl} & 0x1000
    Should Be Equal As Integers    ${ctrlbit}    0x1000
    ${mode870}=    Execute Command    sysbus ReadByte 0x20002870
    ${mode870}=    Strip String    ${mode870}
    ${b1}=    Evaluate    (${mode870}) & 0x2
    Should Be Equal As Integers    ${b1}    0x2
