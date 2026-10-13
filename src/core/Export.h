#pragma once

#include "Batch.h"
#include "Develop.h"
#include "Heal.h"
#include "Image.h"

#include <QMap>
#include <QString>
#include <functional>
#include <optional>

namespace silma {

enum class ExportSizeMode { Original, LongEdge, ShortEdge };
// Sharpening for the output size and medium (screen), after resizing; like Lightroom's.
enum class OutputSharpening { None, Low, Standard, High };

struct ExportOptions {
    QString folder;
    QString pattern = QStringLiteral("{date}-{name}"); // tokens: {date} {time} {name} {import} {seq} {camera}
    int quality = 90;
    ExportSizeMode sizeMode = ExportSizeMode::Original;
    int size = 0; // pixels for LongEdge/ShortEdge
    bool overwrite = false;
    OutputSharpening sharpening = OutputSharpening::Standard;
    // The output file, when chosen beforehand (see resolveExportPaths): written as is,
    // replacing a file that is there. Empty: from `pattern`, with `overwrite`.
    QString path;
};

// Expands the filename pattern (without extension) for a photo.
QString expandExportPattern(const QString& pattern, const PhotoInfo& p, const QString& importName, int seq);

// Where a photo goes: `folder`/`pattern`.jpg, before any renaming.
QString exportPlannedPath(const PhotoInfo& p, const QString& importName, int seq, const ExportOptions& o);

// What to do about an output file that exists.
enum class ExistingFileChoice { Overwrite, Rename, Skip, Cancel };
struct ExistingFileAnswer {
    ExistingFileChoice choice = ExistingFileChoice::Rename;
    bool applyToAll = false;
};
// Final output paths for the planned ones (same order; empty = skipped), or nullopt when the
// export is cancelled. A file that exists is asked about (`ask` gets the path and how many
// such files are left, this one included) until an answer applies to all; a path that an
// earlier photo of the same export takes gets the next free name ("name-2.jpg", ...).
std::optional<QStringList> resolveExportPaths(const QStringList& planned,
                                              const std::function<ExistingFileAnswer(const QString& path, int left)>& ask);

// Encodes an sRGB-encoded float image to a JPEG file with an sRGB ICC profile, copying
// EXIF from `metadataSource` (the raw). Throws std::runtime_error on failure.
void writeJpeg(const QString& path, const ImageF& encoded, int quality, const QString& metadataSource, const QString& lens = {});

// Output sharpening of an sRGB-encoded image at its final size.
void outputSharpen(ImageF& encoded, OutputSharpening level);
float outputSharpeningAmount(OutputSharpening level);

// Renders and writes one photo; returns the written path. Problems with the result that do
// not stop the export (empty borders in the crop, a camera profile that is not installed) are
// added to `warnings`.
QString exportPhoto(const PhotoInfo& p, const DevelopSettings& s, const QString& importName, int seq, const ExportOptions& o,
                    const QMap<qint64, HealPatch>& patches = {}, QStringList* warnings = nullptr);

} // namespace silma
