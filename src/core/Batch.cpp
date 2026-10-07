#include "Batch.h"

#include <QFileInfo>

namespace silma {

namespace {

// Each entry migrates the schema from version (index) to (index + 1).
// Never edit an existing entry once released; append a new one instead.
const char* const kMigrations[] = {
    // 0 -> 1: initial schema
    R"SQL(
    CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);
    CREATE TABLE imports (
        id INTEGER PRIMARY KEY,
        name TEXT NOT NULL UNIQUE,
        folder TEXT NOT NULL,
        created_at TEXT NOT NULL
    );
    CREATE TABLE photos (
        id INTEGER PRIMARY KEY,
        import_id INTEGER NOT NULL REFERENCES imports(id) ON DELETE CASCADE,
        file_name TEXT NOT NULL,
        path TEXT NOT NULL UNIQUE,
        file_size INTEGER,
        capture_time TEXT,
        make TEXT, model TEXT, lens TEXT,
        width INTEGER, height INTEGER, orientation INTEGER,
        iso REAL, exposure_time REAL, f_number REAL, focal_length REAL,
        rejected INTEGER NOT NULL DEFAULT 0,
        kind INTEGER NOT NULL DEFAULT 0,
        derived_from TEXT,
        history_pos INTEGER NOT NULL DEFAULT 0,
        added_at TEXT NOT NULL
    );
    CREATE INDEX photos_import ON photos(import_id, capture_time);
    CREATE TABLE history (
        photo_id INTEGER NOT NULL REFERENCES photos(id) ON DELETE CASCADE,
        seq INTEGER NOT NULL,
        label TEXT NOT NULL,
        settings TEXT NOT NULL,
        created_at TEXT NOT NULL,
        PRIMARY KEY (photo_id, seq)
    );
    CREATE TABLE thumbnails (
        photo_id INTEGER PRIMARY KEY REFERENCES photos(id) ON DELETE CASCADE,
        version INTEGER NOT NULL,
        jpeg BLOB NOT NULL
    );
    CREATE TABLE state (key TEXT PRIMARY KEY, value TEXT);
    )SQL",
    // 1 -> 2: generated heal patches
    R"SQL(
    CREATE TABLE patches (
        id INTEGER PRIMARY KEY,
        photo_id INTEGER NOT NULL REFERENCES photos(id) ON DELETE CASCADE,
        x INTEGER NOT NULL, y INTEGER NOT NULL, w INTEGER NOT NULL, h INTEGER NOT NULL,
        data BLOB NOT NULL,
        created_at TEXT NOT NULL
    );
    CREATE INDEX patches_photo ON patches(photo_id);
    )SQL",
};

constexpr int kMigrationCount = int(std::size(kMigrations));
static_assert(kMigrationCount == Batch::kSchemaVersion);

QString nowIso() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }

const char* kPhotoColumns =
    "id, import_id, file_name, path, file_size, capture_time, make, model, lens, width, height, "
    "orientation, iso, exposure_time, f_number, focal_length, rejected, kind, derived_from, history_pos";

} // namespace

QString photoKindName(PhotoKind k)
{
    return k == PhotoKind::Raw ? QStringLiteral("raw") : QStringLiteral("derived-dng");
}

void Batch::create(const QString& file)
{
    QFileInfo fi(file);
    if (fi.exists())
        throw DbError(("Batch file already exists: " + file).toStdString());
    if (!QDir().mkpath(fi.absolutePath()))
        throw DbError(("Cannot create directory for " + file).toStdString());
    m_db.open(fi.absoluteFilePath(), true);
    m_file = fi.absoluteFilePath();
    m_root = fi.absoluteDir();
    migrate();
    setState(QStringLiteral("created_at"), nowIso());
}

void Batch::open(const QString& file)
{
    QFileInfo fi(file);
    if (!fi.exists())
        throw DbError(("Batch file not found: " + file).toStdString());
    m_db.open(fi.absoluteFilePath(), false);
    m_file = fi.absoluteFilePath();
    m_root = fi.absoluteDir();
    migrate();
}

void Batch::close()
{
    m_db.close();
    m_file.clear();
}

QString Batch::name() const { return QFileInfo(m_file).completeBaseName(); }

