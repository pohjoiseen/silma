#pragma once

#include "Batch.h"

#include <QStringList>
#include <functional>

namespace silma {

struct ImportOptions {
    // Target import name. Empty: name after the capture date (YYYY-MM-DD) of the files.
    QString importName;
    // When the name is automatic, put files into one import per capture date.
    bool splitByDate = false;
    // Move instead of copy.
    bool move = false;
};

struct ImportResult {
    QList<qint64> added;
    QStringList skipped; // duplicates
    QStringList errors;
    QStringList importNames;
};

using ProgressFn = std::function<void(int done, int total, const QString& file)>;

ImportResult importFiles(Batch& batch, const QStringList& files, const ImportOptions& opts, const ProgressFn& progress = {});

// Builds a thumbnail JPEG (long edge `maxSize`) from the embedded preview, oriented upright.
QByteArray thumbnailFromEmbeddedPreview(const QString& rawPath, int orientation, int maxSize = 512);

// Collects raw files from a list of files and/or directories (recursive).
QStringList collectRawFiles(const QStringList& paths);

} // namespace silma
