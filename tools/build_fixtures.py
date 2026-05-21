#!/usr/bin/env python3
"""
Build xlsx fixture files for validation scenarios I-V.
Uses only Python standard library (zipfile + io).
"""
import zipfile
import io
import os
import sys

def col_letter(n):
    """Convert 1-based column index to letter(s): 1→A, 26→Z, 27→AA."""
    result = ''
    while n > 0:
        n, rem = divmod(n - 1, 26)
        result = chr(65 + rem) + result
    return result


def xml_escape(s):
    return (str(s)
            .replace('&', '&amp;')
            .replace('<', '&lt;')
            .replace('>', '&gt;'))


def build_xlsx(path, sheet_name, headers, rows):
    """Create a minimal .xlsx readable by QXlsx."""
    # Build shared-strings table
    strings = []
    str_idx = {}

    def intern(s):
        s = str(s)
        if s not in str_idx:
            str_idx[s] = len(strings)
            strings.append(s)
        return str_idx[s]

    # Pre-intern all strings so indices are stable
    for h in headers:
        intern(h)
    for row in rows:
        for cell in row:
            if cell is not None and str(cell) != '':
                # Only intern non-numeric values as shared strings
                v = str(cell)
                try:
                    float(v)
                except ValueError:
                    intern(v)

    # --- worksheet XML ---
    def ws_xml():
        total_rows = len(rows) + 1  # +1 for header
        total_cols = len(headers)
        dim_ref = f'A1:{col_letter(total_cols)}{total_rows}'
        lines = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
                 '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">',
                 f'<dimension ref="{dim_ref}"/>',
                 f'<sheetViews><sheetView workbookViewId="0"/></sheetViews>',
                 f'<sheetFormatPr defaultRowHeight="15"/>',
                 '<sheetData>']

        def cell_xml(ri, ci, value):
            ref = f'{col_letter(ci)}{ri}'
            if value is None or str(value) == '':
                return ''
            v = str(value)
            try:
                float(v)
                return f'<c r="{ref}"><v>{xml_escape(v)}</v></c>'
            except ValueError:
                idx = intern(v)
                return f'<c r="{ref}" t="s"><v>{idx}</v></c>'

        # Header row (row 1)
        row_cells = ''.join(cell_xml(1, ci + 1, h) for ci, h in enumerate(headers))
        lines.append(f'<row r="1">{row_cells}</row>')

        # Data rows
        for ri, row in enumerate(rows, 2):
            cells = ''.join(cell_xml(ri, ci + 1, v)
                            for ci, v in enumerate(row))
            if cells:
                lines.append(f'<row r="{ri}">{cells}</row>')

        lines += ['</sheetData>', '</worksheet>']
        return '\n'.join(lines)

    # --- sharedStrings XML ---
    def ss_xml():
        parts = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
                 f'<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"'
                 f' count="{len(strings)}" uniqueCount="{len(strings)}">']
        for s in strings:
            parts.append(f'<si><t xml:space="preserve">{xml_escape(s)}</t></si>')
        parts.append('</sst>')
        return '\n'.join(parts)

    content_types = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
  <Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
  <Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/>
  <Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>
</Types>'''

    rels = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>'''

    wb_xml = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"
          xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <sheets>
    <sheet name="{xml_escape(sheet_name)}" sheetId="1" r:id="rId1"/>
  </sheets>
</workbook>'''

    wb_rels = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
</Relationships>'''

    styles_xml = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <fonts count="1"><font><sz val="11"/><name val="Calibri"/></font></fonts>
  <fills count="2">
    <fill><patternFill patternType="none"/></fill>
    <fill><patternFill patternType="gray125"/></fill>
  </fills>
  <borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>
  <cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>
  <cellXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/></cellXfs>
