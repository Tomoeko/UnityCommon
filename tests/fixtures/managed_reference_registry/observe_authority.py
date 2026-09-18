#!/usr/bin/env python3
"""Retain compact addressed observations from the pinned official converter."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import shutil


CONVERTER = Path("/Applications/Unity/Unity.app/Contents/Tools/binary2text")
OBJDUMP = shutil.which("llvm-objdump") or "llvm-objdump"
SELECTIONS = [
    ("registry-count", "__ZN15TypeTreeQueries35ReadReferenceRegistrySizeFromBufferER16TypeTreeIteratoriPKhPib",
     [(0x100011b04, 0x100011b28), (0x100011c14, 0x100011ca0)]),
    ("reference-id", "__ZN20SimpleRefManagedTypeC2ERK16TypeTreeIteratoriPKhPib",
     [(0x100012cc8, 0x100012d34), (0x100012dbc, 0x100012dd8)]),
    ("type-selection", "__ZeqRKN14SerializedFile14SerializedTypeERK20SimpleRefManagedType",
     [(0x10000bfbc, 0x10000c16c)]),
    ("registry-dispatch", "__Z15RecursiveOutputRK16TypeTreeIteratorRK13dynamic_arrayIN14SerializedFile14SerializedTypeELm0EEPKhPiiRNSt3__113basic_ostreamIcNSB_11char_traitsIcEEEE9DumpFlagsibi",
     [(0x100009e04, 0x100009e20), (0x10000abe0, 0x10000ac88),
      (0x10000a4d4, 0x10000a518), (0x10000ae14, 0x10000ae98),
      (0x10000b020, 0x10000b048), (0x10000b1a0, 0x10000b2f0)]),
]


def file_pin(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"path": path.name, "bytes": path.stat().st_size, "sha256": digest}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--converter", type=Path, default=CONVERTER)
    parser.add_argument("--objdump", type=Path, default=Path(OBJDUMP))
    arguments = parser.parse_args()
    converter = file_pin(arguments.converter)
    arguments.output.mkdir()
    report = {"scope": "Read-only symbol disassembly; addresses are preferred arm64 Mach-O VAs",
              "converter": converter, "objdump": file_pin(arguments.objdump), "selections": []}
    for name, symbol, ranges in SELECTIONS:
        command = [str(arguments.objdump), "--disassemble-symbols=" + symbol,
                   "--no-show-raw-insn", str(arguments.converter)]
        result = subprocess.run(command, capture_output=True, timeout=10, check=True)
        if result.stderr or len(result.stdout) > 2 * 1024 * 1024:
            raise RuntimeError("Unexpected disassembler diagnostic or observation extent")
        text = result.stdout.decode("utf-8")
        retained = ["symbol=" + symbol]
        for start, end in ranges:
            retained.append("range=0x%x..0x%x (exclusive end)" % (start, end))
            count = 0
            for line in text.splitlines():
                match = re.match(r"([0-9a-f]+):", line)
                if match and start <= int(match[1], 16) < end:
                    retained.append(line)
                    count += 1
            if count != (end - start) // 4:
                raise RuntimeError("Addressed instruction extent is incomplete")
        path = arguments.output / (name + ".asm.txt")
        path.write_text("\n".join(retained) + "\n")
        report["selections"].append({"name": name, "command": [arguments.objdump.name, "--disassemble-symbols=" + symbol,
                                                              "--no-show-raw-insn", arguments.converter.name],
                                     "full_symbol_disassembly_bytes": len(result.stdout),
                                     "full_symbol_disassembly_sha256": hashlib.sha256(result.stdout).hexdigest(),
                                     "retained": {"file": path.name, "bytes": path.stat().st_size,
                                                  "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}})
    (arguments.output / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"selected_symbols": len(SELECTIONS),
                      "retained_bytes": sum(path.stat().st_size for path in arguments.output.iterdir())}))


if __name__ == "__main__":
    main()
