*** Settings ***
Documentation     Renode smoke tests for the VanMoof shifterboot (MM32F031F6U6) loader.
...               Build first:  make -C shifterboot all
...               then run:     renode-test shifterboot/tests/shifterboot.robot
...               (paths below are relative to the repo root).
...
...               shifterboot is the eShifter bootloader in the first 6 KB of flash.
...               On a cold boot it validates the two image slots (slot 1 = OTA staging
...               @ 0x08001800, slot 2 = active image @ 0x08004800) and either boots a
...               valid image (boot_app), requests a reset to re-evaluate, or — when
...               neither slot validates — stays resident as a Modbus RTU OTA server on
...               USART1 (this module answers as slave 0x20). The MindMotion HAL leaves
...               are satisfied by the firmware's own weak hal_stubs.S, so the image
...               links and runs standalone; the suite only supplies the two RCC ready
...               bits the boot clock bring-up spins on.
Suite Setup       Setup
Suite Teardown    Teardown
Test Teardown     Test Teardown
Resource          ${RENODEKEYWORDS}

*** Variables ***
${ELF}            @${CURDIR}/../build/shifterboot.elf
${REPL}           @${CURDIR}/shifterboot.repl
# Image slot bases + their vector tables (slot base + 40 B image header).
${SLOT1}          0x08001800      # OTA staging slot
${SLOT2}          0x08004800      # active image slot
${SLOT2_VEC}      0x08004828      # slot 2 vector table (SP @ +0, reset @ +4)
${IMAGE_MAGIC}    0xAA55AA55
${TYPE_SHIFTER}   0x000000C1      # version word low byte = TYPE_ID for shifterware
# Modbus RX accumulator filled by USART1_IRQHandler / polled by main's server loop.
${RX_BUF}         0x200000C4
${RX_IDX}         0x20000014
# Leaf-call scratch buffer + stack, high in the 4 KB SRAM, clear of the loader's
# own globals (RX accumulator, OTA scratch, bss — all below ~0x20000210) and of
# the loader's runtime stack (grows down from _estack 0x200006F8).
${SCRATCH}        0x20000C00
${STACKTOP}       0x20001000

*** Keywords ***
Create Loader Machine
    [Documentation]    Build the machine and load the loader image. The CPU resets
    ...                straight into the loader's own vector table at flash 0x08000000
    ...                (no VanMoof image header), so LoadELF's entry/SP are correct
    ...                as-is. The two RCC ready bits that set_sysclock_to_48m spins on
    ...                during boot (RCC_CR.HSIRDY and RCC_CFGR.SWS=PLL) are tagged so
    ...                the clock bring-up completes instead of hanging.
    Execute Command           mach create "shifterboot"
    Execute Command           machine LoadPlatformDescription ${REPL}
    Execute Command           sysbus LoadELF ${ELF}
    Execute Command           sysbus Tag <0x40021000 4> "RCC_CR" 0x00000002
    Execute Command           sysbus Tag <0x40021004 4> "RCC_CFGR" 0x00000008

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
    Execute Command           emulation RunFor "0.002"
    ${r0}=    Execute Command    cpu GetRegister 0
    ${r0}=    Strip String      ${r0}
    [Return]    ${r0}

Seed Image Header
    [Documentation]    Write a VanMoof image header into flash at ${addr}: magic (+0),
    ...                version word (+4, low byte = TYPE_ID), stored crc32 (+8) and
    ...                length (+0xC). The remaining header words are left as-is.
    [Arguments]    ${addr}    ${magic}    ${version}    ${crc32}    ${length}
    Execute Command    sysbus WriteDoubleWord ${addr} ${magic}
    Execute Command    sysbus WriteDoubleWord ${{ ${addr} + 0x4 }} ${version}
    Execute Command    sysbus WriteDoubleWord ${{ ${addr} + 0x8 }} ${crc32}
    Execute Command    sysbus WriteDoubleWord ${{ ${addr} + 0xC }} ${length}

