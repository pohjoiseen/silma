#pragma once

#include "Image.h"

#include <QByteArray>
#include <QString>
#include <array>
#include <memory>
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
    // Linear camera RGB, white-balanced with color.appliedMul; channel c clips at
    // color.appliedMul[c] (the smallest multiplier, normally green's, is 1). Values up to 1 are
    // the same as when everything was clipped at 1; brighter ones keep the unclipped channels.
    ImageF image;
    RawColorInfo color;
    DngOpcodes opcodes;
    QString make, model, lens;
    int orientation = 1; // EXIF orientation, NOT applied to `image`
    int fullWidth = 0, fullHeight = 0; // dimensions of the full-resolution decode
    bool halfSize = false;
    bool isDng = false;
    bool rendered = false; // a JPEG/HEIC (see decodeRendered)
    int hotPixels = 0; // hot/stuck sensor pixels found and removed
    // DNG DefaultCrop in full-resolution pixel coordinates (or whole image).
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    // Parts of the image holding data (panorama coverage); null = all of it.
    std::shared_ptr<const ValidMask> valid;
};

// The undemosaiced data of a Bayer sensor (visible area), for models that demosaic themselves.
struct BayerMosaic {
    int width = 0, height = 0;
    std::vector<float> data; // 0 = black level, 1 = white level (not white-balanced, not clipped below 1)
    int color[2][2] = {};    // colour at (y % 2, x % 2): 0 red, 1 green, 2 blue
};

struct RawDecodeOptions {
    bool halfSize = false; // fast 2x downscaled decode without demosaicing
};

// Throws std::runtime_error on failure.
RawImage decodeRaw(const QString& path, const RawDecodeOptions& opts = {});
// The Bayer mosaic of a raw file; empty (width 0) when the sensor is not a 2x2 RGB Bayer
// one (X-Trans, Foveon, monochrome, linear DNGs, ...). Throws on decoding errors.
BayerMosaic decodeBayerMosaic(const QString& path);
// Replaces hot and stuck pixels in a Bayer mosaic (raw sample values, `pitch` samples per
// row) with the median of their own colour's neighbours. `color`: 0 red, 1 green, 2 blue at
// (y % 2, x % 2); `mul`: white balance multipliers. Returns how many were replaced.
int removeHotPixels(quint16* data, int width, int height, int pitch, const int color[2][2], const float black[2][2], float white,
                    const std::array<double, 3>& mul);
// Largest embedded JPEG preview, or empty if none.
QByteArray extractEmbeddedPreview(const QString& path);
// True for file extensions we consider importable raw files.
bool isRawFileName(const QString& fileName);
QStringList rawFileNameFilters();

} // namespace silma
