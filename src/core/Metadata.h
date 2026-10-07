#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <optional>

namespace silma {

struct MetadataSummary {
    QDateTime captureTime;
    QString make, model, lens;
    double iso = 0, exposureTime = 0, fNumber = 0, focalLength = 0, focalLength35 = 0;
    int orientation = 1;
    int width = 0, height = 0;
    std::optional<double> latitude, longitude, altitude;
    QString software;
};

struct MetadataTag {
    QString group; // Exif.Image, Exif.Photo, Exif.Nikon3, Xmp.dc, ...
    QString key;   // full exiv2 key
    QString label; // human-readable tag label
    QString value; // interpreted (pretty-printed) value
};

// Reads the commonly needed fields. Throws std::runtime_error on unreadable files.
MetadataSummary readMetadataSummary(const QString& path);
// Every tag exiv2 can interpret, in file order. Large binary values are summarized.
QList<MetadataTag> readAllMetadata(const QString& path);

} // namespace silma
