#pragma once

#include "Image.h"

#include <QByteArray>
#include <QString>
#include <array>
#include <vector>

namespace silma {

using Mat3 = std::array<std::array<double, 3>, 3>;

struct DngColorMatrix {
    int illuminant = 0; // EXIF LightSource code (17 = A, 21 = D65, ...)
    Mat3 colorMatrix{}; // XYZ -> camera
    bool valid = false;
};

struct RawColorInfo {
    // White-balance multipliers already applied to the decoded data (green = 1).
    std::array<double, 3> appliedMul{1, 1, 1};
    // As-shot white balance multipliers (green = 1).
    std::array<double, 3> asShotMul{1, 1, 1};
    // Daylight multipliers from the camera's color data (green = 1).
    std::array<double, 3> daylightMul{1, 1, 1};
    // XYZ(D65) -> camera matrix as provided by LibRaw (Adobe coefficients or DNG).
    Mat3 xyzToCam{};
    // DNG dual-illuminant matrices when available.
    DngColorMatrix dng[2];
    double baselineExposure = 0; // EV, from DNG
};

// Raw DNG opcode lists (big-endian blobs as stored in the file), applied by the lens module.
struct DngOpcodes {
    QByteArray list1, list2, list3;
};

struct RawImage {
    // Linear camera RGB, white-balanced with color.appliedMul, normalized so 1.0 = clip.
    ImageF image;
    RawColorInfo color;
    DngOpcodes opcodes;
    QString make, model, lens;
    int orientation = 1; // EXIF orientation, NOT applied to `image`
    int fullWidth = 0, fullHeight = 0; // dimensions of the full-resolution decode
    bool halfSize = false;
    bool isDng = false;
    // DNG DefaultCrop in full-resolution pixel coordinates (or whole image).
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
};

struct RawDecodeOptions {
    bool halfSize = false; // fast 2x downscaled decode without demosaicing
};

// Throws std::runtime_error on failure.
RawImage decodeRaw(const QString& path, const RawDecodeOptions& opts = {});
// Largest embedded JPEG preview, or empty if none.
QByteArray extractEmbeddedPreview(const QString& path);
// True for file extensions we consider importable raw files.
bool isRawFileName(const QString& fileName);
QStringList rawFileNameFilters();

} // namespace silma
