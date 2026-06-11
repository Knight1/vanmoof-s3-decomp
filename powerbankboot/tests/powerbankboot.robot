*** Settings ***
Documentation     Renode smoke tests for the VanMoof powerbankboot (STM32F091) loader.
...               Build the test image first:  make -C powerbankboot test
...               then run:  renode-test powerbankboot/tests/powerbankboot.robot
...               `make test` links the real src/ objects against tests/stubs.c (which
...               stands in for the STM32F0 HAL + X-CUBE-STL leaves the decomp leaves
...               extern) into build/powerbankboot_test.elf.
...
...               powerbankboot is the F0/Cortex-M0 sibling of bmsboot (L0/M0+): same
...               A/B (dual-bank) boot orchestrator + resident serial-download server,
...               but it persists its boot flag in the RTC backup registers (not a data
...               EEPROM) and, being Cortex-M0, runs its vectors at flash base (no VTOR).
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}

*** Variables ***
${ELF}            @${CURDIR}/../build/powerbankboot_test.elf
${REPL}           @${CURDIR}/powerbankboot.repl
# RTC backup registers (RTC_BASE 0x40002800): the persisted upgrade flag lives in
# BKP0R, its redundancy complement in BKP1R.
${RTC_BKP0R}      0x40002850
${RTC_BKP1R}      0x40002854
# Application bank base + image-header size: the real Cortex-M0 vector table sits
# at AP_BASE + 0x28 (initial SP at +0x28, reset entry at +0x2C).
${AP_BASE}        0x08008000
${AP_SP}          0x08008028
${AP_RESET}       0x0800802C
# Scratch header buffer + stack for the direct-call (leaf) tests, high in the
# 32 KB SRAM clear of the loader's own rings/state (which sit below ~0x20002700).
${SCRATCH}        0x20007000
${STACKTOP}       0x20008000

*** Keywords ***
Create Loader Machine
    [Documentation]    Build the machine and load the powerbankboot test ELF. The CPU
    ...                resets straight into the loader's own vector table at flash
    ...                0x08000000 (no VanMoof image header), so LoadELF's entry/SP are
    ...                correct as-is.
    Execute Command           mach create "powerbankboot"
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus LoadELF ${ELF}

Resolve Symbol
    [Documentation]    Look up a symbol's address in the loaded ELF, stripped.
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

Reach Resident Loop
    [Documentation]    Common setup for the resident-download-server tests. The TX is
    ...                an ISR-driven ring; on the bare platform no ISR services it, so
    ...                uart_tx_flush() (called once before the loop) would busy-wait on
    ...                the TX-complete state forever — short-circuit it (cpu.PC = cpu.LR).
    ...                Then log a marker at uart_rx_drain, the first call inside the
    ...                for(;;) server loop, so the suite can wait until the loader is
    ...                resident. The banner has already been enqueued by then.
    [Arguments]    ${marker}
    Create Log Tester    10
    Hook Return          uart_tx_flush
    ${drain}=    Resolve Symbol    uart_rx_drain
    Execute Command      cpu AddHook ${drain} "self.Log(LogLevel.Error, '${marker}')"

Return Trap
    [Documentation]    Address of Default_Handler (a `b .` self-loop) with the Thumb
    ...                bit set, used as LR so a called function parks the CPU harmlessly
    ...                on return (BX LR) instead of running off into undefined code.
    ${trap}=    Resolve Symbol     Default_Handler
    ${trap}=    Evaluate           ${trap} | 1
    [Return]    ${trap}

Call Leaf Function
    [Documentation]    Invoke a Thumb function by symbol name and return R0 (its return
    ...                value) as a stripped string. Arguments R0..R3 must already be set.
    ...                Sets up a fresh stack, points LR at the Return Trap self-loop, runs
    ...                a brief window so the call completes, then reads R0.
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

