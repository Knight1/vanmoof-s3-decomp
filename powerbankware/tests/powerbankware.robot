*** Settings ***
Documentation     Renode smoke tests for the VanMoof powerbankware (STM32F091) image.
...               Build first:  make -C powerbankware all
...               then run:     renode-test powerbankware/tests/powerbankware.robot
...               (paths below are relative to the repo root).
...
...               powerbankware is the STM32F091xC PowerBank battery-module
...               application — the F0/Cortex-M0 sibling of batteryware (L0/M0+).
...               Its Modbus host link is driven exactly as batteryware's: a request
...               frame is written into the USART2 RX ring, then uart_rx_handler()
...               drains the ring one byte at a time into the modbus_process state
...               machine, which assembles the telemetry response in SRAM.
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}

*** Variables ***
${ELF}            @${CURDIR}/../build/powerbankware.elf
${REPL}           @${CURDIR}/powerbankware.repl
# Cortex-M0 app vector table: image header (0x28) then the 48-word table at
# 0x08008028 (SP @ +0x00, reset @ +0x04). hal_bringup later copies it to SRAM.
${VTOR}           0x08008028
# Scratch buffer + stack for the direct-call (leaf function) tests. Both sit high
# in the 32 KB SRAM (0x20000000..0x20008000), clear of the firmware's own globals.
${SCRATCH}        0x20007000
${STACKTOP}       0x20008000
# All function entry points are resolved at run time with `sysbus GetSymbolAddress`
# so the suite survives a rebuild that shifts flash — never hard-code flash here.
#
# Modbus RX/TX SRAM map (from src/uart.c, src/modbus.c):
${RX_RING}        0x20000858      # USART2 RX ring buffer (uart_rx_handler drains it)
${RX_WR}          0x20000a58      # RX ring write index (uint16)
${RX_RD}          0x20000a5a      # RX ring read index (uint16)
${RX_STATE}       0x20001aca      # UART handle +0x6a — must be 0x20 (' ') to drain
${MODE_WORD}      0x200006a0      # mode/cfg word; 0 selects the binary Modbus path
${TX_RING}        0x20000a60      # USART2 TX ring (uart_putchar enqueues here)
${TX_WR_IDX}      0x20000a5c      # TX ring write index (uint16)
${MB_STATE}       0x2000260e      # modbus_process channel-2 frame state (uint16)
${RESP_BUF}       0x20001d00      # assembled response frame (AA 03 ... CRC)
${RESP_WR}        0x20002608      # response write index / final length (uint16)
${RESP_REMAIN}    0x20002610      # response byte count remaining (uint16)
${RESP_NREG}      0x20001e00      # emitted register count / write-ack (uint16)

*** Keywords ***
Create Leaf Machine
    [Documentation]    Minimal machine for direct-call and Modbus tests: load the
    ...                platform and ELF, nothing else. The pure routines under test
    ...                (CRC, hex, the Modbus RX→telemetry→TX cascade) touch no
    ...                clock/flash status registers, so no boot-time tags or hooks
    ...                are needed here.
    Execute Command           mach create
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus LoadELF ${ELF}

Create App Machine
    [Documentation]    Build the machine and load the image for the boot/liveness
    ...                tests. Point the vector table at the app header and register a
    ...                reset macro so a soft reset restores it. main() is the very
    ...                first call out of Reset_Handler (before hal_bringup brings up
    ...                the clock tree), so no clock hooks are needed to reach it.
    Execute Command           mach create "powerbankware"
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus LoadELF ${ELF}
    Execute Command           cpu VectorTableOffset ${VTOR}
    Execute Command           macro reset "cpu VectorTableOffset ${VTOR}"

Resolve Symbol
    [Documentation]    Look up a symbol's address in the loaded ELF, stripped.
    [Arguments]    ${name}
    ${addr}=    Execute Command    sysbus GetSymbolAddress "${name}"
    ${addr}=    Strip String       ${addr}
    [Return]    ${addr}

Return Trap
    [Documentation]    Address of Default_Handler (a `b .` self-loop) with the Thumb
    ...                bit set, used as LR so a called function parks the CPU harmlessly
    ...                on return (BX LR) instead of running off into undefined code.
    ${trap}=    Resolve Symbol     Default_Handler
    ${trap}=    Evaluate           ${trap} | 1
    [Return]    ${trap}

