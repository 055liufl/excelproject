CREATE TABLE IF NOT EXISTS event (
    event_id           INTEGER PRIMARY KEY,
    title              TEXT    NOT NULL,
    event_date         TEXT,            -- 存储格式 yyyy-MM-dd
    event_datetime     TEXT,            -- 存储格式 yyyy-MM-dd HH:mm:ss
    start_time         TEXT,            -- 存储格式 HH:mm:ss
    legacy_date        TEXT,            -- 存储格式 yyyy-MM-dd（旧式 date:yyyy-MM-dd validator）
    date_with_fallback TEXT             -- 存储格式 yyyy-MM-dd
);
