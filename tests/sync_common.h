// tests/sync_common.h  — shared helpers for all sync unit tests
#pragma once
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"

// Open an in-memory SQLite DB and create all __sync_* tables.
// Returns the connection name; caller must call closeSyncDb() in cleanup().
inline QString openSyncDb(QSqlDatabase& outDb) {
    const QString name =
        QStringLiteral("tst_sync_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    outDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    outDb.setDatabaseName(QStringLiteral(":memory:"));
    if (!outDb.open())
        return {};
    QSqlQuery q(outDb);
    for (const QString& ddl : dbridge::sync::ddl::allCreateStatements())
        q.exec(ddl);  // index creates on empty table are benign
    return name;
}

// Open a temp-file SQLite DB (needed for cross-connection visibility tests).
inline QString openSyncFileDb(QSqlDatabase& outDb, const QString& filePath) {
    const QString name = QStringLiteral("tst_sync_file_") +
                         QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    outDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    outDb.setDatabaseName(filePath);
    if (!outDb.open())
        return {};
    QSqlQuery q(outDb);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    for (const QString& ddl : dbridge::sync::ddl::allCreateStatements())
        q.exec(ddl);
    return name;
}

inline void closeSyncDb(const QString& connName) {
    if (QSqlDatabase::contains(connName)) {
        QSqlDatabase::database(connName).close();
        QSqlDatabase::removeDatabase(connName);
    }
}

// Assert that call returned false and *err contains the expected code string.
#define ASSERT_ERR_CODE(ok_val, err_str, code)                                                    \
    do {                                                                                          \
        QVERIFY2(!(ok_val), "Expected failure, got success");                                     \
        QVERIFY2((err_str).contains(QLatin1String(code)),                                         \
                 qPrintable(                                                                      \
                     QString("Expected '%1' in error: %2").arg(QLatin1String(code), (err_str)))); \
    } while (false)