</styleSheet>'''

    os.makedirs(os.path.dirname(path) if os.path.dirname(path) else '.', exist_ok=True)

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as zf:
        zf.writestr('[Content_Types].xml', content_types.encode('utf-8'))
        zf.writestr('_rels/.rels', rels.encode('utf-8'))
        zf.writestr('xl/workbook.xml', wb_xml.encode('utf-8'))
        zf.writestr('xl/_rels/workbook.xml.rels', wb_rels.encode('utf-8'))
        zf.writestr('xl/worksheets/sheet1.xml', ws_xml().encode('utf-8'))
        zf.writestr('xl/sharedStrings.xml', ss_xml().encode('utf-8'))
        zf.writestr('xl/styles.xml', styles_xml.encode('utf-8'))

    with open(path, 'wb') as f:
        f.write(buf.getvalue())
    print(f'Created: {path}')


# ── Scenario I: Orders.xlsx ───────────────────────────────────────────────────
def make_orders():
    build_xlsx(
        'tests/data/xlsx/Orders.xlsx',
        'Orders',
        ['OrderNo', 'Customer', 'Amount', 'LineNo', 'Sku', 'Qty'],
        [
            ['SO-001', 'Alice', '120.50', '1', 'SKU-A', '2'],
            ['SO-001', 'Alice', '120.50', '2', 'SKU-B', '1'],
            ['SO-002', 'Bob',   '80.00',  '1', 'SKU-A', '4'],
            ['SO-003', 'Carol', '15.00',  '1', 'SKU-C', '1'],
        ]
    )

# ── Scenario II: Mixed.xlsx ───────────────────────────────────────────────────
def make_mixed():
    headers = [
        'Type', 'OrderNo', 'Customer', 'Amount', 'LineNo', 'Sku', 'Qty',
        'ShipmentNo', 'Carrier', 'ETA', 'LegNo', 'Origin', 'Dest',
        'InvoiceNo', 'BillTo', 'Total', 'InvLineNo', 'Item', 'Price',
    ]
    E = ''  # empty cell
    rows = [
        ['A', 'SO-001', 'Alice', '120.50', '1', 'SKU-A', '2', E, E, E, E, E, E, E, E, E, E, E, E],
        ['A', 'SO-001', 'Alice', '120.50', '2', 'SKU-B', '1', E, E, E, E, E, E, E, E, E, E, E, E],
        ['B', E, E, E, E, E, E, 'SH-100', 'DHL', '2026-05-20', '1', 'Shanghai',  'Singapore', E, E, E, E, E, E],
        ['B', E, E, E, E, E, E, 'SH-100', 'DHL', '2026-05-20', '2', 'Singapore', 'Sydney',    E, E, E, E, E, E],
        ['C', E, E, E, E, E, E, E, E, E, E, E, E, 'INV-9001', 'Alice', '240.00', '1', 'Widget', '120.00'],
        ['C', E, E, E, E, E, E, E, E, E, E, E, E, 'INV-9001', 'Alice', '240.00', '2', 'Gadget', '120.00'],
    ]
    build_xlsx('tests/data/xlsx/Mixed.xlsx', 'Mixed', headers, rows)

# ── Scenario III: Events.xlsx ─────────────────────────────────────────────────
def make_events():
    build_xlsx(
        'tests/data/xlsx/Events.xlsx',
        'Events',
        ['EventID', 'Title', 'EventDate', 'EventDateTime', 'StartTime', 'LegacyDate', 'DateWithFallback'],
        [
            ['1', 'Workshop',   '2026/5/21',  '21/5/2026 9:30',   '09:30', '2026-05-21', '2026-05-21'],
            ['2', 'Conference', '2026/6/1',   '1/6/2026 14:00',   '14:00', '2026-06-01', '01/06/2026'],
            ['3', 'Seminar',    '2026/7/15',  '15/7/2026 18:30',  '18:30', '2026-07-15', '07/15/2026'],
        ]
    )

# ── Scenario IV: OrdersColOrder.xlsx ─────────────────────────────────────────
def make_orders_col_order():
    build_xlsx(
        'tests/data/xlsx/OrdersColOrder.xlsx',
        'Orders',
        ['OrderNo', 'TenantId', 'Total'],
        [
            ['SO-001', 'TENANT-A', '100.0'],
            ['SO-002', 'TENANT-B', '200.0'],
            ['SO-003', 'TENANT-A', '50.0'],
        ]
    )

# ── Scenario V: ReverseLookup.xlsx ───────────────────────────────────────────
def make_reverse_lookup():
    build_xlsx(
        'tests/data/xlsx/ReverseLookup.xlsx',
        'Orders',
        ['OrderNo', 'OrderDate', '客户编号'],
        [
            ['SO-001', '2026-05-21', 'C001'],
            ['SO-002', '2026-05-22', 'C002'],
            ['SO-003', '2026-05-23', 'C001'],
        ]
    )


# ── Scenario VI: EpochEvents.xlsx ────────────────────────────────────────────
def make_epoch_events():
    build_xlsx(
        'tests/data/xlsx/EpochEvents.xlsx',
        'EpochEvents',
        ['EventID', 'HappenAt'],
        [
            ['1', '2024-05-21 10:00:00'],
            ['2', '2024-06-01 00:00:00'],
            ['3', '1970-01-01 00:00:00'],
        ]
    )


if __name__ == '__main__':
    # Change to repo root
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(os.path.join(script_dir, '..'))

    make_orders()
    make_mixed()
    make_events()
    make_orders_col_order()
    make_reverse_lookup()
    make_epoch_events()
    print('All fixtures created.')
