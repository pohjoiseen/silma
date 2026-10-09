#pragma once
// Photos that were already rendered by the camera or phone: JPEG and HEIC/HEIF. They are
// decoded so the develop pipeline treats them like raw data (see decodeRendered).

#include "RawDecoder.h"

#include <QImage>
#include <QString>
#include <QStringList>

namespace silma {

bool isRenderedFileName(const QString& fileName);
QStringList renderedFileNameFilters();

// The image upright (orientation applied), in its own colour space (set on the QImage).
// Throws std::runtime_error on failure.
QImage loadRenderedImage(const QString& path);

// Decodes a JPEG/HEIC like a raw file: the pixels in linear Rec.2020 with Silmä's default look
// undone (undoDefaultLook), so default settings reproduce the image and every control works
// as on raws. The "camera" colour space is linear Rec.2020, white-balanced (multipliers 1).
RawImage decodeRendered(const QString& path, bool halfSize = false);

} // namespace silma
