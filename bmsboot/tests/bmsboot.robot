*** Settings ***
Documentation     Renode smoke tests for the VanMoof bmsboot (STM32L072) loader image.
...               Build the test image first:  make -C bmsboot test
...               then run:  renode-test bmsboot/tests/bmsboot.robot
...               `make test` links the real src/ objects against tests/stubs.c (which
...               stands in for the STM32L0 HAL/CMSIS leaves the decomp leaves extern)
...               into build/bmsboot_test.elf, and generates build/eeprom_test.bin.
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}

*** Variables ***
${ELF}            @${CURDIR}/../build/bmsboot_test.elf
${REPL}           @${CURDIR}/bmsboot.repl
${EEPROM}         @${CURDIR}/../build/eeprom_test.bin
${VTOR_RAM}       0x20000000      # boot_hw_init relocates the vector table here
${SCB_VTOR}       0xE000ED08      # SCB->VTOR register
${APP_BASE}       0x08005000      # application bank base (image header lives here)
# Scratch + stack for the direct-call (leaf) tests, high in the 20 KB SRAM
# (0x20000000..0x20005000) clear of the loader's own rings/state.
${SCRATCH}        0x20004000
${STACKTOP}       0x20005000

*** Keywords ***
Create Loader Machine
    [Documentation]    Build the machine and load the bmsboot test ELF. The CPU resets
    ...                straight into the loader's own vector table at flash 0x08000000
    ...                (no VanMoof image header), so LoadELF's entry/SP are correct as-is.
    Execute Command           mach create "bmsboot"
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus LoadELF ${ELF}

Resolve Symbol
    [Documentation]    Look up a symbol's address in the loaded ELF, stripped.
    [Arguments]    ${name}
    ${addr}=    Execute Command    sysbus GetSymbolAddress "${name}"
    ${addr}=    Strip String       ${addr}
    [Return]    ${addr}

Return Trap
    [Documentation]    Address of Default_Handler (a `b .` self-loop) with the Thumb bit
    ...                set, used as LR so a called function parks the CPU harmlessly on
    ...                return (BX LR) instead of running off into undefined code.
    ${trap}=    Resolve Symbol     Default_Handler
    ${trap}=    Evaluate           ${trap} | 1
    [Return]    ${trap}

Call Leaf Function
    [Documentation]    Invoke a Thumb function by symbol name and return R0 (its return
    ...                value), stripped. Arguments R0..R3 must already be set. Sets up a
    ...                fresh stack, points LR at the Return Trap self-loop, runs a brief
    ...                window so the call completes, then reads R0.
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
    [Documentation]    Write a VanMoof image header into flash at ${addr}: magic (+0x00),
    ...                crc32 (+0x08), size (+0x0C). Other header words are left as-is.
    [Arguments]    ${addr}    ${magic}    ${crc32}    ${size}
    Execute Command    sysbus WriteDoubleWord ${addr} ${magic}
    ${crcaddr}=    Evaluate    ${addr} + 8
    ${szaddr}=     Evaluate    ${addr} + 0xC
    Execute Command    sysbus WriteDoubleWord ${crcaddr} ${crc32}
    Execute Command    sysbus WriteDoubleWord ${szaddr} ${size}

