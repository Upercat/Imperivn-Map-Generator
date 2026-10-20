"""Release gate (PLAN.md W8): the shipped executable must not carry reverse-engineering details.

    python tools/check_strings.py <file.exe>

Scans the printable ASCII and UTF-16 strings of the file (6 characters or more) and fails on: addresses in the
original game's image range, cerebrivm record identifiers, the names of the analysis tools and projects, local
paths and debug-information references. Exit code 0 when clean, 1 with the offending strings listed.
"""
from __future__ import annotations

import re
import sys

FORBIDDEN = {
    "address of the original executable": re.compile(r"0x00[4-9A-Fa-f][0-9A-Fa-f]{5}(?![0-9A-Fa-f])"),
    "cerebrivm record identifier": re.compile(
        r"\b(?:claim|function|source|experiment|struct|format|system|issue|review|binary|global|vtable):[a-z0-9]"),
    "analysis tool or project name": re.compile(r"(?i)cerebrivm|ghidra|aftersaul|\bsaul\b|\bcdb\b|windbg"),
    "local path": re.compile(r"(?i)[a-z]:\\users\\|\\desktop\\|bakcup_dev|randommap_captures|iberivm_oracle"),
    "debug information": re.compile(r"(?i)\.pdb\b|RSDS"),
}


def strings(data: bytes) -> list[str]:
    out = [m.group().decode("ascii") for m in re.finditer(rb"[\x20-\x7e]{6,}", data)]
    out += [m.group().decode("utf-16le") for m in re.finditer(rb"(?:[\x20-\x7e]\x00){6,}", data)]
    return out


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[2].strip())
        return 2
    data = open(sys.argv[1], "rb").read()
    found = []
    for s in strings(data):
        for why, pattern in FORBIDDEN.items():
            if pattern.search(s):
                found.append((why, s))
    # the debug directory's CodeView record starts with "RSDS" even without printable neighbours
    if b"RSDS" in data:
        found.append(("debug information", "CodeView RSDS record"))
    for why, s in found:
        print(f"FORBIDDEN ({why}): {s[:160]}")
    print(f"{sys.argv[1]}: {len(found)} forbidden string(s)")
    return 1 if found else 0


if __name__ == "__main__":
    raise SystemExit(main())
