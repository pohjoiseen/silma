#pragma once
// Minimal RAII wrapper around the sqlite3 C API.

#include <QByteArray>
#include <QString>
#include <QVariant>
#include <sqlite3.h>
#include <stdexcept>
#include <string>

namespace silma {

class DbError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Statement;

class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void open(const QString& path, bool create);
    void close();
    bool isOpen() const { return m_db != nullptr; }
    sqlite3* handle() const { return m_db; }

    void exec(const char* sql);
    Statement prepare(const char* sql);
    qint64 lastInsertId() const;
    int changes() const;

    int userVersion();
    void setUserVersion(int v);

private:
    sqlite3* m_db = nullptr;
};

class Statement {
public:
    Statement(sqlite3* db, const char* sql);
    ~Statement();
    Statement(Statement&& o) noexcept : m_db(o.m_db), m_stmt(o.m_stmt) { o.m_stmt = nullptr; }
    Statement(const Statement&) = delete;

    Statement& bind(int idx, const QVariant& v);
    Statement& bind(int idx, const QString& v);
    Statement& bind(int idx, const char* v) { return bind(idx, QString::fromUtf8(v)); }
    Statement& bind(int idx, qint64 v);
    Statement& bind(int idx, int v) { return bind(idx, qint64(v)); }
    Statement& bind(int idx, double v);
    Statement& bind(int idx, const QByteArray& blob);
    Statement& bindNull(int idx);

    // Returns true while there is a row available.
    bool step();
    void exec() { step(); }
    void reset();

    bool isNull(int col) const;
    qint64 i64(int col) const;
    int i32(int col) const { return int(i64(col)); }
    double f64(int col) const;
    QString str(int col) const;
    QByteArray blob(int col) const;

private:
    sqlite3* m_db;
    sqlite3_stmt* m_stmt = nullptr;
};

class Transaction {
public:
    explicit Transaction(Database& db) : m_db(db) { m_db.exec("BEGIN IMMEDIATE"); }
    ~Transaction()
    {
        if (!m_done) {
            try { m_db.exec("ROLLBACK"); } catch (...) {}
        }
    }
    void commit()
    {
        m_db.exec("COMMIT");
        m_done = true;
    }

private:
    Database& m_db;
    bool m_done = false;
};

} // namespace silma
