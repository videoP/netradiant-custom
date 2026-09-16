#!/usr/bin/env python3
"""Extracts the jaPRO river solver into river_solver.inc, verbatim.

The -river stage runs the same shallow water solver the engine does, and the
two have to agree: the bake carries a settings hash and a bed hash that the
engine recomputes and checks before it will load the file.  Retyping 1300 lines
of physics would guarantee they drift, so the port is a mechanical extraction
instead - re-run this against a newer jaPRO and diff the result.

Usage:  python river_extract.py [path-to-jaPRO]
"""

import os
import re
import sys

JAPRO = sys.argv[1] if len(sys.argv) > 1 else r"D:/Code/jaPRO"
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(JAPRO, "codemp", "game", "bg_sailing.c")
IO = os.path.join(JAPRO, "codemp", "game", "bg_sailing_river_io.inc")
OUT = os.path.join(HERE, "river_solver.inc")

# Everything the solve needs, in dependency order.  Capture and zone
# rasterisation are deliberately absent: riverbed.cpp already does those from
# real brush geometry, which is better than anything the engine can do.
FUNCTIONS = [
    "BG_SailingRiverFieldGridSize",
    "BG_SailingRiverGridOrigin",
    "BG_SailingRiverFieldIndex",
    "BG_SailingRiverFieldCellCoords",
    "BG_SailingRiverFieldCellCenter",
    "BG_SailingRiverCellOverlapsBounds",
    "BG_SailingRiverCellInBrush",
    "BG_SailingRiverFieldMarkBoundaries",
    "BG_SailingRiverCellIsSource",
    "BG_SailingRiverCellIsSink",
    "BG_SailingRiverFieldInitializeWater",
    "BG_SailingRiverFieldTimeStep",
    "BG_SailingRiverSourceBoundaryCell",
    "BG_SailingRiverInflowDepth",
    "BG_SailingRiverApplyDischargeBoundary",
    "BG_SailingRiverApplyComputedStageSink",
    "BG_SailingRiverFieldApplyBoundariesOverride",
    "BG_SailingRiverFieldApplyBoundaries",
    "BG_SailingRiverHydrostaticFlux",
    "BG_SailingRiverGatherFace",
    "BG_SailingRiverStepSlice",
    "BG_SailingRiverFieldInvalidateActive",
    "BG_SailingRiverFieldRefreshActive",
    "BG_SailingRiverWorkAdd",
    "BG_SailingRiverPrepareWork",
    "BG_SailingRiverFieldStep",
    "BG_SailingRiverInflowDischarge",
    "BG_SailingRiverFieldConverged",
    "BG_SailingRiverFieldInit",
    "BG_SailingRiverFieldSample",
]

IO_FUNCTIONS = [
    "BG_RiverHashBytes",
    "BG_SailingRiverBedHash",
    "BG_SailingRiverSettingsHash",
]

# Defines that live in the .c rather than the header.
LOCAL_DEFINES = ["SAILING_RIVER_CELL_WORK"]


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


def extract_function(text, name):
    """Grabs a whole function plus the comment block above it, verbatim."""
    pattern = re.compile(
        r"^(?:static\s+)?(?:const\s+)?[A-Za-z_][A-Za-z0-9_ \t\*]*?\b"
        + re.escape(name)
        + r"\s*\(",
        re.M,
    )
    # jaPRO forward-declares several of these above their definitions, and a
    # declaration matches the same pattern.  Take the one whose argument list
    # is followed by a body rather than a semicolon.
    match = None
    for candidate in pattern.finditer(text):
        depth = 0
        i = candidate.end() - 1
        while i < len(text):
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        rest = text[i + 1:]
        if rest.lstrip().startswith("{"):
            match = candidate
            break
    if match is None:
        raise SystemExit("could not find a definition of %s" % name)

    start = match.start()
    # take any immediately preceding block comment with it, since those carry
    # the reasoning that makes the physics reviewable
    head = text.rfind("*/", 0, start)
    if head != -1 and text[head + 2 : start].strip() == "":
        opener = text.rfind("/*", 0, head)
        if opener != -1:
            line_start = text.rfind("\n", 0, opener) + 1
            if text[line_start:opener].strip() == "":
                start = line_start

    # walk to the closing brace at column zero
    body = text.index("{", match.end() - 1)
    depth = 0
    i = body
    while i < len(text):
        char = text[i]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
        elif char == '"' or char == "'":
            quote = char
            i += 1
            while i < len(text) and text[i] != quote:
                if text[i] == "\\":
                    i += 1
                i += 1
        elif text.startswith("/*", i):
            i = text.index("*/", i) + 1
        elif text.startswith("//", i):
            i = text.index("\n", i)
        i += 1
    raise SystemExit("unterminated %s" % name)


def prototype(chunk):
    """The signature of an extracted function, for a forward declaration.

    The .inc is compiled as C++, where every one of these must be declared
    before its first use.  In the engine they are C statics in one translation
    unit, so the order they happen to appear in is not a dependency order.
    """
    body = chunk.index("{")
    head = chunk[:body]
    # drop any leading comment block, and any forward declarations jaPRO
    # already has sitting immediately above the definition
    comment = head.rfind("*/")
    semicolon = head.rfind(";")
    cut = max(comment + 2 if comment != -1 else 0,
              semicolon + 1 if semicolon != -1 else 0)
    head = head[cut:]
    return " ".join(head.split()) + ";"


def extract_define(text, name):
    match = re.search(r"^#define\s+" + re.escape(name) + r"\b.*$", text, re.M)
    if match is None:
        raise SystemExit("could not find #define %s" % name)
    return match.group(0)


def main():
    source = read(SRC)
    io_source = read(IO)

    chunks = [
        "/* Generated by river_extract.py - do not edit.",
        " *",
        " * The jaPRO river solver, lifted verbatim from codemp/game/bg_sailing.c",
        " * and bg_sailing_river_io.inc.  The engine recomputes this file's bed and",
        " * settings hashes and refuses a bake whose numbers disagree, so the two",
        " * copies have to stay identical; re-run the script and diff rather than",
        " * editing anything here.",
        " */",
        "",
    ]

    for name in LOCAL_DEFINES:
        chunks.append(extract_define(source, name))
    chunks.append("")

    bodies = [extract_function(source, name) for name in FUNCTIONS]
    bodies += [extract_function(io_source, name) for name in IO_FUNCTIONS]

    chunks.append("/* forward declarations - see prototype() in the extractor */")
    for chunk in bodies:
        chunks.append(prototype(chunk))
    chunks.append("")

    for chunk in bodies:
        chunks.append(chunk)
        chunks.append("")

    text = "\n".join(chunks)

    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)

    print("wrote %s" % OUT)
    print("%d functions, %d lines" % (len(FUNCTIONS) + len(IO_FUNCTIONS),
                                      text.count("\n")))


if __name__ == "__main__":
    main()
