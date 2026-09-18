#!/usr/bin/env python3
"""Extract the declared common-string buffer from the pinned exact35 converter.

This bounded observation handles only the validated arm64 Mach-O layout below.
It imports no product parser, executes no Unity binary and creates no scratch.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct


SOURCE = Path("/Applications/Unity/Unity.app/Contents/Tools/binary2text")
TABLE_BYTES = 1170
TABLE_SHA256 = "4a6ece766a82003fcb86398159b54e5ae84de95b33752950c89b34ea6465444e"
MAX_SOURCE_BYTES = 1024 * 1024

SYMBOLS = {
    "begin": "__ZN5Unity12CommonString11BufferBeginE",
    "end": "__ZN5Unity12CommonString9BufferEndE",
    "buffer": "__ZN5Unity12CommonStringL13gStringBufferE",
    "constructor": "__ZN17CommonStringTableC2E10MemLabelId",
}

# Compact instructions corroborated in the closed Binary Ninja observation.
# The bytes are checked directly against the selected source file.
CONSTRUCTOR_INSTRUCTIONS = (
    (0x100006dac, "14270d10", "adr x20, 0x10002128c"),
    (0x100006dbc, "e00314aa", "mov x0, x20"),
    (0x100006dc0, "7d660094", "bl 0x1000207b4"),
    (0x100006e74, "140500f9", "str x20, [x8, #0x8]"),
    (0x100006e78, "e802148b", "add x8, x23, x20"),
    (0x100006e7c, "14050091", "add x20, x8, #0x1"),
    (0x100006e80, "e8440d50", "adr x8, 0x10002171e"),
    (0x100006e88, "9f0208eb", "cmp x20, x8"),
    (0x100006e8c, "83f9ff54", "b.lo 0x100006dbc"),
)


def require(predicate, message):
    if not predicate:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_bounded(path, limit):
    with path.open("rb") as source:
        data = source.read(limit + 1)
    require(len(data) <= limit, "Input exceeds byte cap: " + str(path))
    return data


def slice_checked(data, offset, size):
    require(0 <= offset <= len(data) and 0 <= size <= len(data) - offset,
            "Mach-O range exceeds authenticated file")
    return data[offset:offset + size]


def mach_o_symbols(data):
    magic, cpu, unused_subtype, unused_type, count, command_bytes, unused_flags, unused_reserved = \
        struct.unpack("<8I", slice_checked(data, 0, 32))
    require(magic == 0xfeedfacf and cpu == 0x0100000c and count <= 128,
            "Expected bounded little-endian arm64 Mach-O")
    command_end = 32 + command_bytes
    slice_checked(data, 32, command_bytes)
    segments = []
    symbol_tables = []
    offset = 32
    for unused in range(count):
        command, size = struct.unpack("<II", slice_checked(data, offset, 8))
        require(8 <= size <= command_end - offset, "Invalid Mach-O load command extent")
        if command == 0x19:  # LC_SEGMENT_64
            require(size >= 72, "Truncated segment command")
            segment = struct.unpack("<II16sQQQQiiII", slice_checked(data, offset, 72))
            segments.append((segment[3], segment[5], segment[6]))
        elif command == 2:  # LC_SYMTAB
            require(size == 24, "Unexpected symbol-table command size")
            symbol_tables.append(struct.unpack("<4I", slice_checked(data, offset + 8, 16)))
        offset += size
    require(offset == command_end and len(symbol_tables) == 1,
            "Mach-O command or symbol-table shape differs")

    symbol_offset, symbol_count, string_offset, string_bytes = symbol_tables[0]
    require(symbol_count <= 32768 and string_bytes <= MAX_SOURCE_BYTES,
            "Symbol observation cap exceeded")
    entries = slice_checked(data, symbol_offset, symbol_count * 16)
    strings = slice_checked(data, string_offset, string_bytes)
    found = {}
    for index in range(symbol_count):
        name_offset, kind, unused_section, unused_description, address = \
            struct.unpack_from("<IBBHQ", entries, index * 16)
        if kind & 0xe0:  # N_STAB debug entries can repeat linker symbol names.
            continue
        require(name_offset < len(strings), "Symbol name is outside string table")
        terminator = strings.find(b"\0", name_offset)
        require(terminator >= 0, "Unterminated symbol name")
        name = strings[name_offset:terminator].decode("ascii")
        if name in SYMBOLS.values():
            require(name not in found and kind & 0x0e == 0x0e,
                    "Expected a unique section-backed symbol")
            found[name] = address
    require(set(found) == set(SYMBOLS.values()), "Required official symbols are absent")

    def file_offset(address, size):
        matches = [base + address - virtual for virtual, base, length in segments
                   if virtual <= address and size <= length and address - virtual <= length - size]
        require(len(matches) == 1, "Address lacks a unique file-backed segment")
        slice_checked(data, matches[0], size)
        return matches[0]

    return found, file_offset


def observe(source, fixture):
    data = read_bounded(source, MAX_SOURCE_BYTES)
    symbols, file_offset = mach_o_symbols(data)
    boundary_rows = []
    boundaries = []
    for key, expected_symbol, expected_pointer in (
            ("begin", 0x1000244c8, 0x10002128c),
            ("end", 0x1000244d0, 0x10002171e)):
        address = symbols[SYMBOLS[key]]
        offset = file_offset(address, 8)
        raw = slice_checked(data, offset, 8)
        pointer, = struct.unpack("<Q", raw)
        require((address, pointer) == (expected_symbol, expected_pointer),
                "Named common-string boundary differs")
        boundaries.append(pointer)
        boundary_rows.append({"symbol": SYMBOLS[key], "address": hex(address),
                              "file_offset": offset, "pointer_hex": raw.hex(),
                              "pointer": hex(pointer)})
    begin, end = boundaries
    require(symbols[SYMBOLS["buffer"]] == begin and end - begin == TABLE_BYTES,
            "Declared common-string buffer differs")
    offset = file_offset(begin, TABLE_BYTES)
    table = slice_checked(data, offset, TABLE_BYTES)
    require(table.endswith(b"Hash128\0\0") and digest(table) == TABLE_SHA256,
            "Complete official common-string buffer hash differs")
    fixture_bytes = read_bounded(fixture, TABLE_BYTES)
    require(fixture_bytes == table, "Retained fixture differs from the complete declared buffer")

    require(symbols[SYMBOLS["constructor"]] == 0x100006c28, "Constructor symbol differs")
    instructions = []
    for address, expected_hex, assembly in CONSTRUCTOR_INSTRUCTIONS:
        instruction_offset = file_offset(address, 4)
        raw = slice_checked(data, instruction_offset, 4)
        require(raw.hex() == expected_hex, "Corroborated constructor instruction differs")
        instructions.append({"address": hex(address), "file_offset": instruction_offset,
                             "bytes_hex": expected_hex, "assembly": assembly})

    starts = [index for index in range(len(table)) if index == 0 or table[index - 1] == 0]
    receipt = {
        "scope": "Pinned exact35 converter buffer observation; no Unity execution or product parser imports",
        "source": {"path": source.name, "bytes": len(data), "sha256": digest(data)},
        "named_boundaries": boundary_rows,
        "buffer": {"symbol": SYMBOLS["buffer"], "begin": hex(begin), "end": hex(end),
                   "file_offset": offset, "bytes": len(table), "sha256": digest(table),
                   "string_starts_including_final_empty": len(starts), "final_empty_offset": starts[-1]},
        "constructor": {"symbol": SYMBOLS["constructor"], "address": "0x100006c28",
                        "instructions": instructions},
        "fixture": {"file": fixture.name, "bytes": len(fixture_bytes),
                    "sha256": digest(fixture_bytes), "complete_buffer_equal": True},
        "extractor": {"file": Path(__file__).name,
                      "sha256": digest(read_bounded(Path(__file__), 32768))},
        "limits": {"source_bytes": MAX_SOURCE_BYTES, "fixture_bytes": TABLE_BYTES,
                   "receipt_bytes": 16384, "scratch_bytes": 0},
        "binary_ninja": "Existing addressed observation was closed; this script opens no view or database",
    }
    return table, receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--fixture", type=Path, default=Path(__file__).with_name("exact35.bin"))
    parser.add_argument("--extract", type=Path, help="Write a new copy of the declared buffer")
    parser.add_argument("--receipt", type=Path, help="Write a new receipt instead of standard output")
    arguments = parser.parse_args()
    for path in (arguments.extract, arguments.receipt):
        require(path is None or not path.exists(), "Refusing to overwrite an output")
    require(arguments.extract is None or arguments.receipt is None or
            arguments.extract.resolve() != arguments.receipt.resolve(), "Output paths must differ")
    table, receipt = observe(arguments.source, arguments.fixture)
    text = json.dumps(receipt, indent=2) + "\n"
    require(len(text.encode()) <= 16384, "Receipt byte cap exceeded")
    if arguments.extract is not None:
        with arguments.extract.open("xb") as output:
            output.write(table)
    if arguments.receipt is not None:
        with arguments.receipt.open("x", encoding="utf-8") as output:
            output.write(text)
    else:
        print(text, end="")


if __name__ == "__main__":
    main()
