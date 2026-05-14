-- Multi-table orders test schema
CREATE TABLE IF NOT EXISTS orders (
    order_no TEXT PRIMARY KEY,
    customer TEXT NOT NULL,
    amount REAL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS order_items (
    order_no TEXT NOT NULL,
    line_no INTEGER NOT NULL,
    sku TEXT NOT NULL,
    qty INTEGER DEFAULT 1,
    PRIMARY KEY (order_no, line_no),
    FOREIGN KEY (order_no) REFERENCES orders(order_no)
);