void Batch::migrate()
{
    int v = m_db.userVersion();
    if (v > kSchemaVersion) {
        m_db.close();
        throw DbError("Batch was created by a newer version of Silmä (schema " + std::to_string(v) +
                      ", supported " + std::to_string(kSchemaVersion) + ")");
    }
    if (v > 0) {
        // Make sure this is our file.
        auto st = m_db.prepare("SELECT value FROM meta WHERE key = 'format'");
        if (!st.step() || st.str(0) != QLatin1String("silma-batch"))
            throw DbError("Not a Silmä batch file");
    }
    if (v == kSchemaVersion)
        return;
    Transaction tx(m_db);
    for (; v < kSchemaVersion; ++v)
        m_db.exec(kMigrations[v]);
    m_db.setUserVersion(kSchemaVersion);
    auto st = m_db.prepare("INSERT OR REPLACE INTO meta(key, value) VALUES (?, ?)");
    st.bind(1, "format").bind(2, "silma-batch").exec();
    st.reset();
    st.bind(1, "schema_version").bind(2, QString::number(kSchemaVersion)).exec();
    st.reset();
    st.bind(1, "written_by").bind(2, QStringLiteral("Silmä " SILMA_VERSION)).exec();
    tx.commit();
}

QString Batch::relativize(const QString& absPath) const
{
    QString rel = m_root.relativeFilePath(absPath);
    // Keep absolute paths for anything outside the batch root.
    if (rel.startsWith(QLatin1String("../")) || QDir::isAbsolutePath(rel))
        return QDir::cleanPath(absPath);
    return rel;
}

QString Batch::absolutize(const QString& stored) const
{
    if (QDir::isAbsolutePath(stored))
        return stored;
    return QDir::cleanPath(m_root.absoluteFilePath(stored));
}

// ---- imports ---------------------------------------------------------------

QList<ImportInfo> Batch::imports()
{
    QList<ImportInfo> out;
    auto st = m_db.prepare(
        "SELECT i.id, i.name, i.folder, i.created_at, "
        "  (SELECT COUNT(*) FROM photos p WHERE p.import_id = i.id), "
        "  (SELECT COUNT(*) FROM photos p WHERE p.import_id = i.id AND p.rejected) "
        "FROM imports i ORDER BY i.name");
    while (st.step()) {
        ImportInfo im;
        im.id = st.i64(0);
        im.name = st.str(1);
        im.path = absolutize(st.str(2));
        im.created = QDateTime::fromString(st.str(3), Qt::ISODateWithMs);
        im.photoCount = st.i32(4);
        im.rejectedCount = st.i32(5);
        out.append(im);
    }
    return out;
}

std::optional<ImportInfo> Batch::importById(qint64 id)
{
    for (const auto& im : imports())
        if (im.id == id)
            return im;
    return std::nullopt;
}

std::optional<ImportInfo> Batch::importByName(const QString& name)
{
    for (const auto& im : imports())
        if (im.name == name)
            return im;
    return std::nullopt;
}

ImportInfo Batch::addImport(const QString& name, const QString& folder)
{
    if (name.isEmpty())
        throw DbError("Import name must not be empty");
    QString abs = absolutize(folder.isEmpty() ? name : folder);
    if (!QDir().mkpath(abs))
        throw DbError(("Cannot create import folder " + abs).toStdString());
    auto st = m_db.prepare("INSERT INTO imports(name, folder, created_at) VALUES (?, ?, ?)");
    st.bind(1, name).bind(2, relativize(abs)).bind(3, nowIso()).exec();
    return *importById(m_db.lastInsertId());
}

void Batch::removeImport(qint64 id)
{
    auto st = m_db.prepare("DELETE FROM imports WHERE id = ?");
    st.bind(1, id).exec();
}

// ---- photos ----------------------------------------------------------------

PhotoInfo Batch::readPhoto(Statement& st)
{
    PhotoInfo p;
    p.id = st.i64(0);
    p.importId = st.i64(1);
    p.fileName = st.str(2);
    p.path = absolutize(st.str(3));
    p.fileSize = st.i64(4);
    p.captureTime = QDateTime::fromString(st.str(5), Qt::ISODate);
    p.make = st.str(6);
    p.model = st.str(7);
    p.lens = st.str(8);
    p.width = st.i32(9);
    p.height = st.i32(10);
    p.orientation = st.isNull(11) ? 1 : st.i32(11);
    p.iso = st.f64(12);
    p.exposureTime = st.f64(13);
    p.fNumber = st.f64(14);
    p.focalLength = st.f64(15);
    p.rejected = st.i32(16) != 0;
    p.kind = PhotoKind(st.i32(17));
    p.derivedFrom = st.str(18);
    p.historyPos = st.i32(19);
    return p;
}

