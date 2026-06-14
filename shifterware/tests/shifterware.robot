*** Settings ***
Documentation     Renode smoke tests for the VanMoof shifterware (MM32F031F6U6) image.
...               Build first:  make -C shifterware all
...               then run:     renode-test shifterware/tests/shifterware.robot
...               (paths below are relative to the repo root).
...
...               shifterware is the eShifter application on the rear-hub PCB —
...               a Cortex-M0 firmware that drives the gear-change H-bridge and
...               answers the bike's main module over a USART1 Modbus RTU link
...               (this module is slave 0x20). Its RX path is driven the way the
...               hardware drives it: a request frame is written into the linear
...               IRQ scratch at 0x200001B2, the receive count at 0x200000E4 is
...               set, then modbus_rx_poll() is called once — it CRC-checks the
...               frame and hands it to modbus_dispatch_pdu, which mutates the
...               shifter state and/or assembles a reply in the SRAM TX buffer.
...               The blocking TX leaf (uart1_send_byte) is hooked to return so
...               reply-emitting commands don't spin on the unmodelled USART.
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}

*** Variables ***
${ELF}            @${CURDIR}/../build/shifterware.elf
${REPL}           @${CURDIR}/shifterware.repl
# Cortex-M0 app vector table: image header (0x28) then the 48-word table at
# 0x08003028 (SP @ +0x00, reset @ +0x04). shifterboot relocates it at boot.
${VTOR}           0x08003028
# Leaf-call scratch buffer + stack. Both sit clear of the firmware's globals
# (which top out near 0x20000200) and below the stack reservation (0x20000C00).
${SCRATCH}        0x20000800
${STACKTOP}       0x20001000
# All function entry points are resolved at run time with `sysbus GetSymbolAddress`
# so the suite survives a rebuild that shifts flash — never hard-code flash here.
#
# --- SRAM globals touched by the Modbus RX FSM / dispatcher (src/*.c) -------
${CRC_LO}         0x200000E7      # modbus_crc16_compute output, low byte
${CRC_HI}         0x200000E8      # modbus_crc16_compute output, high byte
${RX_SCRATCH}     0x200001B2      # linear IRQ-filled inbound scratch
${RX_HEAD}        0x200000E4      # bytes received this frame (0..0x2D)
${RX_FRAME_MODE}  0x200000D9      # 0 = short PDU (8 B), 1 = long PDU (45 B, OTA)
${RX_WAIT_CTR}    0x200000DC      # end-of-frame idle counter
${REQ_PENDING}    0x200000D8      # 1 = PDU validated & dispatching, 0 = idle
${RX_BUF}         0x200000C8      # validated short-frame buffer (8 B)
${TX_BUF}         0x200001A9      # shared TX scratch (response assembled here)
${COUNTER}        0x200000F8      # cmd-0x14 / cmd-0x02 monotonic counter
${MOTOR_RUNNING}  0x20000139      # 1 = H-bridge driving (gates cmds 0x02/0x14/0x5A)
${SHIFT_TARGET}   0x200000EA      # G_5A_TARGET — shift target latched by cmd 0x5A
${GEAR_TARGET}    0x20000116      # G_FLAG_116 — desired gear (1..4)
${FLAG_117}       0x20000117      # cmd-0x02/0x14 follow-up latch
${FLAG_13D}       0x2000013D      # cmd-0x14 "B" flag / sched gate
${FLAG_13E}       0x2000013E      # "scheduler has driving work" latch
${DRIVE_DIR}      0x20000114      # 0x0F up / 0xF0 down / 0 idle
${STATE_115}      0x20000115      # current gear-position counter
${GPIOA_IDR}      0x48000008      # STM32F1-style GPIOA input data register

*** Keywords ***
Create Leaf Machine
    [Documentation]    Minimal machine for direct-call and Modbus tests: load the
    ...                platform and ELF, then neutralise the blocking TX leaf
    ...                (uart1_send_byte spins on the unmodelled USART1 TX-ready
    ...                bit) by hooking it to return immediately. The response
    ...                bytes are assembled in the SRAM TX buffer *before* the
    ...                send loop runs, so tests can still assert on them.
    Execute Command           mach create
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus LoadELF ${ELF}
    ${tx}=    Resolve Symbol    uart1_send_byte
    Execute Command           cpu AddHook ${tx} "cpu.PC = cpu.LR"

