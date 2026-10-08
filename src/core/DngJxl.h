#pragma once
// JPEG XL compressed DNGs (DNG 1.7, written by recent Lightroom versions). LibRaw cannot
// unpack them without Adobe's DNG SDK, so the image data is decoded here with libjxl;
// LibRaw still provides the color metadata.

#include <QByteArray>
#include <QImage>
#include <vector>

namespace silma {

struct DngJxlImage {
    int width = 0, height = 0;
    // Interleaved RGB, black-subtracted and scaled so 1.0 = WhiteLevel (camera space, not white-balanced).
    std::vector<float> data;
};

// True if the main (full-resolution) image of the DNG is JPEG XL compressed.
bool isJxlDng(const QByteArray& file);

// Decodes the main image of a JPEG XL DNG holding 3-channel linear raw data. Throws
// std::runtime_error when it is not one (e.g. a JPEG XL compressed CFA image).
DngJxlImage decodeDngJxl(const QByteArray& file);

// Largest JPEG XL compressed RGB preview in the DNG, or a null image.
QImage decodeDngJxlPreview(const QByteArray& file);

} // namespace silma