Seed Image Header
    [Documentation]    Write a VanMoof image header into flash at ${addr}: magic (+0),
    ...                crc32 (+8), size (+0xC). The other header words are left as-is.
    [Arguments]    ${addr}    ${magic}    ${crc32}    ${size}
    Execute Command    sysbus WriteDoubleWord ${addr} ${magic}
    ${crcaddr}=    Evaluate    ${addr} + 8
    ${szaddr}=     Evaluate    ${addr} + 0xC
    Execute Command    sysbus WriteDoubleWord ${crcaddr} ${crc32}
    Execute Command    sysbus WriteDoubleWord ${szaddr} ${size}

Drive OTA Bytes
    [Documentation]    Feed a sequence of bytes to the loader's serial-download protocol
    ...                the way the host does: write them into the USART2 RX ring, mark the
    ...                RX channel active, zero the TX ring write index (so replies land at
    ...                s_tx_buf[0]), then call uart_rx_drain() once — it loops the ring,
    ...                feeding each byte to ota_process_byte(). The OTA state (s_ota) and
    ...                replies persist in SRAM across the single drain call.
    [Arguments]    @{bytes}
    ${rxbuf}=    Resolve Symbol    s_rx_buf
    ${rxhead}=   Resolve Symbol    s_rx_head
    ${rxtail}=   Resolve Symbol    s_rx_tail
    ${rxstate}=  Resolve Symbol    s_rx_state
    ${txhead}=   Resolve Symbol    s_tx_head
    ${txtail}=   Resolve Symbol    s_tx_tail
    Execute Command    sysbus WriteByte ${rxstate} 0x20         # UART_READY (' ')
    Execute Command    sysbus WriteWord ${txhead} 0
    Execute Command    sysbus WriteWord ${txtail} 0
    ${n}=    Set Variable    ${0}
    FOR    ${b}    IN    @{bytes}
        ${addr}=    Evaluate    ${rxbuf} + ${n}
        Execute Command    sysbus WriteByte ${addr} ${b}
        ${n}=    Evaluate    ${n} + 1
    END
    Execute Command    sysbus WriteWord ${rxhead} ${n}
    Execute Command    sysbus WriteWord ${rxtail} 0
    ${trap}=    Return Trap
    Execute Command    cpu SetRegister 13 ${STACKTOP}
    Execute Command    cpu SetRegister 14 ${trap}
    ${drain}=    Resolve Symbol    uart_rx_drain
    Execute Command    cpu PC ${drain}
    Execute Command    emulation RunFor "0.02"

Tx Byte At Should Be
    [Documentation]    Assert the byte at offset ${off} of the loader's TX ring
    ...                (s_tx_buf) equals ${expected} — used to read back ACK (0x79) /
    ...                NAK (0x1F) protocol replies.
    [Arguments]    ${off}    ${expected}
    ${txbuf}=    Resolve Symbol    s_tx_buf
    ${a}=    Evaluate    ${txbuf} + ${off}
    ${v}=    Execute Command    sysbus ReadByte ${a}
    Should Be Equal As Integers    ${v}    ${expected}

Read Byte Symbol
    [Documentation]    Read a single byte at a resolved symbol and return it as an int.
    [Arguments]    ${symbol}
    ${a}=    Resolve Symbol    ${symbol}
    ${v}=    Execute Command    sysbus ReadByte ${a}
    ${v}=    Strip String       ${v}
    ${v}=    Convert To Integer    ${v}    16
    [Return]    ${v}

*** Test Cases ***
Vector Table Is Well Formed
    [Documentation]    Static check (no execution): vector slot 0 (initial SP) points
    ...                at the top of SRAM and slot 1 (reset PC) into loader flash with
    ...                the Thumb bit set — the CPU resets cleanly into this image.
    Create Loader Machine
    ${sp}=    Execute Command    sysbus ReadDoubleWord 0x08000000
    ${pc}=    Execute Command    sysbus ReadDoubleWord 0x08000004
    # Initial SP at the top of the 32 KB SRAM (0x20008000).
    Should Match Regexp    ${sp}    (?i)0x0*20008000
    # Reset vector in loader flash (0x0800_xxxx) with the Thumb bit (odd) set.
    Should Match Regexp    ${pc}    (?i)0x0*800[0-9a-f]*[13579bdf]

