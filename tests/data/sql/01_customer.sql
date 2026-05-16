-- Customer table for single-table tests
CREATE TABLE IF NOT EXISTS customer (
    customer_no TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    phone TEXT,
    extra_col TEXT DEFAULT 'untouched'
);
