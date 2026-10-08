#include "DngWriter.h"

#include <QFile>
#include <QFileInfo>
#include <cmath>
#include <cstring>
#include <set>
#include <exiv2/exiv2.hpp>
#include <stdexcept>
#include <tiffio.h>
#include <vector>

namespace silma {

namespace {

constexpr ttag_t kTagOpcodeList3 = 51022;
constexpr ttag_t kTagForwardMatrix1 = 50964;

const TIFFFieldInfo kExtraFields[] = {
    {kTagOpcodeList3, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_UNDEFINED, FIELD_CUSTOM, 1, 1, const_cast<char*>("OpcodeList3")},
};

// Sets a rational-valued field, passing float or double as libtiff expects.
void setRational(TIFF* tif, ttag_t tag, double v)
{
    const TIFFField* f = TIFFFieldWithTag(tif, tag);
    if (f && TIFFFieldSetGetSize(f) == 8)
        TIFFSetField(tif, tag, v);
    else
        TIFFSetField(tif, tag, float(v));
}

void setMatrix(TIFF* tif, ttag_t tag, const Mat3& m)
{
    float f[9];
    for (int i = 0; i < 9; ++i)
        f[i] = float(m[i / 3][i % 3]);
    TIFFSetField(tif, tag, uint16_t(9), f);
}

void copyExif(const LinearDngInfo& info, const QString& path)
{
    if (info.exifSource.isEmpty())
        return;
    try {
        auto src = Exiv2::ImageFactory::open(info.exifSource.toStdString());
        src->readMetadata();
        auto dst = Exiv2::ImageFactory::open(path.toStdString());
        dst->readMetadata();
        Exiv2::ExifData& out = dst->exifData();
        for (const auto& md : src->exifData()) {
            const std::string g = md.groupName(), k = md.key();
            static const std::set<std::string> lensKeys = {"Exif.Photo.LensModel", "Exif.Photo.LensSpecification", "Exif.Photo.LensMake",
                                                           "Exif.Photo.FocalLength", "Exif.Photo.FocalLengthIn35mmFilm", "Exif.Photo.LensSerialNumber"};
            if (info.lensCorrected && lensKeys.count(k))
                continue;
            if ((g == "Photo" || g == "GPSInfo") && k != "Exif.Photo.MakerNote" && md.size() <= 4096)
                out[k] = md.value();
            else if (k == "Exif.Image.DateTimeOriginal" || k == "Exif.Image.Artist" || k == "Exif.Image.Copyright")
                out[k] = md.value();
        }
        if (!info.lens.isEmpty() && !info.lensCorrected)
            out["Exif.Photo.LensModel"] = info.lens.toStdString();
        dst->writeMetadata();
    } catch (const Exiv2::Error&) {
        // Best effort.
    }
}

// Rounds a float to 12 mantissa bits: ~0.01% relative precision (far beyond 16-bit
// integer precision in the shadows) and much better deflate compression.
inline float quantizeMantissa(float v)
{
    uint32_t u;
    std::memcpy(&u, &v, 4);
    u = (u + 0x400u) & ~0x7FFu;
    std::memcpy(&v, &u, 4);
    return v;
}

} // namespace

ImageF unWhiteBalance(const ImageF& wb, const RawColorInfo& c)
{
    ImageF out(wb.width, wb.height);
    const float inv[3] = {float(1.0 / c.appliedMul[0]), float(1.0 / c.appliedMul[1]), float(1.0 / c.appliedMul[2])};
#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < wb.pixelCount(); ++i)
        for (int k = 0; k < 3; ++k)
            out.data[i * 3 + k] = wb.data[i * 3 + k] * inv[k];
    return out;
}

LinearDngInfo dngInfoFromRaw(const RawImage& raw, const QString& sourcePath, const QString& lens)
{
    LinearDngInfo i;
    i.make = raw.make;
    i.model = raw.model;
    i.color = raw.color;
    i.orientation = raw.orientation;
    i.opcodeList3 = raw.opcodes.list3;
    i.baselineExposure = raw.color.baselineExposure;
    i.exifSource = sourcePath;
    i.lens = lens;
    i.software = QStringLiteral("Silmä " SILMA_VERSION);
    return i;
}

void writeLinearDng(const QString& path, const ImageF& img, const LinearDngInfo& info, bool floatingPoint)
{
    QString tmp = path + ".tmp";
    TIFF* tif = TIFFOpen(QFile::encodeName(tmp).constData(), "w");
    if (!tif)
        throw std::runtime_error(("Cannot create " + path).toStdString());
    TIFFMergeFieldInfo(tif, kExtraFields, 1);
    const uint8_t dngVersion[4] = {1, 4, 0, 0}, backVersion[4] = {1, 1, 0, 0};
    const bool withMask = info.transparencyMask.size() == img.pixelCount();
    if (withMask) {
        // The mask goes into a SubIFD, written after the main image's directory.
        uint64_t subIfd[1] = {0};
        TIFFSetField(tif, TIFFTAG_SUBIFD, uint16_t(1), subIfd);
    }
    TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, uint32_t(img.width));
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, uint32_t(img.height));
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t(3));
    // 32-bit floating point with deflate (LibRaw only reads deflate DNGs as floats, and not
    // half floats); keeps headroom above 1.0 for HDR merges.
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t(32));
    TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, uint16_t(34892)); // LinearRaw
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_FLOATINGPOINT);
    // DNG readers expect tiles for compressed data.
    TIFFSetField(tif, TIFFTAG_TILEWIDTH, uint32_t(256));
    TIFFSetField(tif, TIFFTAG_TILELENGTH, uint32_t(256));
    TIFFSetField(tif, TIFFTAG_ORIENTATION, uint16_t(info.orientation));
    TIFFSetField(tif, TIFFTAG_MAKE, info.make.toUtf8().constData());
    TIFFSetField(tif, TIFFTAG_MODEL, info.model.toUtf8().constData());
    TIFFSetField(tif, TIFFTAG_SOFTWARE, info.software.toUtf8().constData());
    TIFFSetField(tif, TIFFTAG_DNGVERSION, dngVersion);
    TIFFSetField(tif, TIFFTAG_DNGBACKWARDVERSION, backVersion);
    TIFFSetField(tif, TIFFTAG_UNIQUECAMERAMODEL, (info.make + ' ' + info.model).toUtf8().constData());

    // Colour: dual-illuminant matrices when the source had them, else the D65 matrix.
    const auto& c = info.color;
    if (c.dng[0].valid && c.dng[1].valid) {
        setMatrix(tif, TIFFTAG_COLORMATRIX1, c.dng[0].colorMatrix);
        setMatrix(tif, TIFFTAG_COLORMATRIX2, c.dng[1].colorMatrix);
        TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT1, uint16_t(c.dng[0].illuminant));
        TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT2, uint16_t(c.dng[1].illuminant));
    } else {
        Mat3 m = c.xyzToCam;
        if (c.dng[1].valid)
            m = c.dng[1].colorMatrix;
        else if (c.dng[0].valid)
            m = c.dng[0].colorMatrix;
        setMatrix(tif, TIFFTAG_COLORMATRIX1, m);
        TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT1, uint16_t(21));
    }
    float neutral[3] = {float(1.0 / c.asShotMul[0]), float(1.0 / c.asShotMul[1]), float(1.0 / c.asShotMul[2])};
    TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, uint16_t(3), neutral);
    setRational(tif, TIFFTAG_BASELINEEXPOSURE, info.baselineExposure);
    (void)floatingPoint;
    if (!info.opcodeList3.isEmpty() && !info.lensCorrected)
        TIFFSetField(tif, kTagOpcodeList3, uint32_t(info.opcodeList3.size()), info.opcodeList3.constData());

    const float scale = 1.0f / info.whiteScale;
    const int tw = 256, th = 256;
    std::vector<float> tile(size_t(tw) * th * 3);
    bool ok = true;
    for (int ty = 0; ty < img.height && ok; ty += th)
        for (int tx = 0; tx < img.width && ok; tx += tw) {
            std::fill(tile.begin(), tile.end(), 0.0f);
            for (int y = 0; y < th && ty + y < img.height; ++y) {
                const float* p = img.px(tx, ty + y);
                float* d = tile.data() + size_t(y) * tw * 3;
                int n = std::min(tw, img.width - tx) * 3;
                for (int i = 0; i < n; ++i)
                    d[i] = quantizeMantissa(std::max(p[i] * scale, 0.0f));
            }
            ok = TIFFWriteTile(tif, tile.data(), uint32_t(tx), uint32_t(ty), 0, 0) >= 0;
        }
    if (ok && withMask) {
        ok = TIFFWriteDirectory(tif);
        TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 4); // transparency mask of the main image
        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, uint32_t(img.width));
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, uint32_t(img.height));
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, uint16_t(1));
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, uint16_t(8));
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, uint16_t(32895)); // TransparencyMask
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
        TIFFSetField(tif, TIFFTAG_TILEWIDTH, uint32_t(tw));
        TIFFSetField(tif, TIFFTAG_TILELENGTH, uint32_t(th));
        std::vector<uint8_t> mtile(size_t(tw) * th);
        for (int ty = 0; ty < img.height && ok; ty += th)
            for (int tx = 0; tx < img.width && ok; tx += tw) {
                std::fill(mtile.begin(), mtile.end(), uint8_t(0));
                for (int y = 0; y < th && ty + y < img.height; ++y)
                    std::copy_n(&info.transparencyMask[size_t(ty + y) * img.width + tx], std::min(tw, img.width - tx), &mtile[size_t(y) * tw]);
                ok = TIFFWriteTile(tif, mtile.data(), uint32_t(tx), uint32_t(ty), 0, 0) >= 0;
            }
    }
    TIFFClose(tif);
    if (!ok) {
        QFile::remove(tmp);
        throw std::runtime_error(("Error writing " + path).toStdString());
    }
    QFile::remove(path);
    if (!QFile::rename(tmp, path))
        throw std::runtime_error(("Cannot write " + path).toStdString());
    copyExif(info, path);
}

} // namespace silma
