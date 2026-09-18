#!/usr/bin/env python3
"""Run the pinned official converter only on the untouched generated fixtures."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess
import time


CONVERTER = Path("/Applications/Unity/Unity.app/Contents/Tools/binary2text")


def limits():
    resource.setrlimit(resource.RLIMIT_CPU, (3, 3))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (262144, 262144))


def pin(path):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    return {"bytes": path.stat().st_size, "sha256": digest}


def compare_values(text, observations):
    blocks = re.split(r"\nID: (\d+) \(ClassID: 114\)", text)
    expected = {value["path_id"]: value for value in observations["variants"][0]["values"]}
    if len(blocks) != 1 + 2 * len(expected):
        raise RuntimeError("Converter object count differs from raw observations")
    for index in range(1, len(blocks), 2):
        path_id = int(blocks[index])
        block = blocks[index + 1]
        value = expected.pop(path_id)
        registry = value["registry"]
        if block.count("(ManagedReferencesRegistry)") != 1:
            raise RuntimeError("Converter did not retain one host registry")
        if 'caseTag "' + value["case"] + '" (string)' not in block:
            raise RuntimeError("Converter host name differs")
        ids = [int(number) for number in re.findall(r"\brid (-?\d+) \(SInt64\)", block)]
        expected_ids = [value["first_id"], value["second_id"]] + value["item_ids"] + [
            entry["next_id"] for entry in registry if entry["selection"] != "null"]
        if ids != expected_ids:
            raise RuntimeError("Converter reference-field sequence differs")
        rows = re.findall(r"\brid (-?\d+) \(class: ([^,]*), ns: ([^,]*), asm: ([^)]*)\)", block)
        expected_rows = [(str(entry["rid"]), *entry["identity"]) for entry in registry]
        if rows != expected_rows:
            raise RuntimeError("Converter registry identities differ")
        markers = [int(number) for number in re.findall(r"\bmarker (-?\d+) \(int\)", block)]
        labels = re.findall(r'\blabel "([^"]*)" \(string\)', block)
        amounts = [float(number) for number in re.findall(r"\bamount ([^ ]+) \(float\)", block)]
        if markers != [entry["marker"] for entry in registry if "marker" in entry] or \
                labels != [entry["label"] for entry in registry if "label" in entry] or \
                amounts != [entry["amount"] for entry in registry if "amount" in entry]:
            raise RuntimeError("Converter controlled payload values differ")
    if expected:
        raise RuntimeError("Converter omitted controlled hosts")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--generation", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--observations", required=True, type=Path)
    parser.add_argument("--converter", type=Path, default=CONVERTER)
    arguments = parser.parse_args()
    converter = arguments.converter.resolve()
    arguments.output.mkdir()
    observations = json.loads(arguments.observations.read_text())
    report = {"scope": "Untouched exact35 fixture outputs only; no byte mutations",
              "converter": {"path": converter.name, **pin(converter)},
              "limits": {"cpu_seconds": 3, "wall_seconds": 5, "core_bytes": 0,
                         "per_file_output_bytes": 262144}, "cases": []}
    for variant in ("with-tree", "without-tree"):
        source = arguments.generation / variant / "managed-reference-fixture.assets"
        if source.stat().st_size > 1024 * 1024:
            raise RuntimeError("Input exceeds fixture cap")
        destination = arguments.output / (variant + ".txt")
        command = [str(converter), str(source.resolve()), str(destination.resolve()), "-detailed"]
        started = time.monotonic()
        result = subprocess.run(command, capture_output=True, timeout=5, preexec_fn=limits)
        entry = {"variant": variant, "input": pin(source), "command": [converter.name, variant + "/managed-reference-fixture.assets", destination.name, "-detailed"],
                 "exit_code": result.returncode, "elapsed_seconds": round(time.monotonic() - started, 6),
                 "stdout": result.stdout.decode("utf-8", errors="replace"),
                 "stderr": result.stderr.decode("utf-8", errors="replace"), "output": pin(destination)}
        report["cases"].append(entry)
        if result.returncode != 0 or result.stdout or result.stderr:
            raise RuntimeError("Official converter reported a failure or unexpected diagnostic")
        if variant == "with-tree":
            compare_values(destination.read_text(), observations)
            entry["all_host_reference_and_payload_values_match"] = True
    (arguments.output / "receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