QList<PhotoInfo> Batch::photos(std::optional<qint64> importId)
{
    QList<PhotoInfo> out;
    QByteArray sql = QByteArray("SELECT ") + kPhotoColumns + " FROM photos";
    if (importId)
        sql += " WHERE import_id = ?";
    sql += " ORDER BY capture_time, file_name, id";
    auto st = m_db.prepare(sql.constData());
    if (importId)
        st.bind(1, *importId);
    while (st.step())
        out.append(readPhoto(st));
    return out;
}

std::optional<PhotoInfo> Batch::photo(qint64 id)
{
    QByteArray sql = QByteArray("SELECT ") + kPhotoColumns + " FROM photos WHERE id = ?";
    auto st = m_db.prepare(sql.constData());
    st.bind(1, id);
    if (st.step())
        return readPhoto(st);
    return std::nullopt;
}

std::optional<PhotoInfo> Batch::photoByPath(const QString& absPath)
{
    QByteArray sql = QByteArray("SELECT ") + kPhotoColumns + " FROM photos WHERE path = ?";
    auto st = m_db.prepare(sql.constData());
    st.bind(1, relativize(absPath));
    if (st.step())
        return readPhoto(st);
    return std::nullopt;
}

qint64 Batch::addPhoto(const PhotoInfo& p, const QString& initialSettings)
{
    Transaction tx(m_db);
    auto st = m_db.prepare(
        "INSERT INTO photos(import_id, file_name, path, file_size, capture_time, make, model, lens, "
        "width, height, orientation, iso, exposure_time, f_number, focal_length, rejected, kind, "
        "derived_from, history_pos, added_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,?)");
    st.bind(1, p.importId)
        .bind(2, p.fileName)
        .bind(3, relativize(p.path))
        .bind(4, p.fileSize)
        .bind(5, p.captureTime.isValid() ? p.captureTime.toString(Qt::ISODate) : QString())
        .bind(6, p.make)
        .bind(7, p.model)
        .bind(8, p.lens)
        .bind(9, p.width)
        .bind(10, p.height)
        .bind(11, p.orientation)
        .bind(12, p.iso)
        .bind(13, p.exposureTime)
        .bind(14, p.fNumber)
        .bind(15, p.focalLength)
        .bind(16, p.rejected ? 1 : 0)
        .bind(17, int(p.kind))
        .bind(18, p.derivedFrom)
        .bind(19, nowIso())
        .exec();
    qint64 id = m_db.lastInsertId();
    auto h = m_db.prepare("INSERT INTO history(photo_id, seq, label, settings, created_at) VALUES (?, 0, ?, ?, ?)");
    h.bind(1, id).bind(2, p.kind == PhotoKind::Raw ? "Import" : "Created").bind(3, initialSettings).bind(4, nowIso()).exec();
    tx.commit();
    return id;
}

void Batch::setRejected(qint64 photoId, bool rejected)
{
    auto st = m_db.prepare("UPDATE photos SET rejected = ? WHERE id = ?");
    st.bind(1, rejected ? 1 : 0).bind(2, photoId).exec();
}

void Batch::removePhoto(qint64 photoId)
{
    auto st = m_db.prepare("DELETE FROM photos WHERE id = ?");
    st.bind(1, photoId).exec();
}

// ---- thumbnails ------------------------------------------------------------

QByteArray Batch::thumbnail(qint64 photoId, int* version)
{
    auto st = m_db.prepare("SELECT jpeg, version FROM thumbnails WHERE photo_id = ?");
    st.bind(1, photoId);
    if (!st.step())
        return {};
    if (version)
        *version = st.i32(1);
    return st.blob(0);
}

