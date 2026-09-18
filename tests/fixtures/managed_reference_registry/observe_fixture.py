#!/usr/bin/env python3
"""Independently observe this controlled writer matrix; no product parser imports.

The observer accepts only the small exact35 LE v22 fixture. Payload fields are
read from the controlled C# field order, separately from the retained TypeTrees.
It is an experiment, not a general SerializedFile reader or product rule.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "common_string_table"))
from extract_verify import observe


CONVERTER = Path("/Applications/Unity/Unity.app/Contents/Tools/binary2text")
MAX_FILE_BYTES = 1024 * 1024


def require(predicate, message):
    if not predicate:
        raise ValueError(message)


class Reader:
    def __init__(self, data, start, end):
        self.data = data
        self.position = start
        self.end = end
        self.fields = []

    def raw(self, size):
        require(0 <= size <= self.end - self.position, "Range exceeds reader bound")
        start = self.position
        self.position += size
        return self.data[start:self.position]

    def scalar(self, label, format):
        start = self.position
        raw = self.raw(struct.calcsize(format))
        value = struct.unpack("<" + format, raw)[0]
        self.fields.append({"field": label, "offset": start, "size": len(raw),
                            "value": value, "hex": raw.hex()})
        return value

    def align(self, label):
        start = self.position
        raw = self.raw((-start) & 3)
        self.fields.append({"field": label, "offset": start, "size": len(raw),
                            "hex": raw.hex()})

    def terminated(self, label):
        start = self.position
        terminator = self.data.index(0, start, min(self.end, start + 4096))
        raw = self.raw(terminator + 1 - start)
        value = raw[:-1].decode("utf-8")
        self.fields.append({"field": label, "offset": start, "size": len(raw),
                            "value": value, "hex": raw.hex()})
        return value

    def string(self, label):
        size = self.scalar(label + ".length", "I")
        require(size <= 4096, "String observation cap exceeded")
        start = self.position
        raw = self.raw(size)
        value = raw.decode("utf-8")
        self.fields.append({"field": label, "offset": start, "size": size,
                            "value": value, "hex": raw.hex()})
        self.align(label + ".padding")
        return value


def read_type(reader, ordinal, has_tree, reference, common):
    start = reader.position
    class_id = reader.scalar("class_id", "i")
    stripped = reader.scalar("stripped", "B")
    script_index = reader.scalar("script_index", "h")
    require(not (class_id == -1 and script_index < 0), "Unsupported reference prefix")
    script_hash = reader.raw(16).hex() if class_id == 114 or script_index >= 0 else None
    type_hash = reader.raw(16).hex()
    result = {"ordinal": ordinal, "offset": start, "class_id": class_id,
              "stripped": stripped, "script_index": script_index,
              "script_hash": script_hash, "type_hash": type_hash, "nodes": []}
    if has_tree:
        count = reader.scalar("node_count", "I")
        strings_size = reader.scalar("string_byte_count", "I")
        require(0 < count <= 512 and strings_size < 16384, "TypeTree observation cap exceeded")
        node_start = reader.position
        node_bytes = reader.raw(32 * count)
        string_start = reader.position
        strings = reader.raw(strings_size)

        def name(offset):
            table = common if offset & 0x80000000 else strings
            offset &= 0x7fffffff
            require(offset < len(table) and (offset == 0 or table[offset - 1] == 0),
                    "TypeTree string does not start at a string boundary")
            return table[offset:table.index(0, offset)].decode("utf-8")

        for index in range(count):
            version, level, flags, type_offset, name_offset, size, stored_index, meta, tail = \
                struct.unpack_from("<HBBIIiIIQ", node_bytes, index * 32)
            result["nodes"].append({"ordinal": index, "offset": node_start + index * 32,
                                    "version": version, "level": level, "type_flags": flags,
                                    "byte_size": size, "index": stored_index, "meta_flags": meta,
                                    "tail8_hex": tail.to_bytes(8, "little").hex(),
                                    "type": name(type_offset), "name": name(name_offset)})
        result["strings_offset"] = string_start
        result["strings_hex"] = strings.hex()
        if reference:
            result["identity"] = [reader.terminated("class"), reader.terminated("namespace"),
                                  reader.terminated("assembly")]
        else:
            dependencies = reader.scalar("dependency_count", "I")
            require(dependencies <= 64, "Dependency observation cap exceeded")
            result["dependencies"] = [reader.scalar("dependency", "i")
                                      for unused in range(dependencies)]
    result["size"] = reader.position - start
    return result


def metadata(data, common):
    require(48 <= len(data) <= MAX_FILE_BYTES and data[8:12] == b"\0\0\0\x16",
            "Expected small v22 file")
    metadata_size, declared, origin = struct.unpack_from(">QQQ", data, 16)
    require(declared == len(data) and 48 + metadata_size <= origin <= declared,
            "Physical header extents differ")
    require(data[40] == 0, "Observer only admits the written LE fixture")
    reader = Reader(data, 48, 48 + metadata_size)
    version = reader.terminated("engine_version")
    require(version == "2021.3.35f1", "Exact Editor version differs")
    target = reader.scalar("target", "I")
    flag = reader.scalar("tree_flag", "B")
    require(target == 19 and flag in (0, 1), "Fixture target/tree selection differs")
    type_count = reader.scalar("type_count", "I")
    require(type_count <= 16, "Ordinary type observation cap exceeded")
    ordinary = [read_type(reader, index, flag, False, common) for index in range(type_count)]
    count = reader.scalar("object_count", "I")
    require(count <= 32, "Object observation cap exceeded")
    objects = []
    for index in range(count):
        reader.align("object_alignment")
        source = reader.position
        path_id = reader.scalar("path_id", "q")
        relative = reader.scalar("relative_offset", "Q")
        size = reader.scalar("object_size", "I")
        type_ordinal = reader.scalar("type_ordinal", "I")
        require(type_ordinal < type_count and origin + relative + size <= declared,
                "Object range or type selection differs")
        objects.append({"ordinal": index, "source_offset": source, "path_id": path_id,
                        "offset": origin + relative, "size": size, "type_ordinal": type_ordinal})
    scripts = reader.scalar("script_count", "I")
    require(scripts <= 16, "Script observation cap exceeded")
    for unused in range(scripts):
        reader.scalar("script_file", "i")
        reader.align("script_alignment")
        reader.scalar("script_path", "q")
    externals = reader.scalar("external_count", "I")
    require(externals <= 16, "External observation cap exceeded")
    for unused in range(externals):
        reader.terminated("external_leading")
        reader.raw(16)
        reader.scalar("external_type", "i")
        reader.terminated("external_path")
    references = reader.scalar("reference_type_count", "I")
    require(references <= 16, "Reference type observation cap exceeded")
    reference_types = [read_type(reader, index, flag, True, common)
                       for index in range(references)]
    reader.terminated("user_information")
    require(reader.position == reader.end, "Metadata did not exhaust exactly")
    return {"metadata_size": metadata_size, "data_origin": origin, "tree_flag": flag,
            "ordinary_types": ordinary, "reference_types": reference_types,
            "objects": objects, "metadata_fields": reader.fields}


def object_values(data, row, reference_types):
    reader = Reader(data, row["offset"], row["offset"] + row["size"])
    for name in ("game_object",):
        reader.scalar(name + ".file_id", "i")
        reader.scalar(name + ".path_id", "q")
    reader.scalar("enabled", "B")
    reader.align("enabled.padding")
    reader.scalar("script.file_id", "i")
    reader.scalar("script.path_id", "q")
    reader.string("name")
    case = reader.string("case_tag")
    first = reader.scalar("first.rid", "q")
    second = reader.scalar("second.rid", "q")
    item_count = reader.scalar("items.count", "I")
    require(item_count <= 16, "Managed array observation cap exceeded")
    items = [reader.scalar("items.rid", "q") for unused in range(item_count)]
    reader.align("items.padding")
    registry_start = reader.position
    version = reader.scalar("registry.version", "i")
    require(version == 2, "Only written registry version2 is observed")
    count = reader.scalar("registry.count", "I")
    require(count <= 16, "Registry row observation cap exceeded")
    registry = []
    for ordinal in range(count):
        source = reader.position
        rid = reader.scalar("registry.rid", "q")
        identity = [reader.string("registry.class"), reader.string("registry.namespace"),
                    reader.string("registry.assembly")]
        payload_start = reader.position
        matches = [entry for entry in reference_types if entry.get("identity") == identity]
        record = {"ordinal": ordinal, "offset": source, "rid": rid, "identity": identity,
                  "payload_offset": payload_start}
        if rid == -2:
            require(identity == ["", "", ""], "Null row identity differs")
            record["selection"] = "null"
        else:
            require(len(matches) == 1, "Reference type identity is unavailable or ambiguous")
            require(identity[0] == "Payload" and identity[2] == "Assembly-CSharp",
                    "Outside controlled payload identity")
            record["selection"] = "unique_reference_row"
            record["reference_type_ordinal"] = matches[0]["ordinal"]
            record["marker"] = reader.scalar("payload.marker", "i")
            if identity[1] == "UnityRecoverManagedFixture.Alpha":
                record["label"] = reader.string("payload.label")
            elif identity[1] == "UnityRecoverManagedFixture.Beta":
                record["amount"] = reader.scalar("payload.amount", "f")
            else:
                raise ValueError("Outside controlled payload namespace")
            record["next_id"] = reader.scalar("payload.next.rid", "q")
        record["payload_size"] = reader.position - payload_start
        record["size"] = reader.position - source
        registry.append(record)
    reader.align("registry.padding")
    require(reader.position == reader.end, "Controlled object did not exhaust exactly")
    return {"path_id": row["path_id"], "offset": row["offset"], "size": row["size"],
            "case": case, "first_id": first, "second_id": second, "item_ids": items,
            "registry_offset": registry_start, "registry_size": reader.end - registry_start,
            "registry_version": version, "registry": registry, "fields": reader.fields}


def compare_report(observed, report, variant_index):
    hosts = {host["pathId"]: host for host in report["hosts"]}
    written = {row["pathId"]: row for row in report["variants"][variant_index]["objects"]}
    require(len(hosts) == len(written) == len(observed), "Host object count differs")
    for value in observed:
        host = hosts[value["path_id"]]
        row = written[value["path_id"]]
        require((value["offset"], value["size"]) == (row["offset"], row["size"]),
                "Independent object range differs from WriteResult")
        require((value["case"], value["first_id"], value["second_id"], value["item_ids"]) ==
                (host["caseName"], host["firstId"], host["secondId"], host["itemIds"]),
                "Independent host values differ from official managed API")
        require(host["idsResolveToOriginalInstances"], "Official API did not preserve aliases")
        require((value["first_id"] == value["second_id"]) == host["firstAndSecondSame"],
                "Repeated root field identities differ")
        require(host["unknownUntrackedObjectId"] == report["unknownId"] == -1,
                "Official unknown untracked-object ID differs")
        api_records = {entry["id"]: entry for entry in host["references"]}
        require(len(api_records) == len(host["references"]), "Official API returned duplicate IDs")
        require(len(api_records) == len(value["registry"]), "Registry/API row counts differ")
        for entry in value["registry"]:
            api = api_records[entry["rid"]]
            require(entry["identity"] == [api["className"], api["namespaceName"], api["assemblyName"]],
                    "Independent type identity differs from official managed API")
            if api["isNull"]:
                require(entry["selection"] == "null" and entry["payload_size"] == 0,
                        "Null registry framing differs")
                continue
            require(entry["marker"] == api["marker"] and entry["next_id"] == api["nextId"] and
                    api["nextResolvesToSameInstance"], "Payload/alias observation differs")
            if "label" in entry:
                require(entry["label"] == api["label"], "String payload differs")
            if "amount" in entry:
                require(entry["amount"] == api["amount"], "Float payload differs")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--converter", type=Path, default=CONVERTER)
    parser.add_argument("--generation", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    arguments = parser.parse_args()
    require(not arguments.output.exists(), "Refusing to overwrite an observation")
    fixture = Path(__file__).resolve().parents[1] / "common_string_table/exact35.bin"
    common, authority = observe(arguments.converter, fixture)
    report = json.loads((arguments.generation / "writer-report.json").read_text())
    require(report["status"] == "success", "Official writer did not succeed")
    require(report["observationsAfterAssetReload"], "Host API observations did not follow asset reload")
    results = []
    for index, variant in enumerate(("with-tree",)):
        path = arguments.generation / variant / "managed-reference-fixture.assets"
        require(path.stat().st_size <= MAX_FILE_BYTES, "File observation cap exceeded")
        data = path.read_bytes()
        result = metadata(data, common)
        values = [object_values(data, row, result["reference_types"])
                  for row in result["objects"]]
        compare_report(values, report, index)
        result.update({"variant": variant, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                       "values": values, "official_api_and_write_result_match": True})
        results.append(result)
    path = arguments.generation / "without-tree/managed-reference-fixture.assets"
    require(path.stat().st_size <= MAX_FILE_BYTES, "File observation cap exceeded")
    without_bytes = path.read_bytes()
    without = metadata(without_bytes, common)
    with_bytes = (arguments.generation / "with-tree/managed-reference-fixture.assets").read_bytes()
    with_rows = {row["path_id"]: row for row in results[0]["objects"]}
    writer_rows = {row["pathId"]: row for row in report["variants"][1]["objects"]}
    equality = []
    for row in without["objects"]:
        other = with_rows[row["path_id"]]
        writer_row = writer_rows[row["path_id"]]
        require((row["offset"], row["size"]) == (writer_row["offset"], writer_row["size"]),
                "Tree-free object location differs from official WriteResult")
        raw = without_bytes[row["offset"]:row["offset"] + row["size"]]
        other_raw = with_bytes[other["offset"]:other["offset"] + other["size"]]
        require(raw == other_raw, "Tree flags changed controlled object payload bytes")
        equality.append({"path_id": row["path_id"], "bytes": len(raw),
                         "sha256": hashlib.sha256(raw).hexdigest(), "byte_equal": True})
    without.update({"variant": "without-tree", "bytes": len(without_bytes),
                    "sha256": hashlib.sha256(without_bytes).hexdigest(),
                    "payload_comparisons_to_tree_bearing_file": equality,
                    "schema_scope": "Reference trees/names are absent; payload equality does not resolve schemas"})
    results.append(without)
    output = {"scope": "Controlled exact35 LE registry-v2 writer outputs only; no malformed Unity execution",
              "common_string_authority": authority, "variants": results}
    arguments.output.write_text(json.dumps(output, indent=2) + "\n")
    print(json.dumps({"objects": len(results[0]["values"]),
                      "reference_types": len(results[0]["reference_types"]),
                      "matches": True, "observation_bytes": arguments.output.stat().st_size}))


if __name__ == "__main__":
    main()