Reset Chain Reaches Main
    [Documentation]    The startup chain — Reset_Handler (set SP, init_data_bss:
    ...                .data copy + .bss zero) — must hand control to main(). main()
    ...                runs the STL log bring-up then calls boot_main; reaching it
    ...                needs no peripheral stubbing beyond the HAL link stubs.
    Create Loader Machine
    Create Log Tester         10
    ${main}=    Resolve Symbol    main
    Execute Command           cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Start Emulation
    Wait For Log Entry        REACHED_MAIN

Boot Reaches The Resident Download Server
    [Documentation]    Deeper boot: with both application banks blank (flash reads 0,
    ...                so image_verify fails the magic check), boot_main falls through
    ...                to case 4 — the resident serial-download server. Confirm the
    ...                loader reaches its for(;;) service loop (uart_rx_drain). If this
    ...                stalls, a bring-up step in boot_hw_init is busy-waiting.
    Create Loader Machine
    Reach Resident Loop    IN_LOOP
    Start Emulation
    Wait For Log Entry     IN_LOOP

UART Banner Is Queued For Transmission
    [Documentation]    UART output check. On the resident path the loader enqueues its
    ...                "I am VM-BATT BL" startup banner into the USART2 TX ring before
    ...                entering the server loop. Run to the loop, then confirm the bytes
    ...                queued in s_tx_buf are exactly the flash banner STR_BANNER,
    ...                byte-for-byte — the bytes the loader would clock onto the wire.
    Create Loader Machine
    Reach Resident Loop    BANNER_QUEUED
    ${banner}=    Resolve Symbol    STR_BANNER
    ${txbuf}=     Resolve Symbol    s_tx_buf
    Start Emulation
    Wait For Log Entry    BANNER_QUEUED
    # The 17-byte banner ("\\nI am VM-BATT BL\\r") the loader queued must match the
    # flash banner string byte-for-byte.
    ${expected}=    Execute Command    sysbus ReadBytes ${banner} 17
    ${actual}=      Execute Command    sysbus ReadBytes ${txbuf} 17
    Should Be Equal    ${actual}    ${expected}

Corrupt Upgrade-Flag Pair Is Reset In RTC Backup
    [Documentation]    boot_read_persistent_flags loads the upgrade flag (BKP0R) and
    ...                its complement (BKP1R) and, when they aren't a valid value+
    ...                complement pair, clears the flag to a safe 0 via store_boot_flag
    ...                — which rewrites BKP0R = 0 and BKP1R = ~0. Seed a deliberately
    ...                inconsistent pair (0xAB / 0x123) and confirm the loader detects
    ...                the corruption and rewrites both backup registers.
    Create Loader Machine
    Execute Command    sysbus WriteDoubleWord ${RTC_BKP0R} 0x000000AB
    Execute Command    sysbus WriteDoubleWord ${RTC_BKP1R} 0x00000123
    Reach Resident Loop    IN_LOOP
    Start Emulation
    Wait For Log Entry     IN_LOOP
    ${bkp0}=    Execute Command    sysbus ReadDoubleWord ${RTC_BKP0R}
    ${bkp0}=    Strip String       ${bkp0}
    Should Be Equal As Integers    ${bkp0}    0
    ${bkp1}=    Execute Command    sysbus ReadDoubleWord ${RTC_BKP1R}
    ${bkp1}=    Strip String       ${bkp1}
    Should Be Equal As Integers    ${bkp1}    0xFFFFFFFF