Create App Machine
    [Documentation]    Build the machine and load the image for the boot/liveness
    ...                tests. Point the vector table at the app header and register
    ...                a reset macro so a soft reset restores it. main() is reached
    ...                straight out of Reset_Handler (.data copy + .bss/SRAM zero),
    ...                before any peripheral bring-up, so no clock hooks are needed.
    Execute Command           mach create "shifterware"
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

Call Function
    [Documentation]    Invoke a Thumb function by symbol name for its side effects.
    ...                Arguments R0..R3 must already be set by the caller. Sets up a
    ...                fresh stack, points LR at the Return Trap self-loop, then runs
    ...                a brief window so the call (and any nested dispatch) completes.
    [Arguments]    ${symbol}
    ${addr}=    Resolve Symbol    ${symbol}
    ${trap}=    Return Trap
    Execute Command           cpu SetRegister 13 ${STACKTOP}
    Execute Command           cpu SetRegister 14 ${trap}
    Execute Command           cpu PC ${addr}
    Execute Command           emulation RunFor "0.01"

Call Leaf Function
    [Documentation]    Like Call Function but returns R0 (the AAPCS return value)
    ...                as a stripped string, for pure routines under test.
    [Arguments]    ${symbol}
    Call Function    ${symbol}
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

Modbus Crc16
    [Documentation]    Compute the Modbus CRC-16 (poly 0xA001, init 0xFFFF) over six
    ...                header bytes and return it as an integer. The same nested
    ...                reduce the firmware's modbus_crc16_compute implements.
    [Arguments]    ${b0}    ${b1}    ${b2}    ${b3}    ${b4}    ${b5}
    ${crc}=    Evaluate    functools.reduce(lambda c,b: functools.reduce(lambda x,_:(x>>1)^0xA001 if x&1 else x>>1, range(8), c^b), [${b0}, ${b1}, ${b2}, ${b3}, ${b4}, ${b5}], 0xFFFF)    modules=functools
    [Return]    ${crc}

