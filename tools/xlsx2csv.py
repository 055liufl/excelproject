#!/usr/bin/env python3
"""Minimal xlsx -> CSV dumper used by docs/validation/row-to-multitable.md.

Zero third-party dependencies: pure stdlib zipfile + xml.etree. Handles:
- shared strings, inline strings, formulas with cached values
- numeric, boolean, error cells
- date / datetime / time cells: read xl/styles.xml, recognise built-in date
  formats and custom formats containing y/m/d/h/s, emit ISO-like strings so
  round-trip diffing surfaces the same form on both sides.
- sparse rows / columns (missing cells filled with empty string up to the
  rightmost populated column observed across the sheet)

Usage:
    xlsx2csv.py <path.xlsx> [--sheet <name>]
"""

import argparse
import csv
import datetime as _dt
import re
import sys
import xml.etree.ElementTree as ET
import zipfile
from typing import Optional

NS = {
    "s": "http://schemas.openxmlformats.org/spreadsheetml/2006/main",
    "r": "http://schemas.openxmlformats.org/officeDocument/2006/relationships",
}

# Built-in numFmt IDs that Excel reserves for date / time formats.
# Source: ECMA-376 Part 1, §18.8.30 numFmt.
_BUILTIN_DATE_FMT_IDS = {14, 15, 16, 17, 22, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
                         45, 46, 47, 50, 51, 52, 53, 54, 55, 56, 57, 58}
_BUILTIN_TIME_ONLY_FMT_IDS = {18, 19, 20, 21, 45, 46, 47}

_DATE_CODE_RE = re.compile(r"[ymdhsYMDHS]")


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
        parts = [t.text or "" for t in si.iter("{%s}t" % NS["s"])]
        out.append("".join(parts))
    return out


def _read_sheet_map(zf: zipfile.ZipFile) -> dict:
    """sheet name -> xl/<path-in-zip>"""
    with zf.open("xl/workbook.xml") as f:
        wb = ET.parse(f).getroot()
    sheets = {}
    for s in wb.findall("s:sheets/s:sheet", NS):
        name = s.attrib["name"]
        rid = s.attrib["{%s}id" % NS["r"]]
        sheets[rid] = name
    with zf.open("xl/_rels/workbook.xml.rels") as f:
        rels = ET.parse(f).getroot()
    target_by_rid = {
        rel.attrib["Id"]: rel.attrib["Target"]
        for rel in rels.findall(
            "{http://schemas.openxmlformats.org/package/2006/relationships}Relationship"
        )
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


def _read_style_date_flags(zf: zipfile.ZipFile):
    """Return (style_idx_is_date, style_idx_is_time_only).

    Each is a list indexed by cellXfs position; True means the cell uses a
    date / datetime format. style_idx_is_time_only flags formats that show
    only the time component, so we emit "HH:MM:SS" rather than a full date.
    """
    try:
        with zf.open("xl/styles.xml") as f:
            tree = ET.parse(f)
    except KeyError:
        return [], []
    root = tree.getroot()

    custom_formats = {}  # numFmtId -> formatCode
    nfmts = root.find("s:numFmts", NS)
    if nfmts is not None:
        for nf in nfmts.findall("s:numFmt", NS):
            try:
                fid = int(nf.attrib.get("numFmtId", "-1"))
            except ValueError:
                continue
            custom_formats[fid] = nf.attrib.get("formatCode", "")

    is_date_cache = {}
    is_time_only_cache = {}

    def classify(fmt_id: int):
        if fmt_id in is_date_cache:
            return is_date_cache[fmt_id], is_time_only_cache[fmt_id]
        is_date = False
        is_time = False
        if fmt_id in _BUILTIN_DATE_FMT_IDS:
            is_date = True
            is_time = fmt_id in _BUILTIN_TIME_ONLY_FMT_IDS
        else:
            code = custom_formats.get(fmt_id, "")
            # Strip quoted literals and bracket escapes to avoid false matches.
            stripped = re.sub(r'"[^"]*"', "", code)
            stripped = re.sub(r"\[[^\]]*\]", "", stripped)
            # "m" alone is ambiguous (month vs minute); rely on unambiguous
            # markers: y/d for date, h/s for time.
            has_date_token = any(ch in stripped for ch in "ydYD")
            has_time_token = any(ch in stripped for ch in "hsHS")
            if has_date_token or has_time_token:
                is_date = True
                if has_time_token and not has_date_token:
                    is_time = True
        is_date_cache[fmt_id] = is_date
        is_time_only_cache[fmt_id] = is_time
        return is_date, is_time

    style_is_date = []
    style_is_time = []
    cell_xfs = root.find("s:cellXfs", NS)
    if cell_xfs is not None:
        for xf in cell_xfs.findall("s:xf", NS):
            try:
                fid = int(xf.attrib.get("numFmtId", "0"))
            except ValueError:
                fid = 0
            d, t = classify(fid)
            style_is_date.append(d)
            style_is_time.append(t)
    return style_is_date, style_is_time


def _excel_serial_to_iso(serial: float, time_only: bool) -> str:
    """Convert an Excel 1900-mode date serial to ISO 8601.

    Excel treats 1900 as a leap year (bug); we compensate by anchoring the
    epoch at 1899-12-30 for serials >= 60 and at 1899-12-31 for smaller
    values. Anything earlier returns the raw serial.
    """
    try:
        if serial < 0:
            return str(serial)
        whole_days = int(serial)
        frac = serial - whole_days
        epoch = _dt.date(1899, 12, 30) if whole_days >= 60 else _dt.date(1899, 12, 31)
        d = epoch + _dt.timedelta(days=whole_days)
        # Time portion (fraction of a day)
        secs = round(frac * 86400)
        hh, rem = divmod(secs, 3600)
        mm, ss = divmod(rem, 60)
        if time_only:
            return f"{hh:02d}:{mm:02d}:{ss:02d}"
        if secs == 0:
            return d.isoformat()
        return f"{d.isoformat()} {hh:02d}:{mm:02d}:{ss:02d}"
    except (ValueError, OverflowError):
        return str(serial)


def _cell_value(c: ET.Element, sst: list, style_is_date, style_is_time) -> str:
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

    # default: number (n) or no type. Check style for date interpretation.
    if v_el is None or v_el.text is None:
        return ""
    raw = v_el.text
    s_attr = c.attrib.get("s")
    if s_attr is not None and style_is_date:
        try:
            sidx = int(s_attr)
        except ValueError:
            sidx = -1
        if 0 <= sidx < len(style_is_date) and style_is_date[sidx]:
            try:
                serial = float(raw)
            except ValueError:
                return raw
            return _excel_serial_to_iso(serial, style_is_time[sidx])
    return raw


def dump(xlsx_path: str, sheet_name: Optional[str], out) -> None:
    with zipfile.ZipFile(xlsx_path) as zf:
        sst = _read_shared_strings(zf)
        style_is_date, style_is_time = _read_style_date_flags(zf)
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

    max_col = 0
    parsed_rows = []
    for row in rows:
        cells = []
        for c in row.findall("s:c", NS):
            col_idx = _cell_ref_to_col(c.attrib.get("r", ""))
            val = _cell_value(c, sst, style_is_date, style_is_time)
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
