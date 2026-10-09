#include "RenderedImage.h"

#include "Color.h"
#include "Engine.h"
#include "Metadata.h"

#include <QColorSpace>
#include <QFileInfo>
#include <QImageReader>
#include <libheif/heif.h>
#include <memory>
#include <stdexcept>

namespace silma {

namespace {

bool isHeifFileName(const QString& fileName)
{
    static const QStringList exts = {"heic", "heif", "hif"};
    return exts.contains(QFileInfo(fileName).suffix().toLower());
}

QColorSpace heifColorSpace(const heif_image_handle* h)
{
    const heif_color_profile_type type = heif_image_handle_get_color_profile_type(h);
    if (type == heif_color_profile_type_prof || type == heif_color_profile_type_rICC) {
        QByteArray icc(int(heif_image_handle_get_raw_color_profile_size(h)), Qt::Uninitialized);
        if (!icc.isEmpty() && heif_image_handle_get_raw_color_profile(h, icc.data()).code == heif_error_Ok) {
            QColorSpace cs = QColorSpace::fromIccProfile(icc);
            if (cs.isValid())
                return cs;
        }
    }
    heif_color_profile_nclx* nclx = nullptr;
    if (heif_image_handle_get_nclx_color_profile(h, &nclx).code != heif_error_Ok || !nclx)
        return QColorSpace::SRgb;
    QColorSpace::Primaries primaries = QColorSpace::Primaries::SRgb;
    if (nclx->color_primaries == heif_color_primaries_SMPTE_EG_432_1)
        primaries = QColorSpace::Primaries::DciP3D65; // iPhones: Display P3
    else if (nclx->color_primaries == heif_color_primaries_ITU_R_BT_2020_2_and_2100_0)
        primaries = QColorSpace::Primaries::Bt2020;
    QColorSpace::TransferFunction transfer = QColorSpace::TransferFunction::SRgb;
    if (nclx->transfer_characteristics == heif_transfer_characteristic_linear)
        transfer = QColorSpace::TransferFunction::Linear;
    else if (nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_709_5
             || nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_601_6
             || nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2020_2_10bit
             || nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2020_2_12bit)
        transfer = QColorSpace::TransferFunction::Bt2020;
    heif_nclx_color_profile_free(nclx);
    return QColorSpace(primaries, transfer);
}

QImage loadHeif(const QString& path)
{
    static const bool initialized = [] { return heif_init(nullptr).code == heif_error_Ok; }();
    (void)initialized;
    std::unique_ptr<heif_context, decltype(&heif_context_free)> ctx(heif_context_alloc(), heif_context_free);
    auto check = [&](heif_error e) {
        if (e.code != heif_error_Ok)
            throw std::runtime_error(("Cannot read " + path + ": " + QString::fromUtf8(e.message)).toStdString());
    };
    check(heif_context_read_from_file(ctx.get(), QFile::encodeName(path).constData(), nullptr));
    heif_image_handle* hp = nullptr;
    check(heif_context_get_primary_image_handle(ctx.get(), &hp));
    std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> handle(hp, heif_image_handle_release);
    // More than 8 bits (e.g. 10-bit HEICs) decode to 16 bits per channel. Rotation and
    // mirroring (irot/imir) are applied by libheif.
    const bool deep = heif_image_handle_get_luma_bits_per_pixel(hp) > 8;
    heif_image* ip = nullptr;
    check(heif_decode_image(hp, &ip, heif_colorspace_RGB, deep ? heif_chroma_interleaved_RRGGBB_LE : heif_chroma_interleaved_RGB, nullptr));
    std::unique_ptr<heif_image, decltype(&heif_image_release)> img(ip, heif_image_release);
    const int w = heif_image_get_width(ip, heif_channel_interleaved), h = heif_image_get_height(ip, heif_channel_interleaved);
    const int bits = heif_image_get_bits_per_pixel_range(ip, heif_channel_interleaved);
    size_t stride = 0;
    const uint8_t* data = heif_image_get_plane_readonly2(ip, heif_channel_interleaved, &stride);
    if (!data || w <= 0 || h <= 0)
        throw std::runtime_error(("Cannot decode " + path).toStdString());
    QImage out;
    if (!deep) {
        out = QImage(w, h, QImage::Format_RGB888);
        for (int y = 0; y < h; ++y)
            std::copy_n(data + size_t(y) * stride, size_t(w) * 3, out.scanLine(y));
    } else {
        out = QImage(w, h, QImage::Format_RGBX64);
        const int shift = 16 - std::clamp(bits, 9, 16);
        for (int y = 0; y < h; ++y) {
            const quint16* s = reinterpret_cast<const quint16*>(data + size_t(y) * stride);
            quint16* d = reinterpret_cast<quint16*>(out.scanLine(y));
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const quint16 v = quint16(s[x * 3 + c] << shift);
                    d[x * 4 + c] = quint16(v | (v >> (16 - shift))); // full 16-bit range
                }
                d[x * 4 + 3] = 0xffff;
            }
        }
    }
    out.setColorSpace(heifColorSpace(hp));
    return out;
}

} // namespace

