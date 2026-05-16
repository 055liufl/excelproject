-- Mixed A/B/C with each class routing to its own multi-table set.
-- Used by docs/validation/row-to-multitable.md §II (Scenario II).

-- m set: A class
CREATE TABLE IF NOT EXISTS orders (
    order_no TEXT PRIMARY KEY,
    customer TEXT NOT NULL,
    amount REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS order_items (
    order_no TEXT NOT NULL,
    line_no INTEGER NOT NULL,
    sku TEXT NOT NULL,
    qty INTEGER NOT NULL,
    PRIMARY KEY (order_no, line_no),
    FOREIGN KEY (order_no) REFERENCES orders(order_no)
);

-- n set: B class
CREATE TABLE IF NOT EXISTS shipments (
    shipment_no TEXT PRIMARY KEY,
    carrier TEXT NOT NULL,
    eta TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS shipment_legs (
    shipment_no TEXT NOT NULL,
    leg_no INTEGER NOT NULL,
    origin TEXT NOT NULL,
    dest TEXT NOT NULL,
    PRIMARY KEY (shipment_no, leg_no),
    FOREIGN KEY (shipment_no) REFERENCES shipments(shipment_no)
);

-- o set: C class
CREATE TABLE IF NOT EXISTS invoices (
    invoice_no TEXT PRIMARY KEY,
    bill_to TEXT NOT NULL,
    total REAL NOT NULL
);
CREATE TABLE IF NOT EXISTS invoice_lines (
    invoice_no TEXT NOT NULL,
    line_no INTEGER NOT NULL,
    item TEXT NOT NULL,
    price REAL NOT NULL,
    PRIMARY KEY (invoice_no, line_no),
    FOREIGN KEY (invoice_no) REFERENCES invoices(invoice_no)
);
