#!/usr/bin/env python3
"""Minimal xlsx -> CSV dumper used by docs/validation/row-to-multitable.md.

Zero third-party dependencies: pure stdlib zipfile + xml.etree. Handles:
- shared strings, inline strings, formulas with cached values
- numeric, boolean, error cells
- sparse rows / columns (missing cells filled with empty string up to the
  rightmost populated column observed across the sheet)

Limitations (intentional, MVP):
- Dates are written as the raw numeric serial (or whatever Excel stored).
  This script does not interpret date formats; for round-trip diff use it is
  enough that both sides see the same surface value.
- One sheet per invocation. Use --sheet to pick by name; default is the first
  sheet listed in workbook.xml.

Usage:
    xlsx2csv.py <path.xlsx> [--sheet <name>]
"""

import argparse
import csv
import re
import sys
import xml.etree.ElementTree as ET
import zipfile
from typing import Optional

NS = {
    "s": "http://schemas.openxmlformats.org/spreadsheetml/2006/main",
    "r": "http://schemas.openxmlformats.org/officeDocument/2006/relationships",
}


def _cell_ref_to_col(ref: str) -> int:
    """Convert "B5" -> 2 (1-based column index)."""
    m = re.match(r"([A-Z]+)\d+", ref)
    if not m:
        return 0
    letters = m.group(1)
    n = 0
    for ch in letters:
        n = n * 26 + (ord(ch) - ord("A") + 1)
    return n


def _read_shared_strings(zf: zipfile.ZipFile) -> list:
    try:
        with zf.open("xl/sharedStrings.xml") as f:
            tree = ET.parse(f)
    except KeyError:
        return []
    out = []
    for si in tree.getroot().findall("s:si", NS):
        # <si><t>X</t></si> or <si><r><t>X</t></r><r><t>Y</t></r></si>
        parts = [t.text or "" for t in si.iter("{%s}t" % NS["s"])]
        out.append("".join(parts))
    return out


def _read_sheet_map(zf: zipfile.ZipFile) -> dict:
    """sheet name -> xl/<path-in-zip>"""
    with zf.open("xl/workbook.xml") as f:
        wb = ET.parse(f).getroot()
    # rId -> sheet name
    sheets = {}
    for s in wb.findall("s:sheets/s:sheet", NS):
        name = s.attrib["name"]
        rid = s.attrib["{%s}id" % NS["r"]]
        sheets[rid] = name
    # rId -> relative target
    with zf.open("xl/_rels/workbook.xml.rels") as f:
        rels = ET.parse(f).getroot()
    target_by_rid = {
        rel.attrib["Id"]: rel.attrib["Target"]
        for rel in rels.findall("{http://schemas.openxmlformats.org/package/2006/relationships}Relationship")
    }
    out = {}
    for rid, name in sheets.items():
        target = target_by_rid.get(rid)
        if not target:
            continue
        if target.startswith("/"):
            path = target.lstrip("/")
        else:
            path = "xl/" + target
        out[name] = path
    return out


def _cell_value(c: ET.Element, sst: list) -> str:
    t = c.attrib.get("t", "n")
    v_el = c.find("s:v", NS)
    is_el = c.find("s:is", NS)

    if t == "s":  # shared string index
        if v_el is None or v_el.text is None:
            return ""
        try:
            return sst[int(v_el.text)]
        except (ValueError, IndexError):
            return ""
    if t == "inlineStr":
        if is_el is None:
            return ""
        parts = [t_el.text or "" for t_el in is_el.iter("{%s}t" % NS["s"])]
        return "".join(parts)
    if t == "str":
        return (v_el.text or "") if v_el is not None else ""
    if t == "b":
        return "TRUE" if (v_el is not None and v_el.text == "1") else "FALSE"
    if t == "e":
        return (v_el.text or "") if v_el is not None else ""
    # default: number (n) or no type
    return (v_el.text or "") if v_el is not None else ""


def dump(xlsx_path: str, sheet_name: Optional[str], out) -> None:
    with zipfile.ZipFile(xlsx_path) as zf:
        sst = _read_shared_strings(zf)
        sheet_map = _read_sheet_map(zf)
        if not sheet_map:
            print("no sheets found in workbook", file=sys.stderr)
            sys.exit(2)
        if sheet_name is None:
            sheet_name = next(iter(sheet_map))
        if sheet_name not in sheet_map:
            print(
                f"sheet '{sheet_name}' not found; available: {sorted(sheet_map)}",
                file=sys.stderr,
            )
            sys.exit(2)

        with zf.open(sheet_map[sheet_name]) as f:
            tree = ET.parse(f)

    rows = tree.getroot().findall("s:sheetData/s:row", NS)

    # First pass: determine max column width across all rows
    max_col = 0
    parsed_rows = []
    for row in rows:
        cells = []
        for c in row.findall("s:c", NS):
            col_idx = _cell_ref_to_col(c.attrib.get("r", ""))
            val = _cell_value(c, sst)
            cells.append((col_idx, val))
            if col_idx > max_col:
                max_col = col_idx
        parsed_rows.append(cells)

    writer = csv.writer(out)
    for cells in parsed_rows:
        out_row = [""] * max_col
        for col_idx, val in cells:
            if 1 <= col_idx <= max_col:
                out_row[col_idx - 1] = val
        writer.writerow(out_row)


def main() -> None:
    ap = argparse.ArgumentParser(description="Dump an xlsx sheet to CSV on stdout.")
    ap.add_argument("xlsx", help="Path to .xlsx file")
    ap.add_argument("--sheet", default=None, help="Sheet name (default: first sheet)")
    args = ap.parse_args()
    dump(args.xlsx, args.sheet, sys.stdout)


if __name__ == "__main__":
    main()
