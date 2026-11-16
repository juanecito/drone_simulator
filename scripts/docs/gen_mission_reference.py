#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
"""Regenerates the field reference in docs/MISSIONS.md from assets/missions/mission.schema.json.

The text between the BEGIN/END GENERATED markers is replaced; everything else is kept.
Usage: scripts/docs/gen_mission_reference.py [--check]
  --check  exit with status 1 if docs/MISSIONS.md is out of date (for CI).
"""
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCHEMA = ROOT / "assets" / "missions" / "mission.schema.json"
DOC = ROOT / "docs" / "MISSIONS.md"
BEGIN = "<!-- BEGIN GENERATED: mission-reference (scripts/docs/gen_mission_reference.py) -->"
END = "<!-- END GENERATED: mission-reference -->"

# Sections in reading order: (title, $def name or None for the root object).
SECTIONS = [
    ("Mission (top level)", None),
    ("terrain — procedural", "terrainProcedural"),
    ("terrain — online", "terrainOnline"),
    ("terrain — file", "terrainFile"),
    ("terrain.params (procedural overrides)", "procParams"),
    ("origin", "origin"),
    ("extent", "extent"),
    ("File reference (elevationFiles[], orthophotoFiles[])", "fileRef"),
    ("environment", "environment"),
    ("environment.weather", "weather"),
    ("weather.timeline[]", "weatherChange"),
    ("wind", "wind"),
    ("Cloud layer (clouds[])", "cloudLayer"),
    ("precipitation", "precipitation"),
    ("vehicle", "vehicle"),
    ("spawn", "spawn"),
    ("rules", "rules"),
    ("items[]", "item"),
    ("utm", "utm"),
    ("Objective: reachAltitude", "objReachAltitude"),
    ("Objective: reachWaypoint", "objReachWaypoint"),
    ("Objective: hover", "objHover"),
    ("Objective: land", "objLand"),
    ("Objective: gateCourse", "objGateCourse"),
    ("Objective: deliver", "objDeliver"),
    ("Objective: inspect", "objInspect"),
    ("Objective: survey", "objSurvey"),
    ("Objective: returnHome", "objReturnHome"),
    ("Objective: park", "objPark"),
    ("failures[]", "failure"),
    ("scoring.stars[]", "star"),
]


def resolve(schema, node):
    while isinstance(node, dict) and "$ref" in node and len(node) <= 2:
        name = node["$ref"].split("/")[-1]
        extra = {k: v for k, v in node.items() if k != "$ref"}
        node = dict(schema["$defs"][name], **extra)
    return node


def type_text(schema, node):
    if isinstance(node, dict) and "$ref" in node:
        name = node["$ref"].split("/")[-1]
        target = schema["$defs"][name]
        if name in ("id",):
            return "id"
        if target.get("type") == "object":
            return f"object ({name})"
        return type_text(schema, dict(target, **{k: v for k, v in node.items() if k != "$ref"}))
    if "enum" in node:
        return " \\| ".join(json.dumps(v) for v in node["enum"])
    if "const" in node:
        return json.dumps(node["const"])
    t = node.get("type", "any")
    if isinstance(t, list):
        t = " \\| ".join(t)
    if t == "array":
        if "prefixItems" in node:
            return f"array[{len(node['prefixItems'])}]"
        items = node.get("items")
        if isinstance(items, dict):
            return f"array of {type_text(schema, items)}"
    return t


def limits(schema, node):
    node = resolve(schema, node)
    parts = []
    for key, sym in (("minimum", "≥"), ("exclusiveMinimum", ">"), ("maximum", "≤"),
                     ("exclusiveMaximum", "<")):
        if key in node:
            parts.append(f"{sym} {node[key]}")
    for key, word in (("minItems", "min items"), ("maxItems", "max items"),
                      ("minLength", "min length"), ("maxLength", "max length")):
        if key in node:
            parts.append(f"{word} {node[key]}")
    if "pattern" in node and node.get("$comment") is None:
        parts.append(f"pattern `{node['pattern']}`")
    return ", ".join(parts)


def table(schema, node):
    required = set(node.get("required", []))
    rows = ["| Field | Type | Required | Limits | Description |",
            "|-------|------|----------|--------|-------------|"]
    for name, prop in node.get("properties", {}).items():
        if name in ("$schema",) or prop == {}:
            continue
        desc = resolve(schema, prop).get("description", "") if isinstance(prop, dict) else ""
        rows.append(f"| `{name}` | {type_text(schema, prop)} | {'yes' if name in required else ''}"
                    f" | {limits(schema, prop)} | {desc} |")
    return "\n".join(rows)


def generate():
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    out = [BEGIN, "", "*Generated from `assets/missions/mission.schema.json` — do not edit by hand.*",
           ""]
    for title, name in SECTIONS:
        node = schema if name is None else schema["$defs"][name]
        out.append(f"#### {title}")
        out.append("")
        if node.get("description"):
            out.append(node["description"])
            out.append("")
        out.append(table(schema, node))
        out.append("")
    out.append(END)
    return "\n".join(out)


def main():
    text = DOC.read_text(encoding="utf-8")
    start, end = text.index(BEGIN), text.index(END) + len(END)
    updated = text[:start] + generate() + text[end:]
    if "--check" in sys.argv:
        if updated != text:
            print("docs/MISSIONS.md is out of date; run scripts/docs/gen_mission_reference.py")
            return 1
        return 0
    DOC.write_text(updated, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
