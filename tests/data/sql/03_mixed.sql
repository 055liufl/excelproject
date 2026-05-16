-- Mixed A/B/C test schema
CREATE TABLE IF NOT EXISTS m1 (
    m_no TEXT PRIMARY KEY,
    m_data TEXT
);

CREATE TABLE IF NOT EXISTS m2 (
    m_no TEXT NOT NULL,
    line_no INTEGER NOT NULL,
    m_detail TEXT,
    PRIMARY KEY (m_no, line_no),
    FOREIGN KEY (m_no) REFERENCES m1(m_no)
);

CREATE TABLE IF NOT EXISTS n1 (
    n_no TEXT PRIMARY KEY,
    n_data TEXT
);

CREATE TABLE IF NOT EXISTS o1 (
    o_no TEXT PRIMARY KEY,
    o_data TEXT
);