Arm Ping Injection
    [Documentation]    Hook mdelay (the 250 ms settle main runs once at the end of phase 1,
    ...                before the boot decision) to deposit a Modbus "ping" frame into the
    ...                RX accumulator and return immediately. The frame — slave 0x20, func
    ...                0x03, sub-id 0x01, valid CRC — survives into phase 2, where the
    ...                resident server consumes it and replies via uart1_send_buf. Hooking
    ...                here (rather than pre-seeding) is necessary because Reset_Handler
    ...                zeroes the SRAM that holds the accumulator.
    ${mdelay}=    Resolve Symbol    mdelay
    Execute Command    cpu AddHook ${mdelay} "self.Bus.WriteByte(0x200000C4,0x20); self.Bus.WriteByte(0x200000C5,0x03); self.Bus.WriteByte(0x200000C6,0x00); self.Bus.WriteByte(0x200000C7,0x01); self.Bus.WriteByte(0x200000C8,0x00); self.Bus.WriteByte(0x200000C9,0x00); self.Bus.WriteByte(0x200000CA,0x12); self.Bus.WriteByte(0x200000CB,0xBB); self.Bus.WriteWord(0x20000014,8); cpu.PC = cpu.LR"

Read PC
    [Documentation]    Read the CPU program counter and return it as an integer.
    ${pc}=    Execute Command    cpu PC
    ${pc}=    Strip String       ${pc}
    ${pc}=    Convert To Integer    ${pc}    16
    [Return]    ${pc}

*** Test Cases ***
# --- Boot / liveness ---------------------------------------------------------

Vector Table Is Well Formed
    [Documentation]    Static check (no execution): vector slot 0 (initial SP) points at
    ...                the loader's stack top and slot 1 (reset PC) into loader flash with
    ...                the Thumb bit set — the CPU resets cleanly into this image.
    Create Loader Machine
    ${sp}=    Execute Command    sysbus ReadDoubleWord 0x08000000
    ${pc}=    Execute Command    sysbus ReadDoubleWord 0x08000004
    # Initial SP = _estack (0x200006F8), inside the 4 KB SRAM.
    Should Match Regexp    ${sp}    (?i)0x0*200006F8
    # Reset vector in loader flash (0x0800_0xxx) with the Thumb bit (odd) set.
    Should Match Regexp    ${pc}    (?i)0x0*800[0-9a-f]*[13579bdf]

Reset Chain Reaches Main
    [Documentation]    The startup chain — Reset_Handler → SystemInit (set_sysclock_to_48m
    ...                clock bring-up) → .data copy + .bss zero — must hand control to
    ...                main(). With the RCC ready bits tagged the clock bring-up completes
    ...                and main is entered; a hard requirement for a bootable image.
    Create Loader Machine
    Create Log Tester         15
    ${main}=    Resolve Symbol    main
    Execute Command           cpu AddHook ${main} "self.Log(LogLevel.Error, 'REACHED_MAIN')"
    Start Emulation
    Wait For Log Entry        REACHED_MAIN

Cold Boot Of A Valid Active Image Jumps To It
    [Documentation]    The core bootloader behaviour: when the active slot (slot 2) holds
    ...                a valid image and the staging slot is blank, the loader hands control
    ...                to the application via boot_app. Seed slot 2's magic and a vector
    ...                table whose reset entry points at the loader's harmless Default_Handler
    ...                self-loop, then run past the 250 ms boot settle and confirm the CPU
    ...                jumped to that reset vector (i.e. it parks there).
    Create Loader Machine
    Execute Command    sysbus WriteDoubleWord ${SLOT2} ${IMAGE_MAGIC}
    ${trap}=    Resolve Symbol    Default_Handler
    ${trapt}=   Evaluate    ${trap} | 1
    Execute Command    sysbus WriteDoubleWord ${SLOT2_VEC} 0x200006F8
    Execute Command    sysbus WriteDoubleWord ${{ ${SLOT2_VEC} + 0x4 }} ${trapt}
    Execute Command    emulation RunFor "0.35"
    ${pc}=    Read PC
    Should Be Equal As Integers    ${pc}    ${trap}