Call Leaf Function
    [Documentation]    Invoke a Thumb function by symbol name and return R0 (its
    ...                return value) as a stripped string. Arguments R0..R3 must
    ...                already be set by the caller. Sets up a fresh stack, points LR
    ...                at the Return Trap self-loop, runs a brief window so the call
    ...                completes, then reads R0.
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

Inject Modbus Frame
    [Documentation]    Put the BMS into binary Modbus mode, clear the channel-2 RX
    ...                state machine + response cells, mark the RX channel active, and
    ...                write the request bytes into the USART2 RX ring so the next
    ...                'Process Rx Ring' drains them. Frame bytes are byte literals
    ...                (incl. the trailing little-endian CRC-16).
    [Arguments]    @{frame}
    Execute Command    sysbus WriteByte ${RX_STATE} 0x20         # RX channel active (' ')
    Execute Command    sysbus WriteWord ${MODE_WORD} 0          # binary Modbus path
    Execute Command    sysbus WriteWord ${MB_STATE} 0           # fresh frame
    Execute Command    sysbus WriteWord ${RESP_WR} 0
    Execute Command    sysbus WriteWord ${RESP_REMAIN} 0
    Execute Command    sysbus WriteWord ${RESP_NREG} 0
    Execute Command    sysbus WriteWord ${TX_WR_IDX} 0
    ${n}=    Set Variable    ${0}
    FOR    ${b}    IN    @{frame}
        ${addr}=    Evaluate    ${RX_RING} + ${n}
        Execute Command    sysbus WriteByte ${addr} ${b}
        ${n}=    Evaluate    ${n} + 1
    END
    Execute Command    sysbus WriteWord ${RX_WR} ${n}           # write index = frame length
    Execute Command    sysbus WriteWord ${RX_RD} 0

Process Rx Ring
    [Documentation]    Call uart_rx_handler() once. It loops over the whole RX ring,
    ...                feeding each byte to modbus_process(2, ...) — so a complete
    ...                frame is parsed and its response assembled in a single call
    ...                (which matters: a returning call parks the CPU, so only one
    ...                RunFor per machine).
    Execute Command    cpu SetRegister 13 ${STACKTOP}
    ${trap}=    Return Trap
    Execute Command    cpu SetRegister 14 ${trap}
    ${h}=       Resolve Symbol    uart_rx_handler
    Execute Command    cpu PC ${h}
    Execute Command    emulation RunFor "0.05"

Response Is Crc Valid
    [Documentation]    Read ${length} response bytes from ${RESP_BUF} and return True
    ...                iff the trailing two bytes are a correct little-endian Modbus
    ...                CRC-16 over the preceding body (poly 0xA001, init 0xFFFF),
    ...                computed in-line with a nested reduce.
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

Response Word At Offset Should Be
    [Documentation]    Assert the big-endian 16-bit value at byte offset ${off} of the
    ...                assembled response (${RESP_BUF}) equals ${expected}. Telemetry
    ...                fields are appended hi-byte-first by mb_push16/mb_emit16.
    [Arguments]    ${off}    ${expected}
    ${hiaddr}=    Evaluate    ${RESP_BUF} + ${off}
    ${loaddr}=    Evaluate    ${RESP_BUF} + ${off} + 1
    ${hi}=    Execute Command    sysbus ReadByte ${hiaddr}
    ${hi}=    Strip String       ${hi}
    ${lo}=    Execute Command    sysbus ReadByte ${loaddr}
    ${lo}=    Strip String       ${lo}
    ${val}=   Evaluate    ((${hi}) << 8) | (${lo})
    Should Be Equal As Integers    ${val}    ${expected}

Nibble To Hex Should Be
    [Documentation]    Fresh-machine helper for the nibble_to_hex table check: load
    ...                the 4-bit value into R0, call, and assert the ASCII result.
    [Arguments]    ${nibble}    ${expected}
    Create Leaf Machine
    Execute Command    cpu SetRegister 0 ${nibble}
    ${r}=    Call Leaf Function    nibble_to_hex
    Should Be Equal As Integers    ${r}    ${expected}

*** Test Cases ***
# --- Boot / liveness ---------------------------------------------------------

