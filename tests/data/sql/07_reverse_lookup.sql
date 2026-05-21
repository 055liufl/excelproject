CREATE TABLE IF NOT EXISTS ref_customers (
    c_no   TEXT PRIMARY KEY,
    c_name TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS orders (
    order_no      TEXT PRIMARY KEY,
    order_date    TEXT,
    customer_name TEXT            -- H 列：导入时由 lookup select 填入
);