Cold Boot Of A Valid Staging Slot Requests A Reset
    [Documentation]    When the staging slot (slot 1) holds a valid shifterware-typed
    ...                image (matching the slot 2 CRC, here both 0) but slot 2 is blank,
    ...                the boot decision is "valid | valid/invalid" → NVIC_SystemReset to
    ...                re-evaluate. Seed slot 1 with a valid header (magic, TYPE_ID 0xC1,
    ...                crc 0, in-range length) and confirm the loader requests a reset.
    Create Loader Machine
    Seed Image Header    ${SLOT1}    ${IMAGE_MAGIC}    ${TYPE_SHIFTER}    0x00000000    0x00000100
    Create Log Tester    30
    ${rst}=    Resolve Symbol    NVIC_SystemReset
    Execute Command    cpu AddHook ${rst} "self.Log(LogLevel.Error, 'SYSTEM_RESET')"
    Start Emulation
    Wait For Log Entry    SYSTEM_RESET    timeout=30

Both Slots Blank Reaches The Modbus OTA Server
    [Documentation]    With both image slots blank the cold-boot decision falls through to
    ...                the resident Modbus RTU server. Drive that path end to end: inject a
    ...                "ping" command (slave 0x20, func 0x03, sub-id 0x01) into the RX
    ...                accumulator and confirm the loader, now resident, dispatches it and
    ...                emits a reply via uart1_send_buf — proving the bus command reached
    ...                and was serviced by the OTA server.
    Create Loader Machine
    Arm Ping Injection
    Create Log Tester    30
    ${send}=    Resolve Symbol    uart1_send_buf
    Execute Command    cpu AddHook ${send} "self.Log(LogLevel.Error, 'REPLY_SENT')"
    Start Emulation
    Wait For Log Entry    REPLY_SENT    timeout=30

# --- Modbus RTU CRC primitive (direct leaf calls) ----------------------------

CRC-16 Modbus Standard Check Value
    [Documentation]    modbus_crc16 over ASCII "123456789" must equal 0x4B37 — the
    ...                canonical Modbus/CRC-16 check value (poly 0xA001, init 0xFFFF) the
    ...                loader uses to validate every inbound OTA frame.
    Create Loader Machine
    Execute Command    sysbus WriteByte 0x20000C00 0x31
    Execute Command    sysbus WriteByte 0x20000C01 0x32
    Execute Command    sysbus WriteByte 0x20000C02 0x33
    Execute Command    sysbus WriteByte 0x20000C03 0x34
    Execute Command    sysbus WriteByte 0x20000C04 0x35
    Execute Command    sysbus WriteByte 0x20000C05 0x36
    Execute Command    sysbus WriteByte 0x20000C06 0x37
    Execute Command    sysbus WriteByte 0x20000C07 0x38
    Execute Command    sysbus WriteByte 0x20000C08 0x39
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 9
    ${crc}=    Call Leaf Function    modbus_crc16
    Should Be Equal As Integers    ${crc}    0x4B37

CRC-16 Of An Empty Buffer Is The Init Value
    [Documentation]    With count 0 the loop never runs, so modbus_crc16 returns the
    ...                initial register 0xFFFF unchanged — guards the `while (count != 0)`
    ...                boundary.
    Create Loader Machine
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 0
    ${crc}=    Call Leaf Function    modbus_crc16
    Should Be Equal As Integers    ${crc}    0xFFFF

# --- image_verify_crc: the boot-decision validator (direct leaf calls) --------
# image_verify_crc validates the staging image at flash 0x08001800: magic (+0 ==
# 0xAA55AA55) then length (+0xC < 0x3000), then an MPEG-2 CRC-32 over the masked
# header + payload against the stored crc (+8). The hardware CRC unit is unmodelled
# (reads 0), so a header whose stored crc word is 0 with no payload verifies; any
# other stored crc mismatches.