void Batch::setThumbnail(qint64 photoId, const QByteArray& jpeg, int version)
{
    auto st = m_db.prepare("INSERT OR REPLACE INTO thumbnails(photo_id, version, jpeg) VALUES (?, ?, ?)");
    st.bind(1, photoId).bind(2, version).bind(3, jpeg).exec();
}

// ---- history ---------------------------------------------------------------

int Batch::historyPos(qint64 photoId)
{
    auto st = m_db.prepare("SELECT history_pos FROM photos WHERE id = ?");
    st.bind(1, photoId);
    if (!st.step())
        throw DbError("No such photo");
    return st.i32(0);
}

QString Batch::settings(qint64 photoId)
{
    auto st = m_db.prepare(
        "SELECT h.settings FROM history h JOIN photos p ON p.id = h.photo_id AND h.seq = p.history_pos "
        "WHERE p.id = ?");
    st.bind(1, photoId);
    if (!st.step())
        return QStringLiteral("{}");
    return st.str(0);
}

QList<HistoryEntry> Batch::history(qint64 photoId)
{
    QList<HistoryEntry> out;
    auto st = m_db.prepare("SELECT seq, label, settings, created_at FROM history WHERE photo_id = ? ORDER BY seq");
    st.bind(1, photoId);
    while (st.step())
        out.append({st.i32(0), st.str(1), st.str(2), QDateTime::fromString(st.str(3), Qt::ISODateWithMs)});
    return out;
}

int Batch::pushHistory(qint64 photoId, const QString& label, const QString& settings)
{
    Transaction tx(m_db);
    int pos = historyPos(photoId);
    auto del = m_db.prepare("DELETE FROM history WHERE photo_id = ? AND seq > ?");
    del.bind(1, photoId).bind(2, pos).exec();
    int seq = pos + 1;
    auto ins = m_db.prepare("INSERT INTO history(photo_id, seq, label, settings, created_at) VALUES (?, ?, ?, ?, ?)");
    ins.bind(1, photoId).bind(2, seq).bind(3, label).bind(4, settings).bind(5, nowIso()).exec();
    auto up = m_db.prepare("UPDATE photos SET history_pos = ? WHERE id = ?");
    up.bind(1, seq).bind(2, photoId).exec();
    tx.commit();
    return seq;
}

QString Batch::setHistoryPos(qint64 photoId, int pos)
{
    auto chk = m_db.prepare("SELECT settings FROM history WHERE photo_id = ? AND seq = ?");
    chk.bind(1, photoId).bind(2, pos);
    if (!chk.step())
        throw DbError("No such history entry");
    QString s = chk.str(0);
    auto up = m_db.prepare("UPDATE photos SET history_pos = ? WHERE id = ?");
    up.bind(1, pos).bind(2, photoId).exec();
    return s;
}

qint64 Batch::addPatch(qint64 photoId, const QRect& r, const QByteArray& data)
{
    auto st = m_db.prepare("INSERT INTO patches(photo_id, x, y, w, h, data, created_at) VALUES (?,?,?,?,?,?,?)");
    st.bind(1, photoId).bind(2, r.x()).bind(3, r.y()).bind(4, r.width()).bind(5, r.height()).bind(6, data).bind(7, nowIso()).exec();
    return m_db.lastInsertId();
}

bool Batch::patch(qint64 patchId, QRect* rect, QByteArray* data)
{
    auto st = m_db.prepare("SELECT x, y, w, h, data FROM patches WHERE id = ?");
    st.bind(1, patchId);
    if (!st.step())
        return false;
    if (rect)
        *rect = QRect(st.i32(0), st.i32(1), st.i32(2), st.i32(3));
    if (data)
        *data = st.blob(4);
    return true;
}

qint64 Batch::dataVersion()
{
    auto st = m_db.prepare("PRAGMA data_version");
    st.step();
    return st.i64(0);
}

// ---- state -----------------------------------------------------------------

QString Batch::state(const QString& key, const QString& def)
{
    auto st = m_db.prepare("SELECT value FROM state WHERE key = ?");
    st.bind(1, key);
    if (st.step())
        return st.str(0);
    return def;
}

void Batch::setState(const QString& key, const QString& value)
{
    auto st = m_db.prepare("INSERT OR REPLACE INTO state(key, value) VALUES (?, ?)");
    st.bind(1, key).bind(2, value).exec();
}

} // namespace silma