Drive OTA Bytes
    [Documentation]    Feed a sequence of bytes to the loader's serial-download protocol
    ...                the way the host does: write them into the USART1 RX ring, zero the
    ...                TX ring indices (so replies land at s_tx_buf[0]), then call
    ...                uart_rx_drain() once — it loops the ring, feeding each byte to
    ...                ota_process_byte(). The OTA state (s_ota) and replies persist in
    ...                SRAM across the single drain call.
    [Arguments]    @{bytes}
    ${rxbuf}=    Resolve Symbol    s_rx_buf
    ${rxhead}=   Resolve Symbol    s_rx_head
    ${rxtail}=   Resolve Symbol    s_rx_tail
    ${txwidx}=   Resolve Symbol    s_tx_widx
    ${txridx}=   Resolve Symbol    s_tx_ridx
    ${enabled}=  Resolve Symbol    s_uart_enabled
    # The TX enqueue (uart_tx_byte/string) no-ops unless the comms port is up; on the
    # normal download path download_pin_check sets this — assert it directly here.
    Execute Command    sysbus WriteByte ${enabled} 1
    Execute Command    sysbus WriteWord ${txwidx} 0
    Execute Command    sysbus WriteWord ${txridx} 0
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
    [Documentation]    Assert the byte at offset ${off} of the loader's TX ring (s_tx_buf)
    ...                equals ${expected} — used to read back ACK (0x79) / NAK (0x1F) replies.
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
    ...                the Thumb bit set — the CPU will reset cleanly into this image.
    Create Loader Machine
    ${sp}=    Execute Command    sysbus ReadDoubleWord 0x08000000
    ${pc}=    Execute Command    sysbus ReadDoubleWord 0x08000004
    # Initial SP at the top of the 20 KB SRAM (0x20005000).
    Should Match Regexp    ${sp}    (?i)0x0*20005000
    # Reset vector in loader flash (0x0800_xxxx) with the Thumb bit (odd) set.
    Should Match Regexp    ${pc}    (?i)0x0*800[0-9a-f]*[13579bdf]

Reset Chain Reaches Main
    [Documentation]    The startup chain — Reset_Handler (.data copy, .bss zero, libc
    ...                init) then boot_hw_init (vector relocation, clock/CRC/IWDG/GPIO
    ...                bring-up via the HAL stubs) — must hand control to main().
    Create Loader Machine
    Create Log Tester         10
    ${main}=    Resolve Symbol    main
    Execute Command           cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Start Emulation
    Wait For Log Entry        REACHED_MAIN

Boot Relocates The Vector Table Into SRAM
    [Documentation]    Loader-specific: boot_hw_init copies the 0xC0-byte vector table
    ...                to SRAM 0x20000000 and points SCB->VTOR at it. After reaching
    ...                main, VTOR must equal 0x20000000 and the relocated table's first
    ...                two words (SP, reset) must match the flash originals.
    Create Loader Machine
    Create Log Tester         10
    ${main}=    Resolve Symbol    main
    Execute Command           cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Start Emulation
    Wait For Log Entry        REACHED_MAIN
    ${vtor}=    Execute Command    sysbus ReadDoubleWord ${SCB_VTOR}
    Should Match Regexp       ${vtor}    (?i)0x0*20000000
    ${ram_sp}=    Execute Command    sysbus ReadDoubleWord ${VTOR_RAM}
    ${flash_sp}=    Execute Command    sysbus ReadDoubleWord 0x08000000
    Should Be Equal           ${ram_sp}    ${flash_sp}

UART Banner Is Queued For Transmission
    [Documentation]    UART output check. With the download pin (PA10) asserted the
    ...                loader brings up USART1 and main() queues the V007 startup banner
    ...                into the TX ring. Force the pin high (hal_gpio_read -> 1) and
    ...                short-circuit the blocking uart_tx_flush (no ISR services TX on
    ...                the bare platform), then confirm the bytes the loader queued for
    ...                transmission are exactly the "I am VanMoof BL V007 ..." banner.
    Create Loader Machine
    Create Log Tester         10
    ${gpio_read}=    Resolve Symbol    hal_gpio_read
    ${flush}=        Resolve Symbol    uart_tx_flush
    ${banner}=       Resolve Symbol    STR_BANNER_V007
    ${txbuf}=        Resolve Symbol    s_tx_buf
    # PA10 high -> download_pin_check brings USART1 up (s_uart_enabled = 1).
    Execute Command    cpu AddHook ${gpio_read} "self.SetRegisterUlong(0, 1); cpu.PC = cpu.LR"
    # uart_tx_flush would spin forever waiting on the TX-complete ISR — signal that
    # the banner has been queued (the enqueue happens on the line before) and return.
    Execute Command    cpu AddHook ${flush} "self.Log(LogLevel.Error, 'BANNER_QUEUED'); cpu.PC = cpu.LR"
    Start Emulation
    Wait For Log Entry    BANNER_QUEUED
    # The 42-byte banner (incl. leading \\n and trailing \\r) the loader queued must
    # match the flash banner string byte-for-byte.
    ${expected}=    Execute Command    sysbus ReadBytes ${banner} 42
    ${actual}=      Execute Command    sysbus ReadBytes ${txbuf} 42
    Should Be Equal    ${actual}    ${expected}