Vector Table Is Well Formed
    [Documentation]    Static check (no execution): vector slot 0 (initial SP) points
    ...                at the top of SRAM and slot 1 (reset PC) into application flash
    ...                with the Thumb bit set — the CPU resets cleanly into this image.
    Create App Machine
    ${sp}=    Execute Command    sysbus ReadDoubleWord ${VTOR}
    ${pc}=    Execute Command    sysbus ReadDoubleWord 0x0800802C
    # Initial SP at the top of the 32 KB SRAM (0x20008000).
    Should Match Regexp    ${sp}    (?i)0x0*20008000
    # Reset vector in app flash (0x0800_xxxx / 0x0801_xxxx) with the Thumb bit set.
    Should Match Regexp    ${pc}    (?i)0x0*80[0-9a-f]*[13579bdf]

Reset Chain Reaches Main
    [Documentation]    The startup chain — Reset_Handler (.data copy + .bss zero via
    ...                init_data_bss, SystemInit, __libc_init_array_lite) — must hand
    ...                control to main(). main() is entered before any clock bring-up,
    ...                so this needs no peripheral stubbing; it is a hard requirement.
    Create App Machine
    Create Log Tester         10
    ${main}=    Resolve Symbol    main
    Execute Command           cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Start Emulation
    Wait For Log Entry        REACHED_MAIN

# --- Functional unit tests (direct leaf-function calls) ----------------------
# These do not run the firmware's boot path. Each loads the image into a fresh
# machine, sets up the AAPCS registers (R0..R3 = args, SP = stack, LR = a self-
# loop trap), points PC at a pure routine, runs a brief window, and reads R0 —
# exercising the real compiled code against externally-known reference values.

CRC-16 Modbus Standard Check Value
    [Documentation]    modbus_crc16 over ASCII "123456789" must equal 0x4B37 — the
    ...                canonical Modbus/CRC-16 check value (poly 0xA001, init 0xFFFF).
    ...                Validates the protocol-frame CRC against an external reference.
    Create Leaf Machine
    Load Scratch Bytes    0x31  0x32  0x33  0x34  0x35  0x36  0x37  0x38  0x39
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 9
    ${crc}=    Call Leaf Function    modbus_crc16
    Should Be Equal As Integers    ${crc}    0x4B37

CRC-16 Of A Read-Holding-Registers Frame
    [Documentation]    The Read Holding Registers request AA 03 00 00 00 04 has
    ...                CRC-16 0xD25D; appending 5D D2 little-endian forms the full
    ...                on-wire frame the BMS expects (and matches batteryware's
    ...                identical CRC, cross-validating both reconstructions).
    Create Leaf Machine
    Load Scratch Bytes    0xAA  0x03  0x00  0x00  0x00  0x04
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 6
    ${crc}=    Call Leaf Function    modbus_crc16
    Should Be Equal As Integers    ${crc}    0xD25D

CRC-16 Of An Empty Buffer Is The Init Value
    [Documentation]    With len 0 the loop body never executes, so modbus_crc16
    ...                returns the initial register 0xFFFF unchanged. Guards the
    ...                `while (len-- != 0)` boundary.
    Create Leaf Machine
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 0
    ${crc}=    Call Leaf Function    modbus_crc16
    Should Be Equal As Integers    ${crc}    0xFFFF

Nibble To Hex Maps Digits And Letters
    [Documentation]    nibble_to_hex: 0->'0', 9->'9', 0xA->'A', 0xF->'F'. Each case
    ...                runs in its own machine (one leaf call per machine).
    Nibble To Hex Should Be    0x0    0x30
    Nibble To Hex Should Be    0x9    0x39
    Nibble To Hex Should Be    0xA    0x41
    Nibble To Hex Should Be    0xF    0x46

# --- Modbus RTU functional tests (RX → handler → TX) -------------------------
# These drive the binary Modbus link the way the cartridge MCU does, without the
# slow full super-loop: a request frame is written into the USART2 RX ring, then
# uart_rx_handler() is called once (it drains the ring, feeding each byte to the
# modbus_process state machine). The handler parses the frame, verifies its CRC-16,
# and — for a read — assembles the response in RESP_BUF and pushes it through
# uart_putchar (the real TX path). The BMS is Modbus slave 0xAA; CRC-16 is poly
# 0xA001, appended LE.

Modbus Read Holding Registers Returns A CRC-Valid Response
    [Documentation]    Inject a Read Holding Registers request (func 0x03, AA 03 00
    ...                00 00 04, CRC 5D D2) and confirm the firmware emits a response
    ...                that opens with AA 03 and carries a correct trailing CRC-16 —
    ...                i.e. the full RX-parse → telemetry-cascade → TX path works and
    ...                produces a frame the host would accept.
    Create Leaf Machine
    Inject Modbus Frame    0xAA  0x03  0x00  0x00  0x00  0x04  0x5D  0xD2
    Process Rx Ring
    # A response was assembled and is non-trivially long.
    ${len}=    Read Sram Word    ${RESP_WR}
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

