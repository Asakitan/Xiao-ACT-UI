#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Generate a complete proto3 schema from SRDC's generated protobuf JS files."""

from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PROTO_DIR = Path(__file__).resolve().parent
PB_JS = ROOT / "StarResonanceDamageCounter" / "algo" / "BlueProtobuf_pb.js"
ROOT_JS = ROOT / "StarResonanceDamageCounter" / "algo" / "blueprotobuf.js"
SCHEMA_JSON = PROTO_DIR / "srdc_blueprotobuf_schema.json"
OUT_PROTO = PROTO_DIR / "star_resonance.proto"

ENUM_RE = re.compile(r"proto\.(?P<name>[A-Za-z0-9_]+)\s*=\s*\{(?P<body>.*?)\n\};", re.S)
ENUM_ITEM_RE = re.compile(r"(?P<name>[A-Za-z0-9_]+)\s*:\s*(?P<value>-?\d+)")
ROOT_MESSAGE_RE = re.compile(r"\$root\.(?P<name>[A-Za-z0-9_]+)\s*=\s*\(function\(\)")
ROOT_ENUM_RE = re.compile(
    r"\$root\.(?P<name>[A-Za-z0-9_]+)\s*=\s*\(function\(\)\s*\{\s*var valuesById = \{\}, values = Object\.create\(valuesById\);",
    re.S,
)

SCALAR_TYPES = {
    "double",
    "float",
    "int32",
    "int64",
    "uint32",
    "uint64",
    "sint32",
    "sint64",
    "fixed32",
    "fixed64",
    "sfixed32",
    "sfixed64",
    "bool",
    "string",
    "bytes",
}


def load_schema() -> dict:
    payload = json.loads(SCHEMA_JSON.read_text(encoding="utf-8"))
    deduped_messages: dict[str, list[dict]] = {}
    for message_name, fields in payload["messages"].items():
        seen: set[tuple[int, str, str, str]] = set()
        clean_fields: list[dict] = []
        for field in fields:
            key = (field["number"], field["name"], field["type"], field["label"])
            if key in seen:
                continue
            seen.add(key)
            clean_fields.append(field)
        deduped_messages[message_name] = clean_fields
    payload["messages"] = deduped_messages
    return payload


def parse_enums(pb_text: str) -> dict[str, list[tuple[str, int]]]:
    enums: dict[str, list[tuple[str, int]]] = {}
    for match in ENUM_RE.finditer(pb_text):
        values = [
            (item.group("name"), int(item.group("value")))
            for item in ENUM_ITEM_RE.finditer(match.group("body"))
        ]
        if values:
            enums[match.group("name")] = values
    return enums


def parse_root_symbols(root_text: str) -> tuple[set[str], set[str]]:
    enums = set(ROOT_ENUM_RE.findall(root_text))
    messages = set(ROOT_MESSAGE_RE.findall(root_text)) - enums
    return messages, enums


def normalize_type_name(type_name: str, known_messages: set[str], known_enums: set[str]) -> str:
    if type_name.startswith("map<"):
        return type_name
    if type_name in SCALAR_TYPES:
        return type_name
    if type_name in known_messages or type_name in known_enums:
        return type_name
    return type_name


def format_field(field: dict, known_messages: set[str], known_enums: set[str]) -> str:
    type_name = normalize_type_name(field["type"], known_messages, known_enums)
    label = field["label"]
    if type_name.startswith("map<"):
        field_type = type_name
    elif label == "repeated":
        field_type = f"repeated {type_name}"
    else:
        field_type = type_name
    return f"    {field_type} {field['name']} = {field['number']};"


def build_proto(
    schema_messages: dict[str, list[dict]],
    enums: dict[str, list[tuple[str, int]]],
    verified_messages: set[str],
    verified_enums: set[str],
) -> str:
    lines = [
        '// Generated from SRDC "BlueProtobuf_pb.js" and "blueprotobuf.js".',
        "// DO NOT EDIT BY HAND. Re-run generate_star_resonance_proto.py.",
        "",
        'syntax = "proto3";',
        "",
        "package star;",
        "",
    ]

    for enum_name in sorted(enums):
        lines.append(f"enum {enum_name} {{")
        for value_name, value_number in enums[enum_name]:
            lines.append(f"    {value_name} = {value_number};")
        lines.append("}")
        lines.append("")

    known_messages = set(schema_messages) | verified_messages
    known_enums = set(enums) | verified_enums

    all_messages = {name: list(fields) for name, fields in schema_messages.items()}
    for message_name in verified_messages:
        all_messages.setdefault(message_name, [])

    for message_name in sorted(all_messages):
        lines.append(f"message {message_name} {{")
        for field in all_messages[message_name]:
            lines.append(format_field(field, known_messages, known_enums))
        lines.append("}")
        lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def ensure_consistency(
    schema_messages: dict[str, list[dict]],
    enums: dict[str, list[tuple[str, int]]],
    root_messages: set[str],
    root_enums: set[str],
) -> None:
    schema_message_names = set(schema_messages)
    missing_from_root = sorted(schema_message_names - root_messages)
    missing_enums_from_root = sorted(set(enums) - root_enums)
    missing_enums_from_pb = sorted(root_enums - set(enums))

    if missing_from_root or missing_enums_from_root or missing_enums_from_pb:
        chunks = []
        if missing_from_root:
            chunks.append(f"schema->root missing messages: {missing_from_root[:10]}")
        if missing_enums_from_root:
            chunks.append(f"pb->root missing enums: {missing_enums_from_root[:10]}")
        if missing_enums_from_pb:
            chunks.append(f"root->pb missing enums: {missing_enums_from_pb[:10]}")
        raise SystemExit("Consistency check failed: " + "; ".join(chunks))


def main() -> int:
    for path in (PB_JS, ROOT_JS, SCHEMA_JSON):
        if not path.exists():
            raise SystemExit(f"Required source file not found: {path}")

    schema = load_schema()
    schema_messages = schema["messages"]
    pb_text = PB_JS.read_text(encoding="utf-8")
    root_text = ROOT_JS.read_text(encoding="utf-8")
    enums = parse_enums(pb_text)
    root_messages, root_enums = parse_root_symbols(root_text)

    ensure_consistency(schema_messages, enums, root_messages, root_enums)

    proto_text = build_proto(schema_messages, enums, root_messages, root_enums)
    OUT_PROTO.write_text(proto_text, encoding="utf-8")
    total_messages = len(set(schema_messages) | root_messages)
    print(
        f"Wrote {OUT_PROTO} "
        f"({len(enums)} enums, {total_messages} messages)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