Normal Boot Leaves The Comms Port Disabled
    [Documentation]    With PA10 low (hal_gpio_read -> 0, the stub default) the loader
    ...                takes comms_disable, never bringing USART1 up — so the banner
    ...                enqueue is a no-op and the TX ring stays empty. Confirms the
    ...                download path is properly gated.
    Create Loader Machine
    Create Log Tester         10
    ${main}=     Resolve Symbol    main
    ${enabled}=  Resolve Symbol    s_uart_enabled
    Execute Command    cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Start Emulation
    Wait For Log Entry    REACHED_MAIN
    ${en}=    Execute Command    sysbus ReadByte ${enabled}
    Should Be Equal As Integers    ${en}    0

# --- EEPROM-backed boot-flag state machine -----------------------------------
# The persisted boot flag at EEPROM 0x08080000 selects the boot behaviour. Seed it
# before reset and confirm main() acts on it. (Requires the EEPROM region in the
# .repl and the write-through hal_flash_program in stubs.c, so flash_program's
# read-back-verify loop terminates and the rewritten flag is observable.)

Boot Flag ACK Is Rewritten To Normal
    [Documentation]    Boot flag 0x33 (ACK, "just wrote the flag") makes the loader
    ...                rewrite it to 0x55 (NORMAL) and fall through to the resident
    ...                loop. Seed 0x33, run to the super-loop, confirm the flag is 0x55.
    Create Loader Machine
    Create Log Tester         10
    ${drain}=    Resolve Symbol    uart_rx_drain
    Execute Command    cpu AddHook ${drain} "self.Log(LogLevel.Error, 'IN_LOOP')"
    Execute Command    sysbus WriteByte 0x08080000 0x33
    Start Emulation
    Wait For Log Entry    IN_LOOP
    ${flag}=    Execute Command    sysbus ReadByte 0x08080000
    Should Be Equal As Integers    ${flag}    0x55

Boot Flag Wipe Is Rewritten To Normal
    [Documentation]    Boot flag 0x5A (WIPE, force serial download) makes the loader
    ...                rewrite it to 0x55 and erase the AP + Shadow banks. Seed 0x5A,
    ...                run to the super-loop, confirm the flag is 0x55.
    Create Loader Machine
    Create Log Tester         10
    ${drain}=    Resolve Symbol    uart_rx_drain
    Execute Command    cpu AddHook ${drain} "self.Log(LogLevel.Error, 'IN_LOOP')"
    Execute Command    sysbus WriteByte 0x08080000 0x5A
    Start Emulation
    Wait For Log Entry    IN_LOOP
    ${flag}=    Execute Command    sysbus ReadByte 0x08080000
    Should Be Equal As Integers    ${flag}    0x55