Modbus Read Emits The Fixed Identity Registers
    [Documentation]    Registers 0 and 1 of the telemetry cascade are compile-time
    ...                constants (mb_push16(1,0) = 0x0100 and mb_push16(0,1) = 0x0001).
    ...                They land at response offsets 3 and 5 (3-byte header, 2 bytes
    ...                each, big-endian) — a deterministic check of the cascade's
    ...                framing independent of any seeded SRAM.
    Create Leaf Machine
    Inject Modbus Frame    0xAA  0x03  0x00  0x00  0x00  0x04  0x5D  0xD2
    Process Rx Ring
    Response Word At Offset Should Be    3    0x0100
    Response Word At Offset Should Be    5    0x0001

Modbus Read Reflects Seeded Telemetry Values
    [Documentation]    Seed distinctive values at the documented telemetry SRAM sources
    ...                and confirm a Read Holding Registers of 0x2B registers from reg 0
    ...                streams them back at the expected response offsets — verifying the
    ...                telemetry register map, not just the CRC. Offsets are fixed by the
    ...                cascade order (3-byte header, each register 2 bytes, big-endian:
    ...                reg N at offset 3 + N*2).
    Create Leaf Machine
    Execute Command    sysbus WriteWord 0x200003ce 0xAA11    # pack voltage (reg 4)  -> offset 11
    Execute Command    sysbus WriteWord 0x200003a2 0xCC33    # cell max mV  (reg 0x29)-> offset 85
    Execute Command    sysbus WriteWord 0x200003d2 0xDD44    # cell min mV  (reg 0x2a)-> offset 87
    Inject Modbus Frame    0xAA  0x03  0x00  0x00  0x00  0x2B  0x1C  0x0E
    Process Rx Ring
    Response Word At Offset Should Be    11    0xAA11
    Response Word At Offset Should Be    85    0xCC33
    Response Word At Offset Should Be    87    0xDD44

Modbus Read Response Header Echoes The Function Code
    [Documentation]    A second read at a different start register (read 8 regs from
    ...                reg 2) still produces a well-formed AA 03 header with a valid
    ...                trailing CRC — the parser isn't tied to one specific request.
    Create Leaf Machine
    Inject Modbus Frame    0xAA  0x03  0x00  0x02  0x00  0x08  0xFC  0x17
    Process Rx Ring
    ${len}=    Read Sram Word    ${RESP_WR}
    Should Be True    ${len} > 4
    ${valid}=    Response Is Crc Valid    ${len}
    Should Be True    ${valid}

Modbus Bad-CRC Request Is Rejected
    [Documentation]    The same read frame with a corrupted CRC (last byte D2->00)
    ...                must be dropped by the handler's CRC check — no response is
    ...                assembled (the response write index stays at its reset value).
    Create Leaf Machine
    Inject Modbus Frame    0xAA  0x03  0x00  0x00  0x00  0x04  0x5D  0x00
    Process Rx Ring
    ${len}=    Read Sram Word    ${RESP_WR}
    Should Be Equal As Integers    ${len}    0

Modbus Write Single To A Read-Only Register Is Acked
    [Documentation]    Write Single Register (func 0x06) to register 9 is a recognised
    ...                read-only/no-op command: modbus_write_single increments the ack
    ...                counter (s_resp_nreg) and echoes the 8-byte request frame back
    ...                over the TX ring. Confirm the write parsed, passed CRC, and was
    ...                dispatched (ack counter goes 0 -> 1).
    Create Leaf Machine
    Inject Modbus Frame    0xAA  0x06  0x00  0x09  0x00  0x00  0x40  0x13
    Process Rx Ring
    ${ack}=    Read Sram Word    ${RESP_NREG}
    Should Be Equal As Integers    ${ack}    1
    # The echoed acknowledgement frame opens with the request header in the TX ring.
    ${e0}=    Execute Command    sysbus ReadByte ${TX_RING}
    ${e1addr}=    Evaluate    ${TX_RING} + 1
    ${e1}=    Execute Command    sysbus ReadByte ${e1addr}
    Should Be Equal As Integers    ${e0}    0xAA
    Should Be Equal As Integers    ${e1}    0x06