Valid AP Image Boots Via Goto Application
    [Documentation]    The core bootloader behaviour: when the application bank holds a
    ...                valid image, the loader hands control to it. Force image_verify
    ...                to report the bank good (return IMG_OK = 0), seed the application
    ...                reset vector at AP_BASE+0x2C to point at the loader's harmless
    ...                Default_Handler self-loop (and a plausible SP at +0x28), then
    ...                confirm boot_main takes goto_application and the CPU jumps to the
    ...                application's reset entry — i.e. it parks at that vector.
    Create Loader Machine
    ${trap}=    Resolve Symbol    Default_Handler
    ${trapt}=   Evaluate    ${trap} | 1
    Execute Command    sysbus WriteDoubleWord ${AP_SP} 0x20008000
    Execute Command    sysbus WriteDoubleWord ${AP_RESET} ${trapt}
    # Force both banks "valid" so boot_main takes the AP-good branch (case 2) and jumps.
    ${iv}=    Resolve Symbol    image_verify
    Execute Command    cpu AddHook ${iv} "self.SetRegisterUlong(0, 0); cpu.PC = cpu.LR"
    Execute Command    emulation RunFor "0.02"
    ${pc}=    Execute Command    cpu PC
    ${pc}=    Strip String    ${pc}
    Should Be Equal As Integers    ${pc}    ${trap}

# --- image_verify: the boot-decision validation primitive (direct leaf calls) ---
# image_verify(slot) checks magic (+0x00 == 0xAA55AA55) and size (+0x0C < 0x1C001),
# then an MPEG-2 CRC-32 over the header (with the crc/size words blanked) + body
# against the stored crc (+0x08). The HAL CRC unit is stubbed to return 0, so a
# header whose crc word is 0 verifies; any other crc word mismatches. Each case
# seeds a header in flash and calls image_verify directly.

Image Verify Rejects A Bad Magic
    [Documentation]    A bank whose first word isn't 0xAA55AA55 is rejected up front
    ...                with IMG_MAGIC_BAD (2) — before any CRC work.
    Create Loader Machine
    Seed Image Header    ${AP_BASE}    0x00000000    0x00000000    0x00000028
    Execute Command      cpu SetRegister 0 ${AP_BASE}
    ${rc}=    Call Leaf Function    image_verify
    Should Be Equal As Integers    ${rc}    2

Image Verify Rejects A CRC Mismatch
    [Documentation]    Magic + size valid but the stored CRC-32 (here 0xDEADBEEF)
    ...                doesn't match the computed one (stub CRC = 0) -> IMG_CRC_BAD (1).
    Create Loader Machine
    Seed Image Header    ${AP_BASE}    0xAA55AA55    0xDEADBEEF    0x00000028
    Execute Command      cpu SetRegister 0 ${AP_BASE}
    ${rc}=    Call Leaf Function    image_verify
    Should Be Equal As Integers    ${rc}    1

Image Verify Accepts A Matching Image
    [Documentation]    Magic + size valid and the stored CRC matches the computed CRC
    ...                (both 0 with the stubbed CRC unit) -> IMG_OK (0). Confirms the
    ...                accept path of the boot decision's validator.
    Create Loader Machine
    Seed Image Header    ${AP_BASE}    0xAA55AA55    0x00000000    0x00000028
    Execute Command      cpu SetRegister 0 ${AP_BASE}
    ${rc}=    Call Leaf Function    image_verify
    Should Be Equal As Integers    ${rc}    0

Store Boot Flag Persists Flag And Complement To RTC Backup
    [Documentation]    store_boot_flag(v) writes the value to BKP0R and its bitwise
    ...                complement to BKP1R (the redundancy pair boot_read_persistent_flags
    ...                cross-checks). Call it directly with 0x42 and confirm both backup
    ...                registers — 0x42 and 0xFFFFFFBD.
    Create Loader Machine
    Execute Command    cpu SetRegister 0 0x42
    Call Leaf Function    store_boot_flag
    ${bkp0}=    Execute Command    sysbus ReadDoubleWord ${RTC_BKP0R}
    ${bkp0}=    Strip String       ${bkp0}
    Should Be Equal As Integers    ${bkp0}    0x42
    ${bkp1}=    Execute Command    sysbus ReadDoubleWord ${RTC_BKP1R}
    ${bkp1}=    Strip String       ${bkp1}
    Should Be Equal As Integers    ${bkp1}    0xFFFFFFBD