Reset Cause Is Persisted To EEPROM
    [Documentation]    boot_hw_init saves RCC_CSR (the reset-cause flags) to EEPROM
    ...                0x08080002. Tag RCC_CSR with a known value and confirm the
    ...                loader persists its low byte there (flash_program is byte-wise).
    Create Loader Machine
    Execute Command    sysbus Tag <0x40021050 4> "RCC_CSR" 0x0A000000
    Create Log Tester         10
    ${drain}=    Resolve Symbol    uart_rx_drain
    Execute Command    cpu AddHook ${drain} "self.Log(LogLevel.Error, 'IN_LOOP')"
    Start Emulation
    Wait For Log Entry    IN_LOOP
    # RCC_CSR low byte (0x00) lands at +2; byte 3 (0x0A) lands at +5.
    ${cause3}=    Execute Command    sysbus ReadByte 0x08080005
    Should Be Equal As Integers    ${cause3}    0x0A

Real Provisioned EEPROM Boots Normally
    [Documentation]    Real-data variant: load a complete tool-generated EEPROM image
    ...                (make test — tools/eeprom_example.py, boot flag 0x55 = normal)
    ...                instead of seeding individual bytes, and confirm the loader reads
    ...                it, takes the normal-boot path, and leaves the flag at 0x55 (it
    ...                only rewrites the 0x33/0x5A flags). Exercises the boot decision
    ...                against a realistic provisioned EEPROM.
    Create Loader Machine
    Execute Command    sysbus LoadBinary ${EEPROM} 0x08080000
    Create Log Tester         10
    ${drain}=    Resolve Symbol    uart_rx_drain
    Execute Command           cpu AddHook ${drain} "self.Log(LogLevel.Error, 'IN_LOOP')"
    Start Emulation
    Wait For Log Entry        IN_LOOP
    ${flag}=    Execute Command    sysbus ReadByte 0x08080000
    Should Be Equal As Integers    ${flag}    0x55

# --- image_verify: the boot-decision validation primitive (direct leaf calls) ---
# image_verify(slot) checks magic (+0x00 == 0xAA55AA55) and size (+0x0C < 0x15801),
# then an MPEG-2 CRC-32 (the HW CRC unit, stubbed to return 0) over the header (with
# the crc/size words blanked) + body, compared against the stored crc (+0x08). So a
# header whose crc word is 0 verifies; any other crc word mismatches. Each case seeds
# a header in the AP bank and calls image_verify directly.

Image Verify Rejects A Bad Magic
    [Documentation]    A bank whose first word isn't 0xAA55AA55 is rejected up front with
    ...                IMG_MAGIC_BAD (2) — before any CRC work.
    Create Loader Machine
    Seed Image Header    ${APP_BASE}    0x00000000    0x00000000    0x00000028
    Execute Command      cpu SetRegister 0 ${APP_BASE}
    ${rc}=    Call Leaf Function    image_verify
    Should Be Equal As Integers    ${rc}    2

Image Verify Rejects A CRC Mismatch
    [Documentation]    Magic + size valid but the stored CRC-32 (here 0xDEADBEEF) doesn't
    ...                match the computed one (stub CRC = 0) -> IMG_CRC_BAD (1).
    Create Loader Machine
    Seed Image Header    ${APP_BASE}    0xAA55AA55    0xDEADBEEF    0x00000028
    Execute Command      cpu SetRegister 0 ${APP_BASE}
    ${rc}=    Call Leaf Function    image_verify
    Should Be Equal As Integers    ${rc}    1

Image Verify Accepts A Matching Image
    [Documentation]    Magic + size valid and the stored CRC matches the computed CRC
    ...                (both 0 with the stubbed CRC unit) -> IMG_OK (0). Confirms the accept
    ...                path of the boot decision's validator.
    Create Loader Machine
    Seed Image Header    ${APP_BASE}    0xAA55AA55    0x00000000    0x00000028
    Execute Command      cpu SetRegister 0 ${APP_BASE}
    ${rc}=    Call Leaf Function    image_verify
    Should Be Equal As Integers    ${rc}    0

# --- Serial-download ("WHO?") OTA protocol over USART1 -----------------------
# The host drives the loader one byte at a time. These feed byte sequences through
# the RX ring into ota_process_byte() (via a single uart_rx_drain call) and read the
# single-byte ACK (0x79) / NAK (0x1F) replies back from the TX ring. A fresh machine
# starts with s_ota zeroed (state IDLE), so each test is an independent transaction.