bool isRenderedFileName(const QString& fileName)
{
    static const QStringList exts = {"jpg", "jpeg", "jpe", "heic", "heif", "hif"};
    return exts.contains(QFileInfo(fileName).suffix().toLower());
}

QStringList renderedFileNameFilters()
{
    return {"*.jpg", "*.JPG", "*.jpeg", "*.JPEG", "*.heic", "*.HEIC", "*.heif", "*.HEIF", "*.hif", "*.HIF"};
}

QImage loadRenderedImage(const QString& path)
{
    if (isHeifFileName(path))
        return loadHeif(path);
    QImageReader reader(path);
    reader.setAutoTransform(true); // EXIF orientation
    QImage img = reader.read();
    if (img.isNull())
        throw std::runtime_error(("Cannot read " + path + ": " + reader.errorString()).toStdString());
    return img;
}

RawImage decodeRendered(const QString& path, bool halfSize)
{
    QImage img = loadRenderedImage(path);
    QColorSpace cs = img.colorSpace();
    if (img.format() != QImage::Format_RGB888 && img.format() != QImage::Format_RGBX64) {
        // Greyscale, CMYK, palette, ...: plain RGB first (in sRGB unless the profile says otherwise).
        const bool rgbSpace = cs.isValid() && cs.colorModel() == QColorSpace::ColorModel::Rgb;
        img = img.convertToFormat(QImage::Format_RGB32);
        cs = rgbSpace ? cs : QColorSpace(QColorSpace::SRgb);
    }
    if (!cs.isValid())
        cs = QColorSpace::SRgb;
    img.setColorSpace(cs);
    const QImage lin = img.convertedToColorSpace(QColorSpace(QColorSpace::Primaries::Bt2020, QColorSpace::TransferFunction::Linear),
                                                 QImage::Format_RGBX32FPx4);
    if (lin.isNull())
        throw std::runtime_error(("Cannot convert the colours of " + path).toStdString());

    RawImage out;
    out.image = ImageF(lin.width(), lin.height());
#pragma omp parallel for schedule(static)
    for (int y = 0; y < lin.height(); ++y) {
        const float* s = reinterpret_cast<const float*>(lin.constScanLine(y));
        float* d = out.image.row(y);
        for (int x = 0; x < lin.width(); ++x)
            for (int c = 0; c < 3; ++c)
                d[x * 3 + c] = std::max(0.0f, s[x * 4 + c]);
    }
    undoDefaultLook(out.image);
    out.fullWidth = out.image.width;
    out.fullHeight = out.image.height;
    if (halfSize) {
        out.image = downscale(out.image, 2);
        out.halfSize = true;
    }
    out.cropW = out.fullWidth;
    out.cropH = out.fullHeight;
    out.rendered = true;
    // The "camera" is linear Rec.2020, already white-balanced.
    out.color.xyzToCam = matInverse(color::kRec2020ToXyz);
    try {
        const MetadataSummary m = readMetadataSummary(path);
        out.make = m.make;
        out.model = m.model;
        out.lens = m.lens;
    } catch (...) {
    }
    return out;
}

} // namespace silma