# --- Serial-download ("Who?") OTA protocol over USART2 -----------------------
# The host drives the loader one byte at a time. These feed byte sequences through
# the RX ring into ota_process_byte() (via a single uart_rx_drain call) and read the
# single-byte ACK (0x79) / NAK (0x1F) replies back from the TX ring. A fresh machine
# starts with s_ota zeroed (state IDLE), so each test is an independent transaction.

OTA Who Keepalive Emits The Banner
    [Documentation]    The "W h o ? \\r" keepalive makes the resident loader re-announce
    ...                itself: the idle handler enqueues STR_BANNER on the terminating CR.
    ...                Confirm the banner bytes land in the TX ring.
    Create Loader Machine
    ${banner}=    Resolve Symbol    STR_BANNER
    ${txbuf}=     Resolve Symbol    s_tx_buf
    Drive OTA Bytes    0x57  0x68  0x6F  0x3F  0x0D
    ${expected}=    Execute Command    sysbus ReadBytes ${banner} 17
    ${actual}=      Execute Command    sysbus ReadBytes ${txbuf} 17
    Should Be Equal    ${actual}    ${expected}

OTA Erase Command With A Valid Address Is Acked
    [Documentation]    Full erase transaction: command header '1' 0xCE (cmd + ~cmd) then
    ...                argument "08 00 80 00 88" (the AP-bank start 0x08008000, big-endian,
    ...                with a correct trailing XOR). The loader ACKs the command, ACKs the
    ...                argument, latches the address, sets the busy bit and enters the data
    ...                phase. Confirm both ACKs, the data state, and the busy lock.
    Create Loader Machine
    Drive OTA Bytes    0x31  0xCE  0x08  0x00  0x80  0x00  0x88
    Tx Byte At Should Be    0    0x79          # command-header ACK
    Tx Byte At Should Be    1    0x79          # argument ACK
    # s_ota.state == OTA_ST_DATA (2): the address latched and the loader is ready for
    # payload blocks — i.e. the command + argument were accepted end to end.
    ${state}=    Read Byte Symbol    s_ota
    Should Be Equal As Integers    ${state}    2

OTA Argument With A Bad XOR Is NAKed
    [Documentation]    Same erase command but the argument's trailing XOR byte is wrong
    ...                (0x00 instead of 0x88) — the loader rejects it with a NAK and does
    ...                not enter the data phase.
    Create Loader Machine
    Drive OTA Bytes    0x31  0xCE  0x08  0x00  0x80  0x00  0x00
    Tx Byte At Should Be    0    0x79          # command-header ACK
    Tx Byte At Should Be    1    0x1F          # argument NAK (bad XOR)
    ${state}=    Read Byte Symbol    s_ota
    Should Not Be Equal As Integers    ${state}    2

OTA Argument Outside The AP Bank Is NAKed
    [Documentation]    A well-formed argument (XOR ok) whose address falls outside the
    ...                writable AP bank (here 0x00000000) is refused with a NAK — the
    ...                loader will only accept write targets in 0x08008000..0x08023FFF.
    Create Loader Machine
    Drive OTA Bytes    0x31  0xCE  0x00  0x00  0x00  0x00  0x00
    Tx Byte At Should Be    1    0x1F          # argument NAK (out of range)

OTA Data Block Is Programmed Into Flash
    [Documentation]    Complete erase + one data block in a single transaction: after the
    ...                erase command + address (0x08008000) the loader is in the data phase;
    ...                feed a block "L=4, 11 22 33 44, XOR" and confirm the payload word is
    ...                programmed into flash at 0x08008000 (the write-through program stub
    ...                lets the read-back verify pass) and the block is ACKed.
    Create Loader Machine
    Drive OTA Bytes    0x31  0xCE  0x08  0x00  0x80  0x00  0x88
    ...                0x04  0x11  0x22  0x33  0x44  0x41
    ${w}=    Execute Command    sysbus ReadDoubleWord ${AP_BASE}
    Should Match Regexp    ${w}    (?i)0x0*44332211
    # The data block was ACKed (command ACK, arg ACK, block ACK = three 0x79s).
    Tx Byte At Should Be    2    0x79