OTA Who Keepalive Emits The Banner
    [Documentation]    The "W H O ? \\r" keepalive makes the resident loader re-announce
    ...                itself: the idle handler enqueues STR_BANNER_WHO on the terminating
    ...                CR. Confirm the banner bytes land in the TX ring.
    Create Loader Machine
    ${banner}=    Resolve Symbol    STR_BANNER_WHO
    ${txbuf}=     Resolve Symbol    s_tx_buf
    Drive OTA Bytes    0x57  0x48  0x4F  0x3F  0x0D
    ${expected}=    Execute Command    sysbus ReadBytes ${banner} 23
    ${actual}=      Execute Command    sysbus ReadBytes ${txbuf} 23
    Should Be Equal    ${actual}    ${expected}

OTA Set-Address Command With A Valid Address Is Acked
    [Documentation]    Full set-address transaction: command header '1' 0xCE (cmd + ~cmd)
    ...                then argument "08 00 50 00 58" (the AP-bank base 0x08005000, big-
    ...                endian, with a correct trailing XOR). The loader ACKs the command,
    ...                ACKs the argument, latches the address and enters the data phase.
    ...                Confirm both ACKs and the data state.
    Create Loader Machine
    Drive OTA Bytes    0x31  0xCE  0x08  0x00  0x50  0x00  0x58
    Tx Byte At Should Be    0    0x79          # command-header ACK
    Tx Byte At Should Be    1    0x79          # argument ACK
    # s_ota.state == OTA_ST_DATA (2): the address latched and the loader awaits payload.
    ${state}=    Read Byte Symbol    s_ota
    Should Be Equal As Integers    ${state}    2

OTA Argument With A Bad XOR Is NAKed
    [Documentation]    Same set-address command but the argument's trailing XOR byte is
    ...                wrong (0x00 instead of 0x58) — the loader rejects it with a NAK and
    ...                does not enter the data phase.
    Create Loader Machine
    Drive OTA Bytes    0x31  0xCE  0x08  0x00  0x50  0x00  0x00
    Tx Byte At Should Be    0    0x79          # command-header ACK
    Tx Byte At Should Be    1    0x1F          # argument NAK (bad XOR)
    ${state}=    Read Byte Symbol    s_ota
    Should Not Be Equal As Integers    ${state}    2

OTA Argument Below The AP Bank Is NAKed
    [Documentation]    A well-formed argument (XOR ok) whose address falls below the
    ...                writable AP bank (here 0x08004000 <= OTA_LO_BOUND) is refused with a
    ...                NAK — the loader only accepts write targets at/above APP_BASE.
    Create Loader Machine
    Drive OTA Bytes    0x31  0xCE  0x08  0x00  0x40  0x00  0x48
    Tx Byte At Should Be    1    0x1F          # argument NAK (out of range)

OTA Data Block Is Programmed Into Flash
    [Documentation]    Complete set-address + one data block in a single transaction: after
    ...                the command + address (0x08005080, a non-header page so it programs
    ...                inline) the loader is in the data phase; feed a block "L=3, 11 22 33
    ...                44, XOR" and confirm the payload word is programmed into flash at
    ...                0x08005080 (the write-through half-page stub lets flash_program_verify
    ...                succeed) and the block is ACKed.
    Create Loader Machine
    Drive OTA Bytes    0x31  0xCE  0x08  0x00  0x50  0x80  0xD8
    ...                0x03  0x11  0x22  0x33  0x44  0x47
    ${w}=    Execute Command    sysbus ReadDoubleWord 0x08005080
    Should Match Regexp    ${w}    (?i)0x0*44332211
    # Three ACKs: command, argument, data block.
    Tx Byte At Should Be    2    0x79