Inject Short Frame
    [Documentation]    Stage an 8-byte short Modbus frame for modbus_rx_poll: write
    ...                the six header bytes plus the appended little-endian CRC-16
    ...                into the IRQ scratch, mark the frame complete (RX_HEAD = 8) and
    ...                select short-frame mode. The caller passes the six header bytes;
    ...                byte 0 must be 0x20 (this module's slave address) and byte 3 is
    ...                the command code the dispatcher switches on.
    [Arguments]    ${b0}    ${b1}    ${b2}    ${b3}    ${b4}    ${b5}
    ${crc}=    Modbus Crc16    ${b0}    ${b1}    ${b2}    ${b3}    ${b4}    ${b5}
    ${lo}=    Evaluate    ${crc} & 0xFF
    ${hi}=    Evaluate    (${crc} >> 8) & 0xFF
    @{frame}=    Create List    ${b0}    ${b1}    ${b2}    ${b3}    ${b4}    ${b5}    ${lo}    ${hi}
    ${i}=    Set Variable    ${0}
    FOR    ${b}    IN    @{frame}
        ${addr}=    Evaluate    ${RX_SCRATCH} + ${i}
        Execute Command    sysbus WriteByte ${addr} ${b}
        ${i}=    Evaluate    ${i} + 1
    END
    Execute Command    sysbus WriteDoubleWord ${RX_HEAD} 8
    Execute Command    sysbus WriteByte ${RX_FRAME_MODE} 0
    Execute Command    sysbus WriteByte ${REQ_PENDING} 0

Process Rx Frame
    [Documentation]    Call modbus_rx_poll() once. It copies the staged frame into the
    ...                validated buffer, verifies its CRC-16, and on success latches
    ...                G_REQ_PENDING and dispatches the PDU — all in a single call.
    Call Function    modbus_rx_poll

Read Byte
    [Documentation]    Read a single SRAM byte and return it as an integer.
    [Arguments]    ${addr}
    ${v}=    Execute Command    sysbus ReadByte ${addr}
    ${v}=    Strip String       ${v}
    ${v}=    Convert To Integer    ${v}    16
    [Return]    ${v}

Read Dword
    [Documentation]    Read a 32-bit SRAM word and return it as an integer.
    [Arguments]    ${addr}
    ${v}=    Execute Command    sysbus ReadDoubleWord ${addr}
    ${v}=    Strip String       ${v}
    ${v}=    Convert To Integer    ${v}    16
    [Return]    ${v}

Read Crc16 Result
    [Documentation]    Read the two-byte modbus_crc16_compute output (lo @ ${CRC_LO},
    ...                hi @ ${CRC_HI}) and return it as a 16-bit integer.
    ${lo}=    Read Byte    ${CRC_LO}
    ${hi}=    Read Byte    ${CRC_HI}
    ${crc}=    Evaluate    (${hi} << 8) | ${lo}
    [Return]    ${crc}

Pin Read Should Be
    [Documentation]    Fresh-machine helper for the GPIO-input leaf checks: seed
    ...                GPIOA->IDR, call the input_paN reader, and assert its boolean
    ...                return. A new machine per read because only one RunFor executes
    ...                per machine (the CPU won't re-run after its first window).
    [Arguments]    ${func}    ${idr}    ${nonzero}
    Create Leaf Machine
    Execute Command    sysbus WriteDoubleWord ${GPIOA_IDR} ${idr}
    ${r}=    Call Leaf Function    ${func}
    IF    ${nonzero}
        Should Be True    ${r} != 0
    ELSE
        Should Be Equal As Integers    ${r}    0
    END

*** Test Cases ***
# --- Boot / liveness ---------------------------------------------------------

Vector Table Is Well Formed
    [Documentation]    Static check (no execution): vector slot 0 (initial SP) points
    ...                at the top of the 4 KB SRAM (0x20001000) and slot 1 (reset PC)
    ...                into application flash with the Thumb bit set — the CPU resets
    ...                cleanly into this image.
    Create App Machine
    ${sp}=    Execute Command    sysbus ReadDoubleWord ${VTOR}
    ${pc}=    Execute Command    sysbus ReadDoubleWord 0x0800302C
    Should Match Regexp    ${sp}    (?i)0x0*20001000
    # Reset vector in app flash (0x0800_3xxx ..) with the Thumb bit set.
    Should Match Regexp    ${pc}    (?i)0x0*80[0-9a-f]*[13579bdf]

Reset Chain Reaches Main
    [Documentation]    The startup chain — Reset_Handler (MSP reload, .data copy,
    ...                .bss + SRAM-globals zero) — must hand control to main(). main()
    ...                is entered before any clock/peripheral bring-up, so this needs
    ...                no stubbing; it is a hard requirement for a bootable image.
    Create App Machine
    Create Log Tester         10
    ${main}=    Resolve Symbol    main
    Execute Command           cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Start Emulation
    Wait For Log Entry        REACHED_MAIN

# --- Pure leaf-function unit tests -------------------------------------------
# Each loads the image into a fresh machine, sets up AAPCS registers (R0..R3 =
# args, SP = stack, LR = a self-loop trap), points PC at a real compiled routine,
# runs a brief window, and reads back the result — exercising the firmware's own
# code against externally-known reference values.

CRC-16 Modbus Standard Check Value
    [Documentation]    modbus_crc16_compute over ASCII "123456789" must produce the
    ...                canonical Modbus/CRC-16 check value 0x4B37 (poly 0xA001, init
    ...                0xFFFF). The result is written to the CRC byte pair in SRAM.
    Create Leaf Machine
    Load Scratch Bytes    0x31  0x32  0x33  0x34  0x35  0x36  0x37  0x38  0x39
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 9
    Call Function         modbus_crc16_compute
    ${crc}=    Read Crc16 Result
    Should Be Equal As Integers    ${crc}    0x4B37

CRC-16 Of An Empty Buffer Is The Init Value
    [Documentation]    With len 0 the loop body never executes, so the running CRC
    ...                register stays at its initial 0xFFFF — guards the `while (len
    ...                != 0)` boundary.
    Create Leaf Machine
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 0
    Call Function         modbus_crc16_compute
    ${crc}=    Read Crc16 Result
    Should Be Equal As Integers    ${crc}    0xFFFF

CRC-16 Of A Short-Frame Header
    [Documentation]    The cmd-0x14 request header (20 14 00 14 00 00) has CRC-16
    ...                0x7C77; appending 77 7C little-endian forms the on-wire frame
    ...                the shifter expects — cross-validating the suite's own frame
    ...                builder against the firmware's CRC routine.
    Create Leaf Machine
    Load Scratch Bytes    0x20  0x14  0x00  0x14  0x00  0x00
    Execute Command       cpu SetRegister 0 ${SCRATCH}
    Execute Command       cpu SetRegister 1 6
    Call Function         modbus_crc16_compute
    ${crc}=    Read Crc16 Result
    Should Be Equal As Integers    ${crc}    0x7C77

Memcpy Copies A Word-Aligned Block
    [Documentation]    memcpy with word-aligned src/dst takes the 32-bit fast path.
    ...                Copy 8 bytes from the scratch buffer to scratch+0x40 and confirm
    ...                every byte arrives intact.
    Create Leaf Machine
    Load Scratch Bytes    0x11  0x22  0x33  0x44  0x55  0x66  0x77  0x88
    ${dst}=    Evaluate    ${SCRATCH} + 0x40
    Execute Command    cpu SetRegister 0 ${dst}
    Execute Command    cpu SetRegister 1 ${SCRATCH}
    Execute Command    cpu SetRegister 2 8
    Call Function      memcpy
    ${b0}=    Read Byte    ${dst}
    ${b7}=    Read Byte    ${{ ${dst} + 7 }}
    Should Be Equal As Integers    ${b0}    0x11
    Should Be Equal As Integers    ${b7}    0x88

Input PA0 Reflects The GPIOA Input Register
    [Documentation]    input_pa0 reads GPIOA->IDR bit 0 (STM32F1-style IDR @ offset
    ...                0x08) — the position-sensor line the shift FSM polls. It reads
    ...                high only when bit 0 is set; bit 1 alone must not leak in (the
    ...                mask is 0x1). Each case runs in its own machine.
    Pin Read Should Be    input_pa0    0x1    ${TRUE}
    Pin Read Should Be    input_pa0    0x0    ${FALSE}
    Pin Read Should Be    input_pa0    0x2    ${FALSE}

Input PA1 Reflects The GPIOA Input Register
    [Documentation]    input_pa1 reads GPIOA->IDR bit 1 — the second position-sensor
    ...                line. It reads high only when bit 1 is set; bit 0 alone must not
    ...                read as PA1 high (the mask is 0x2), confirming the two inputs are
    ...                decoded independently.
    Pin Read Should Be    input_pa1    0x2    ${TRUE}
    Pin Read Should Be    input_pa1    0x0    ${FALSE}
    Pin Read Should Be    input_pa1    0x1    ${FALSE}

Image Verify Rejects A Bad Magic
    [Documentation]    image_verify_crc reads the manifest at flash 0x08001800. With
    ...                the slot blank (magic 0x00000000 != 0xAA55AA55) it must return 2
    ...                without ever touching the hardware CRC — the early magic guard.
    Create Leaf Machine
    ${rc}=    Call Leaf Function    image_verify_crc
    Should Be Equal As Integers    ${rc}    2

Image Verify Rejects An Oversized Length
    [Documentation]    With a valid magic but a length field >= the 12 KB cap
    ...                (IMAGE_MAX_SIZE 0x3000) image_verify_crc still returns 2 — the
    ...                length sanity check fires before the CRC compute.
    Create Leaf Machine
    Execute Command    sysbus WriteDoubleWord 0x08001800 0xAA55AA55
    Execute Command    sysbus WriteDoubleWord 0x0800180C 0x4000
    ${rc}=    Call Leaf Function    image_verify_crc
    Should Be Equal As Integers    ${rc}    2

# --- Modbus RX FSM + dispatcher ----------------------------------------------
# These drive the real receive path: stage a frame in the IRQ scratch, set the
# byte count, and call modbus_rx_poll once. It validates the CRC and dispatches
# the PDU. The shifter is Modbus slave 0x20; CRC-16 is poly 0xA001, appended LE.

Frame Addressed To Another Slave Is Ignored
    [Documentation]    modbus_rx_poll filters by slave address: a frame whose first
    ...                byte isn't 0x20 is dropped (RX_HEAD cleared, nothing dispatched).
    ...                Use a cmd 0x14 frame re-addressed to 0x21 and confirm the
    ...                cmd-0x14 counter never moves.
    Create Leaf Machine
    Execute Command    sysbus WriteDoubleWord ${COUNTER} 5
    Inject Short Frame    0x21  0x14  0x00  0x14  0x00  0x00
    Process Rx Frame
    ${ctr}=    Read Dword    ${COUNTER}
    Should Be Equal As Integers    ${ctr}    5
    ${head}=    Read Dword    ${RX_HEAD}
    Should Be Equal As Integers    ${head}    0

Incomplete Frame Waits For More Bytes
    [Documentation]    With fewer than 8 bytes received the short-frame FSM neither
    ...                dispatches nor drops the frame — it advances the end-of-frame
    ...                idle counter and returns. Seed RX_HEAD = 4 and confirm the wait
    ...                counter ticks to 1 while RX_HEAD is preserved.
    Create Leaf Machine
    Execute Command    sysbus WriteByte ${RX_SCRATCH} 0x20
    Execute Command    sysbus WriteByte ${RX_FRAME_MODE} 0
    Execute Command    sysbus WriteDoubleWord ${RX_WAIT_CTR} 0
    Execute Command    sysbus WriteDoubleWord ${RX_HEAD} 4
    Process Rx Frame
    ${wait}=    Read Dword    ${RX_WAIT_CTR}
    Should Be Equal As Integers    ${wait}    1
    ${head}=    Read Dword    ${RX_HEAD}
    Should Be Equal As Integers    ${head}    4

Bad-CRC Frame Is Rejected
    [Documentation]    A cmd-0x14 frame with a corrupted CRC byte must fail the
    ...                handler's CRC check — G_REQ_PENDING is never latched and the
    ...                dispatcher never runs (the cmd-0x14 counter stays put).
    Create Leaf Machine
    Execute Command    sysbus WriteDoubleWord ${COUNTER} 7
    # Stage a valid cmd-0x14 frame, then clobber the CRC-high byte.
    Inject Short Frame    0x20  0x14  0x00  0x14  0x00  0x00
    Execute Command    sysbus WriteByte ${{ ${RX_SCRATCH} + 7 }} 0x00
    Process Rx Frame
    ${ctr}=    Read Dword    ${COUNTER}
    Should Be Equal As Integers    ${ctr}    7
    ${pend}=    Read Byte    ${REQ_PENDING}
    Should Be Equal As Integers    ${pend}    0

Command 0x14 Bumps The Counter And Arms The Follow-Up
    [Documentation]    cmd 0x14 with the motor idle bumps the monotonic counter and
    ...                raises the two follow-up latches (G_FLAG_117 @ 0x117 and
    ...                G_FLAG_13D @ 0x13D). Seed counter 0 and confirm it reaches 1 and
    ...                both flags are set.
    Create Leaf Machine
    Execute Command    sysbus WriteByte ${MOTOR_RUNNING} 0
    Execute Command    sysbus WriteDoubleWord ${COUNTER} 0
    Inject Short Frame    0x20  0x14  0x00  0x14  0x00  0x00
    Process Rx Frame
    ${ctr}=    Read Dword    ${COUNTER}
    Should Be Equal As Integers    ${ctr}    1
    ${f117}=    Read Byte    ${FLAG_117}
    Should Be Equal As Integers    ${f117}    1
    ${f13d}=    Read Byte    ${FLAG_13D}
    Should Be Equal As Integers    ${f13d}    1

Command 0x14 While Motor Running Does Not Bump The Counter
    [Documentation]    The same cmd 0x14 while the H-bridge is driving takes the busy
    ...                arm: it clears the "B" flag (G_FLAG_13D) and leaves the counter
    ...                untouched — the bike can't preempt an in-flight shift.
    Create Leaf Machine
    Execute Command    sysbus WriteByte ${MOTOR_RUNNING} 1
    Execute Command    sysbus WriteDoubleWord ${COUNTER} 9
    Execute Command    sysbus WriteByte ${FLAG_13D} 1
    Inject Short Frame    0x20  0x14  0x00  0x14  0x00  0x00
    Process Rx Frame
    ${ctr}=    Read Dword    ${COUNTER}
    Should Be Equal As Integers    ${ctr}    9
    ${f13d}=    Read Byte    ${FLAG_13D}
    Should Be Equal As Integers    ${f13d}    0

Command 0x5A Latches The Shift Target
    [Documentation]    cmd 0x5A with the motor idle latches the shift target from
    ...                RX byte 5 into G_5A_TARGET (0x200000EA). Send value 0x42 and
    ...                confirm it lands.
    Create Leaf Machine
    Execute Command    sysbus WriteByte ${MOTOR_RUNNING} 0
    Inject Short Frame    0x20  0x5A  0x00  0x5A  0x00  0x42
    Process Rx Frame
    ${tgt}=    Read Byte    ${SHIFT_TARGET}
    Should Be Equal As Integers    ${tgt}    0x42

Command 0x5A While Motor Running Is Ignored
    [Documentation]    With the motor running cmd 0x5A must not overwrite the shift
    ...                target — the busy guard keeps the in-flight target. Seed a
    ...                sentinel target, send a different value, confirm it's unchanged.
    Create Leaf Machine
    Execute Command    sysbus WriteByte ${MOTOR_RUNNING} 1
    Execute Command    sysbus WriteByte ${SHIFT_TARGET} 0x07
    Inject Short Frame    0x20  0x5A  0x00  0x5A  0x00  0x42
    Process Rx Frame
    ${tgt}=    Read Byte    ${SHIFT_TARGET}
    Should Be Equal As Integers    ${tgt}    0x07

Command 0x02 Write-Single Latches The Gear And Drive Direction
    [Documentation]    cmd 0x02 with len 6 (write-single) and a valid gear (1..4) while
    ...                idle accepts the new gear: it latches G_FLAG_116 (target gear),
    ...                bumps the counter, and runs sched_task_extra, which — for a
    ...                target above the current position — sets the up-shift drive
    ...                direction 0x0F. Seed current gear 1, request gear 4.
    Create Leaf Machine
    Execute Command    sysbus WriteByte ${MOTOR_RUNNING} 0
    Execute Command    sysbus WriteByte ${STATE_115} 1
    Execute Command    sysbus WriteDoubleWord ${COUNTER} 0
    Inject Short Frame    0x20  0x06  0x00  0x02  0x00  0x04
    Process Rx Frame
    ${tgt}=    Read Byte    ${GEAR_TARGET}
    Should Be Equal As Integers    ${tgt}    4
    ${dir}=    Read Byte    ${DRIVE_DIR}
    Should Be Equal As Integers    ${dir}    0x0F
    ${ctr}=    Read Dword    ${COUNTER}
    Should Be Equal As Integers    ${ctr}    1
    ${f13e}=    Read Byte    ${FLAG_13E}
    Should Be Equal As Integers    ${f13e}    1

Command 0x02 Write-Single Rejects An Out-Of-Range Gear
    [Documentation]    A gear request outside 1..4 (here 5) must be rejected: neither
    ...                the target nor the counter changes. Guards the `1 <= gear <= 4`
    ...                acceptance window in the cmd-0x02 handler.
    Create Leaf Machine
    Execute Command    sysbus WriteByte ${MOTOR_RUNNING} 0
    Execute Command    sysbus WriteByte ${STATE_115} 1
    Execute Command    sysbus WriteByte ${GEAR_TARGET} 0
    Execute Command    sysbus WriteDoubleWord ${COUNTER} 0
    Inject Short Frame    0x20  0x06  0x00  0x02  0x00  0x05
    Process Rx Frame
    ${tgt}=    Read Byte    ${GEAR_TARGET}
    Should Be Equal As Integers    ${tgt}    0
    ${ctr}=    Read Dword    ${COUNTER}
    Should Be Equal As Integers    ${ctr}    0

Command 0x02 Short-Read Reports The Motor-Running State
    [Documentation]    cmd 0x02 with len 3 (short read) replies with (0, G_MOTOR_RUNNING)
    ...                via the image-status emit path. With the TX leaf hooked off, the
    ...                7-byte PDU is still assembled in the TX buffer; assert the busy
    ...                byte lands at TX[4]. Seed motor-running = 1.
    Create Leaf Machine
    Execute Command    sysbus WriteByte ${MOTOR_RUNNING} 1
    Inject Short Frame    0x20  0x03  0x00  0x02  0x00  0x00
    Process Rx Frame
    ${busy}=    Read Byte    ${{ ${TX_BUF} + 4 }}
    Should Be Equal As Integers    ${busy}    1

Command 0x95 Erases Staging And Enters Long-Frame Mode
    [Documentation]    cmd 0x95 arms an OTA: it erases the staging slot and flips the
    ...                RX FSM into long-frame (45-byte) mode so the next PDUs are
    ...                accepted as image payloads. Confirm G_RX_FRAME_MODE becomes 1.
    Create Leaf Machine
    Execute Command    sysbus WriteByte ${RX_FRAME_MODE} 0
    Inject Short Frame    0x20  0x95  0x00  0x95  0x00  0x00
    Process Rx Frame
    ${mode}=    Read Byte    ${RX_FRAME_MODE}
    Should Be Equal As Integers    ${mode}    1
