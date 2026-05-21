CREATE TABLE IF NOT EXISTS orders (
    order_no  TEXT PRIMARY KEY,
    tenant_id TEXT NOT NULL,
    total     REAL DEFAULT 0
);
