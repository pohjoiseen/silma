#include "Export.h"

#include "Engine.h"
#include "Heal.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <cstdio>
#include <exiv2/exiv2.hpp>
#include <jpeglib.h>
#include <lcms2.h>
#include <set>
#include <stdexcept>

namespace silma {

namespace {

QByteArray srgbIccProfile()
{
    static QByteArray icc = [] {
        cmsHPROFILE p = cmsCreate_sRGBProfile();
        cmsUInt32Number n = 0;
        cmsSaveProfileToMem(p, nullptr, &n);
        QByteArray buf(int(n), 0);
        cmsSaveProfileToMem(p, buf.data(), &n);
        cmsCloseProfile(p);
        return buf;
    }();
    return icc;
}

QByteArray encodeJpeg(const ImageF& img, int quality)
{
    jpeg_compress_struct cinfo;
    jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = [](j_common_ptr c) {
        char msg[JMSG_LENGTH_MAX];
        (*c->err->format_message)(c, msg);
        throw std::runtime_error(std::string("JPEG encoding failed: ") + msg);
    };
    jpeg_create_compress(&cinfo);
    unsigned char* mem = nullptr;
    unsigned long memSize = 0;
    QByteArray out;
    try {
        jpeg_mem_dest(&cinfo, &mem, &memSize);
        cinfo.image_width = img.width;
        cinfo.image_height = img.height;
        cinfo.input_components = 3;
        cinfo.in_color_space = JCS_RGB;
        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, std::clamp(quality, 1, 100), TRUE);
        cinfo.optimize_coding = TRUE;
        if (quality >= 90) {
            // No chroma subsampling at high quality.
            for (int i = 0; i < 3; ++i)
                cinfo.comp_info[i].h_samp_factor = cinfo.comp_info[i].v_samp_factor = 1;
        }
        jpeg_start_compress(&cinfo, TRUE);
        QByteArray icc = srgbIccProfile();
        jpeg_write_icc_profile(&cinfo, reinterpret_cast<const JOCTET*>(icc.constData()), unsigned(icc.size()));
        std::vector<unsigned char> row(size_t(img.width) * 3);
        while (cinfo.next_scanline < cinfo.image_height) {
            const float* p = img.row(int(cinfo.next_scanline));
            for (int x = 0; x < img.width * 3; ++x)
                row[x] = (unsigned char)(std::clamp(int(p[x] * 255.0f + 0.5f), 0, 255));
            JSAMPROW r = row.data();
            jpeg_write_scanlines(&cinfo, &r, 1);
        }
        jpeg_finish_compress(&cinfo);
        out = QByteArray(reinterpret_cast<const char*>(mem), qsizetype(memSize));
    } catch (...) {
        jpeg_destroy_compress(&cinfo);
        free(mem);
        throw;
    }
    jpeg_destroy_compress(&cinfo);
    free(mem);
    return out;
}

void copyMetadata(const QString& from, const QString& to, int width, int height, const QString& lens)
{
    try {
        auto src = Exiv2::ImageFactory::open(from.toStdString());
        src->readMetadata();
        Exiv2::ExifData out;
        // Keep standard tags only: maker notes, sub-images and DNG tags reference raw data.
        static const std::set<std::string> imageKeys = {
            "Exif.Image.Make", "Exif.Image.Model", "Exif.Image.DateTime", "Exif.Image.Artist",
            "Exif.Image.Copyright", "Exif.Image.ImageDescription", "Exif.Image.DateTimeOriginal"};
        for (const auto& md : src->exifData()) {
            const std::string g = md.groupName(), k = md.key();
            bool keep = (g == "Image" && imageKeys.count(k)) || ((g == "Photo" || g == "GPSInfo") && k != "Exif.Photo.MakerNote" && md.size() <= 4096);
            if (keep)
                out.add(md);
        }
        out["Exif.Image.Orientation"] = uint16_t(1);
        out["Exif.Image.Software"] = std::string("Silmä " SILMA_VERSION);
        out["Exif.Photo.PixelXDimension"] = uint32_t(width);
        out["Exif.Photo.PixelYDimension"] = uint32_t(height);
        out["Exif.Photo.ColorSpace"] = uint16_t(1);
        if (!lens.isEmpty() && out.findKey(Exiv2::ExifKey("Exif.Photo.LensModel")) == out.end())
            out["Exif.Photo.LensModel"] = lens.toStdString();
        auto dst = Exiv2::ImageFactory::open(to.toStdString());
        dst->readMetadata();
        dst->setExifData(out);
        dst->writeMetadata();
    } catch (const Exiv2::Error&) {
        // Metadata is best effort; the image itself is already written.
    }
}

} // namespace

