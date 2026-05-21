"""Modbus RTU helpers for the VanMoof S3 inter-module bus.

Implements the wire format the eShifter speaks (slave addr `0x20`, baud
9600, 8-N-1, CRC16 with polynomial `0xA001` and init `0xFFFF`). The
frame builders are usable against both shifterboot (the loader's
OTA-server dispatch) and shifterware (the running app's gear / shift
counter registers).

Three pre-built command vocabularies are included:

  - SHIFTER_TOOL_*  — the read-gear / read-shifts / write-gear commands
    the VangelisBV/vanmoof-shifter-tool sends to a running shifter
    (https://github.com/VangelisBV/vanmoof-shifter-tool).
  - SHIFTERBOOT_*   — the ping / apply / erase / OTA-stream commands
    shifterboot's main dispatcher recognises (see
    `shifterboot/docs/protocol.md`).
"""

from __future__ import annotations

import struct
from typing import Sequence

SHIFTER_SLAVE_ADDR = 0x20

# Modbus function codes used on the S3 bus.
FUNC_READ_HOLDING     = 0x03
FUNC_WRITE_SINGLE     = 0x06
FUNC_WRITE_MULTIPLE   = 0x10


# ---------- CRC ------------------------------------------------------------

def modbus_crc16(data: bytes) -> int:
    """Modbus RTU CRC16 — polynomial 0xA001, init 0xFFFF, returned as
    a single 16-bit integer. Matches the bytes-on-the-wire layout
    (low byte first) when packed `<H`. Same algorithm as our
    `shifterboot/src/modbus.c::modbus_crc16`."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def append_crc(pdu: bytes) -> bytes:
    """Append the 2-byte CRC suffix in canonical Modbus RTU order
    (low byte then high byte)."""
    return pdu + struct.pack("<H", modbus_crc16(pdu))


def check_crc(frame: bytes) -> bool:
    """True if the last two bytes are a valid CRC over the rest."""
    if len(frame) < 3:
        return False
    return modbus_crc16(frame[:-2]) == struct.unpack("<H", frame[-2:])[0]


# ---------- Generic frame builders -----------------------------------------

def read_holding(slave: int, addr: int, qty: int) -> bytes:
    """Modbus func 0x03 — Read Holding Registers. 8-byte frame."""
    pdu = struct.pack(">BBHH", slave, FUNC_READ_HOLDING, addr, qty)
    return append_crc(pdu)


def write_single(slave: int, addr: int, value: int) -> bytes:
    """Modbus func 0x06 — Write Single Register. 8-byte frame."""
    pdu = struct.pack(">BBHH", slave, FUNC_WRITE_SINGLE, addr, value & 0xFFFF)
    return append_crc(pdu)


def write_multiple(slave: int, addr: int, values: Sequence[int]) -> bytes:
    """Modbus func 0x10 — Write Multiple Registers. Variable size.
    `values` is a sequence of 16-bit big-endian register values."""
    qty = len(values)
    byte_count = qty * 2
    pdu = struct.pack(">BBHHB", slave, FUNC_WRITE_MULTIPLE, addr, qty, byte_count)
    for v in values:
        pdu += struct.pack(">H", v & 0xFFFF)
    return append_crc(pdu)


# ---------- shifter-tool commands (shifterware app runtime) ----------------
#
# These reach the running shifter app on the bus, not the bootloader.
# Decoded from VangelisBV/vanmoof-shifter-tool's ModbusService.java +
# SwingUI.java.

def shifter_read_gear() -> bytes:
    """Read 1 holding register at addr 2 — returns the current gear."""
    return read_holding(SHIFTER_SLAVE_ADDR, addr=2, qty=1)


def shifter_read_shifts() -> bytes:
    """Read 2 holding registers at addr 15 — combined as a 32-bit
    total-shifts counter (high register first, low register second)."""
    return read_holding(SHIFTER_SLAVE_ADDR, addr=15, qty=2)


def shifter_write_gear(gear: int) -> bytes:
    """Write 1 register at addr 2 — set target gear (1..4)."""
    if not 1 <= gear <= 4:
        raise ValueError(f"gear must be 1..4, got {gear}")
    return write_single(SHIFTER_SLAVE_ADDR, addr=2, value=gear)


# ---------- shifterboot OTA-server commands --------------------------------
#
# These reach shifterboot's main dispatcher; see protocol.md.

def shifterboot_ping() -> bytes:
    """Read 1 register at sub_id = 0x01 — shifterboot replies with the
    pre-built template B (constant 0x0200 register value)."""
    return read_holding(SHIFTER_SLAVE_ADDR, addr=0x0001, qty=1)


def shifterboot_apply_image() -> bytes:
    """Read 1 register at sub_id = 0x81 — shifterboot runs
    image_verify_crc on slot 1 and replies with the status, then
    latches NVIC_SystemReset for the end of the loop iteration."""
    return read_holding(SHIFTER_SLAVE_ADDR, addr=0x0081, qty=1)


def shifterboot_erase_slot1() -> bytes:
    """Read 1 register at sub_id = 0x95 — shifterboot erases slot 1
    (the OTA staging slot) and echoes back the inbound frame as ack."""
    return read_holding(SHIFTER_SLAVE_ADDR, addr=0x0095, qty=1)


def shifterboot_ota_chunk(stream_pos: int, image_bytes: bytes) -> bytes:
    """OTA streaming chunk (func 0x10, sub_id = 0x82) — 45-byte frame
    carrying a 32-byte image chunk at `stream_pos` (big-endian byte
    offset within the staged image). `image_bytes` must be exactly 32
    bytes; if shorter, it's zero-padded (the OEM dispatcher always
    flushes 32 bytes regardless of the actual chunk size)."""
    if len(image_bytes) > 32:
        raise ValueError("OTA chunk image_bytes must be <= 32 bytes")
    payload = image_bytes.ljust(32, b"\x00")
    # The OEM wire layout the dispatcher decodes:
    #   [0]     slave         = 0x20
    #   [1]     func          = 0x10
    #   [2..3]  addr_be       = 0x82__ (sub_id at addr_lo = frame[3])
    #   [4..5]  qty_be        = 16    (16 regs of 2 B = 32 B payload)
    #   [6]     byte_count    = 32
    #   [7..10] stream_pos_be = big-endian byte offset
    #   [11..42] image bytes
    #   [43..44] CRC
    head = struct.pack(">BBHHB", SHIFTER_SLAVE_ADDR, FUNC_WRITE_MULTIPLE,
                       0x0082, 16, 32)
    stream = struct.pack(">I", stream_pos & 0xFFFFFFFF)
    return append_crc(head + stream + payload)


# ---------- Decoders -------------------------------------------------------

def decode_read_holding_response(frame: bytes) -> dict:
    """Parse a Modbus `0x03` Read Holding Registers reply. Returns a
    dict with `{slave, func, byte_count, values, crc_ok}`."""
    if len(frame) < 5:
        return {"error": "too short", "raw": frame}
    slave, func, byte_count = frame[0], frame[1], frame[2]
    if func != FUNC_READ_HOLDING:
        return {"error": f"not a read-holding reply (func=0x{func:02X})", "raw": frame}
    n_regs = byte_count // 2
    values = list(struct.unpack(">" + "H" * n_regs, frame[3:3 + byte_count]))
    return {
        "slave": slave,
        "func": func,
        "byte_count": byte_count,
        "values": values,
        "crc_ok": check_crc(frame[:5 + byte_count]),
        "raw": frame,
    }


def decode_write_single_response(frame: bytes) -> dict:
    """Parse a Modbus `0x06` Write Single Register reply (echo)."""
    if len(frame) < 8:
        return {"error": "too short", "raw": frame}
    slave, func, addr, value, crc = struct.unpack(">BBHHH", frame[:8])
    return {
        "slave": slave,
        "func": func,
        "addr": addr,
        "value": value,
        "crc_ok": check_crc(frame[:8]),
        "raw": frame,
    }


def hex_frame(b: bytes) -> str:
    """Render a frame as space-separated hex bytes."""
    return " ".join(f"{x:02X}" for x in b)
