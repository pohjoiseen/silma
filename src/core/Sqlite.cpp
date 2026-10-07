#include "Sqlite.h"

namespace silma {

static std::string errmsg(sqlite3* db, const char* what)
{
    return std::string(what) + ": " + (db ? sqlite3_errmsg(db) : "no database");
}

Database::~Database() { close(); }

void Database::open(const QString& path, bool create)
{
    close();
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX;
    if (create)
        flags |= SQLITE_OPEN_CREATE;
    if (sqlite3_open_v2(path.toUtf8().constData(), &m_db, flags, nullptr) != SQLITE_OK) {
        std::string e = errmsg(m_db, "Cannot open database");
        sqlite3_close(m_db);
        m_db = nullptr;
        throw DbError(e);
    }
    sqlite3_busy_timeout(m_db, 5000);
    exec("PRAGMA foreign_keys = ON");
    exec("PRAGMA journal_mode = WAL");
    exec("PRAGMA synchronous = NORMAL");
}

void Database::close()
{
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

void Database::exec(const char* sql)
{
    char* err = nullptr;
    if (sqlite3_exec(m_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = std::string("SQL error: ") + (err ? err : "?") + " in: " + sql;
        sqlite3_free(err);
        throw DbError(e);
    }
}

Statement Database::prepare(const char* sql) { return Statement(m_db, sql); }
qint64 Database::lastInsertId() const { return sqlite3_last_insert_rowid(m_db); }
int Database::changes() const { return sqlite3_changes(m_db); }

int Database::userVersion()
{
    auto st = prepare("PRAGMA user_version");
    st.step();
    return st.i32(0);
}

void Database::setUserVersion(int v)
{
    exec(("PRAGMA user_version = " + std::to_string(v)).c_str());
}

Statement::Statement(sqlite3* db, const char* sql) : m_db(db)
{
    if (sqlite3_prepare_v2(db, sql, -1, &m_stmt, nullptr) != SQLITE_OK)
        throw DbError(errmsg(db, "Prepare failed") + " in: " + sql);
}

Statement::~Statement()
{
    if (m_stmt)
        sqlite3_finalize(m_stmt);
}

Statement& Statement::bind(int idx, const QVariant& v)
{
    if (v.isNull())
        return bindNull(idx);
    switch (v.typeId()) {
    case QMetaType::Int:
    case QMetaType::LongLong:
    case QMetaType::UInt:
    case QMetaType::Bool:
        return bind(idx, v.toLongLong());
    case QMetaType::Double:
    case QMetaType::Float:
        return bind(idx, v.toDouble());
    case QMetaType::QByteArray:
        return bind(idx, v.toByteArray());
    default:
        return bind(idx, v.toString());
    }
}

Statement& Statement::bind(int idx, const QString& v)
{
    if (v.isNull())
        return bindNull(idx);
    QByteArray u = v.toUtf8();
    sqlite3_bind_text(m_stmt, idx, u.constData(), int(u.size()), SQLITE_TRANSIENT);
    return *this;
}

Statement& Statement::bind(int idx, qint64 v)
{
    sqlite3_bind_int64(m_stmt, idx, v);
    return *this;
}

Statement& Statement::bind(int idx, double v)
{
    sqlite3_bind_double(m_stmt, idx, v);
    return *this;
}

Statement& Statement::bind(int idx, const QByteArray& blob)
{
    if (blob.isNull())
        return bindNull(idx);
    sqlite3_bind_blob(m_stmt, idx, blob.constData(), int(blob.size()), SQLITE_TRANSIENT);
    return *this;
}

Statement& Statement::bindNull(int idx)
{
    sqlite3_bind_null(m_stmt, idx);
    return *this;
}

bool Statement::step()
{
    int rc = sqlite3_step(m_stmt);
    if (rc == SQLITE_ROW)
        return true;
    if (rc == SQLITE_DONE)
        return false;
    throw DbError(errmsg(m_db, "Step failed"));
}

void Statement::reset()
{
    sqlite3_reset(m_stmt);
    sqlite3_clear_bindings(m_stmt);
}

bool Statement::isNull(int col) const { return sqlite3_column_type(m_stmt, col) == SQLITE_NULL; }
qint64 Statement::i64(int col) const { return sqlite3_column_int64(m_stmt, col); }
double Statement::f64(int col) const { return sqlite3_column_double(m_stmt, col); }

QString Statement::str(int col) const
{
    auto p = reinterpret_cast<const char*>(sqlite3_column_text(m_stmt, col));
    if (!p)
        return {};
    return QString::fromUtf8(p, sqlite3_column_bytes(m_stmt, col));
}

QByteArray Statement::blob(int col) const
{
    auto p = static_cast<const char*>(sqlite3_column_blob(m_stmt, col));
    if (!p)
        return {};
    return QByteArray(p, sqlite3_column_bytes(m_stmt, col));
}

} // namespace silma
