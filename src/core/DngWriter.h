#pragma once
// Writes linear (demosaiced, "LinearRaw") DNG files: used for AI-denoised, HDR and
// panorama results so they can be developed like any other raw.

#include "Image.h"
#include "RawDecoder.h"

#include <QString>

namespace silma {

struct LinearDngInfo {
    QString make, model;
    RawColorInfo color;        // colour matrices and as-shot white balance of the source
    int orientation = 1;       // EXIF orientation of the pixel data
    QByteArray opcodeList3;    // copied lens corrections (DNG sources)
    double baselineExposure = 0;
    QString exifSource;        // file to copy EXIF (exposure, lens, GPS, dates) from
    QString lens;              // lens name written as Exif.Photo.LensModel
    QString software;
    float whiteScale = 1.0f;   // values are divided by this before writing (HDR headroom)
    bool lensCorrected = false; // pixels already lens-corrected: omit lens metadata/opcodes
};

// `cameraLinear`: camera-native linear RGB (not white balanced), 0..1 (clipped above).
// Floating point (HDR) output when `floatingPoint` is set: values may exceed 1.
void writeLinearDng(const QString& path, const ImageF& cameraLinear, const LinearDngInfo& info, bool floatingPoint = false);

// Convenience: build info from a decoded raw.
LinearDngInfo dngInfoFromRaw(const RawImage& raw, const QString& sourcePath, const QString& lens);

// Undo the as-decoded white balance (RawImage::image is white balanced with appliedMul).
ImageF unWhiteBalance(const ImageF& wb, const RawColorInfo& c);

} // namespace silma