QString expandExportPattern(const QString& pattern, const PhotoInfo& p, const QString& importName, int seq)
{
    QString s = pattern;
    QDateTime t = p.captureTime.isValid() ? p.captureTime : QFileInfo(p.path).lastModified();
    s.replace(QLatin1String("{date}"), t.toString(QStringLiteral("yyyyMMdd")));
    s.replace(QLatin1String("{time}"), t.toString(QStringLiteral("HHmmss")));
    s.replace(QLatin1String("{name}"), QFileInfo(p.fileName).completeBaseName());
    s.replace(QLatin1String("{import}"), importName);
    s.replace(QLatin1String("{seq}"), QStringLiteral("%1").arg(seq, 4, 10, QLatin1Char('0')));
    s.replace(QLatin1String("{camera}"), p.model);
    // Keep file names portable.
    for (QChar& c : s)
        if (c == '/' || c == '\\' || c == ':' || c.unicode() < 32)
            c = '_';
    return s;
}

void writeJpeg(const QString& path, const ImageF& encoded, int quality, const QString& metadataSource, const QString& lens)
{
    QByteArray jpeg = encodeJpeg(encoded, quality);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(jpeg) != jpeg.size() || !f.commit())
        throw std::runtime_error(("Cannot write " + path).toStdString());
    if (!metadataSource.isEmpty())
        copyMetadata(metadataSource, path, encoded.width, encoded.height, lens);
}

QString exportPhoto(const PhotoInfo& p, const DevelopSettings& s, const QString& importName, int seq, const ExportOptions& o,
                    const QMap<qint64, HealPatch>& patches)
{
    DevelopEngine engine(p.path);
    addHealPatches(engine, patches);
    QSize full = engine.outputSize(s);
    int w = full.width(), h = full.height();
    if (o.sizeMode != ExportSizeMode::Original && o.size > 0) {
        int edge = o.sizeMode == ExportSizeMode::LongEdge ? std::max(w, h) : std::min(w, h);
        if (o.size < edge) {
            double k = double(o.size) / edge;
            w = std::max(1, int(std::round(w * k)));
            h = std::max(1, int(std::round(h * k)));
        }
    }
    ImageF img;
    if (w == full.width()) {
        img = engine.renderEncoded(s, {.width = w, .height = h, .highQuality = true});
    } else {
        // Render at up to twice the target size, then area-downscale: avoids aliasing.
        int w2 = std::min(full.width(), w * 2), h2 = std::min(full.height(), h * 2);
        ImageF big = engine.renderEncoded(s, {.width = w2, .height = h2, .highQuality = true});
        // Downscale in linear light.
        for (float& v : big.data)
            v = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        img = resize(big, w, h);
        for (float& v : img.data)
            v = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    }

    QDir().mkpath(o.folder);
    QString base = expandExportPattern(o.pattern, p, importName, seq);
    QString path = QDir(o.folder).filePath(base + ".jpg");
    if (!o.overwrite)
        for (int i = 2; QFileInfo::exists(path); ++i)
            path = QDir(o.folder).filePath(QStringLiteral("%1-%2.jpg").arg(base).arg(i));
    writeJpeg(path, img, o.quality, p.path, p.lens);
    return path;
}

} // namespace silma