Image Verify Rejects A Bad Magic
    [Documentation]    A staging slot whose first word isn't 0xAA55AA55 is rejected up
    ...                front with IMAGE_HDR_INVALID (2) — before any CRC work.
    Create Loader Machine
    Seed Image Header    ${SLOT1}    0x00000000    ${TYPE_SHIFTER}    0x00000000    0x00000028
    ${rc}=    Call Leaf Function    image_verify_crc
    Should Be Equal As Integers    ${rc}    2

Image Verify Rejects An Oversized Length
    [Documentation]    Valid magic but a length field >= the 12 KB cap (IMAGE_MAX_SIZE
    ...                0x3000) is rejected with IMAGE_HDR_INVALID (2) — the length sanity
    ...                check fires before the CRC compute.
    Create Loader Machine
    Seed Image Header    ${SLOT1}    ${IMAGE_MAGIC}    ${TYPE_SHIFTER}    0x00000000    0x00003000
    ${rc}=    Call Leaf Function    image_verify_crc
    Should Be Equal As Integers    ${rc}    2

Image Verify Accepts A Matching Image
    [Documentation]    Valid magic + in-range length + a stored CRC that matches the
    ...                computed one (both 0 with the unmodelled CRC unit, header-only
    ...                image so no payload words) -> IMAGE_OK (0). Confirms the accept
    ...                path of the boot decision's validator.
    Create Loader Machine
    Seed Image Header    ${SLOT1}    ${IMAGE_MAGIC}    ${TYPE_SHIFTER}    0x00000000    0x00000028
    ${rc}=    Call Leaf Function    image_verify_crc
    Should Be Equal As Integers    ${rc}    0

# --- Image-header stream helpers + OTA chunk flasher (direct leaf calls) ------

Image Read U32 Reassembles A Little-Endian Word
    [Documentation]    image_read_u32_le(src, staging) reads two halfwords from `src` and
    ...                returns (staging[1] << 16) | staging[0] — the LE u32 layout main
    ...                uses to pull magic/crc/length out of an image header. Seed the two
    ...                halfwords 0x5678 and 0x1234 and expect 0x12345678.
    Create Loader Machine
    Execute Command    sysbus WriteWord ${SCRATCH} 0x5678
    Execute Command    sysbus WriteWord ${{ ${SCRATCH} + 0x2 }} 0x1234
    Execute Command    cpu SetRegister 0 ${SCRATCH}
    Execute Command    cpu SetRegister 1 ${{ ${SCRATCH} + 0x10 }}
    ${v}=    Call Leaf Function    image_read_u32_le
    Should Be Equal As Integers    ${v}    0x12345678

OTA Chunk Is Programmed Into Flash
    [Documentation]    ota_program_chunk(dst, frame, count) packs the LE halfwords from
    ...                the frame payload (which starts 11 bytes in) into the OTA scratch and
    ...                writes them to flash via flash_program_range. With the FLASH status
    ...                registers unmodelled the program loop runs through and the write-to-
    ...                MappedMemory lands, so feed a 4-byte payload "11 22 33 44" at frame
    ...                offset 11 and confirm the first word appears at the destination.
    Create Loader Machine
    Execute Command    sysbus WriteByte ${{ ${SCRATCH} + 11 }} 0x11
    Execute Command    sysbus WriteByte ${{ ${SCRATCH} + 12 }} 0x22
    Execute Command    sysbus WriteByte ${{ ${SCRATCH} + 13 }} 0x33
    Execute Command    sysbus WriteByte ${{ ${SCRATCH} + 14 }} 0x44
    Execute Command    cpu SetRegister 0 ${SLOT1}
    Execute Command    cpu SetRegister 1 ${SCRATCH}
    Execute Command    cpu SetRegister 2 4
    Call Leaf Function    ota_program_chunk
    ${w}=    Execute Command    sysbus ReadDoubleWord ${SLOT1}
    Should Match Regexp    ${w}    (?i)0x0*44332211
