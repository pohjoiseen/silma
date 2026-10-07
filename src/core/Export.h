#pragma once

#include "Batch.h"
#include "Develop.h"
#include "Image.h"

#include <QString>
#include <functional>

namespace silma {

enum class ExportSizeMode { Original, LongEdge, ShortEdge };

struct ExportOptions {
    QString folder;
    QString pattern = QStringLiteral("{date}-{name}"); // tokens: {date} {time} {name} {import} {seq} {camera}
    int quality = 90;
    ExportSizeMode sizeMode = ExportSizeMode::Original;
    int size = 0; // pixels for LongEdge/ShortEdge
    bool overwrite = false;
};

// Expands the filename pattern (without extension) for a photo.
QString expandExportPattern(const QString& pattern, const PhotoInfo& p, const QString& importName, int seq);

// Encodes an sRGB-encoded float image to a JPEG file with an sRGB ICC profile, copying
// EXIF from `metadataSource` (the raw). Throws std::runtime_error on failure.
void writeJpeg(const QString& path, const ImageF& encoded, int quality, const QString& metadataSource, const QString& lens = {});

// Renders and writes one photo; returns the written path.
QString exportPhoto(const PhotoInfo& p, const DevelopSettings& s, const QString& importName, int seq, const ExportOptions& o);

} // namespace silma
