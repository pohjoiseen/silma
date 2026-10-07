#pragma once
// A batch is a sqlite catalog (a *.silma file). It holds imports (folders,
// by default sub-directories next to the batch file) which hold photos.
// All develop settings, history and UI state live in the batch.

#include "Sqlite.h"

#include <QDateTime>
#include <QDir>
#include <QList>
#include <QString>
#include <optional>

namespace silma {

struct ImportInfo {
    qint64 id = 0;
    QString name;
    QString path; // absolute folder path
    QDateTime created;
    int photoCount = 0;
    int rejectedCount = 0;
};

enum class PhotoKind { Raw = 0, DerivedDng = 1 };

struct PhotoInfo {
    qint64 id = 0;
    qint64 importId = 0;
    QString fileName;
    QString path; // absolute file path
    qint64 fileSize = 0;
    QDateTime captureTime;
    QString make, model, lens;
    int width = 0, height = 0; // sensor image size as decoded (before orientation)
    int orientation = 1;       // EXIF orientation
    double iso = 0, exposureTime = 0, fNumber = 0, focalLength = 0;
    bool rejected = false;
    PhotoKind kind = PhotoKind::Raw;
    QString derivedFrom; // JSON list of source photo ids for merged/denoised DNGs
    int historyPos = 0;
};

struct HistoryEntry {
    int seq = 0;
    QString label;
    QString settings; // JSON
    QDateTime time;
};

class Batch {
public:
    static constexpr int kSchemaVersion = 1;
    static constexpr const char* kExtension = "silma";

    Batch() = default;
    // Creates a new batch file (fails if it exists).
    void create(const QString& file);
    // Opens an existing batch, migrating the schema if it is older.
    void open(const QString& file);
    void close();
    bool isOpen() const { return m_db.isOpen(); }

    QString file() const { return m_file; }
    QDir root() const { return m_root; }
    QString name() const;
    Database& db() { return m_db; }

    // Imports
    QList<ImportInfo> imports();
    std::optional<ImportInfo> importById(qint64 id);
    std::optional<ImportInfo> importByName(const QString& name);
    // Creates the import record and its folder (relative names resolve against root()).
    ImportInfo addImport(const QString& name, const QString& folder = {});
    void removeImport(qint64 id); // only the record; caller handles files

    // Photos
    QList<PhotoInfo> photos(std::optional<qint64> importId = {});
    std::optional<PhotoInfo> photo(qint64 id);
    std::optional<PhotoInfo> photoByPath(const QString& absPath);
    qint64 addPhoto(const PhotoInfo& p, const QString& initialSettings = QStringLiteral("{}"));
    void setRejected(qint64 photoId, bool rejected);
    void removePhoto(qint64 photoId);

    // Thumbnails (JPEG blobs)
    QByteArray thumbnail(qint64 photoId, int* version = nullptr);
    void setThumbnail(qint64 photoId, const QByteArray& jpeg, int version);

    // Develop settings & history. The current settings are history[historyPos].
    QString settings(qint64 photoId);
    QList<HistoryEntry> history(qint64 photoId);
    // Appends a new state after the current position, dropping any redo tail.
    int pushHistory(qint64 photoId, const QString& label, const QString& settings);
    // Moves the current position; returns the settings at the new position.
    QString setHistoryPos(qint64 photoId, int pos);
    int historyPos(qint64 photoId);

    // Arbitrary key/value state (UI session etc.)
    QString state(const QString& key, const QString& def = {});
    void setState(const QString& key, const QString& value);

    QString relativize(const QString& absPath) const;
    QString absolutize(const QString& stored) const;

private:
    void migrate();
    PhotoInfo readPhoto(Statement& st);

    Database m_db;
    QString m_file;
    QDir m_root;
};

QString photoKindName(PhotoKind k);

} // namespace silma
