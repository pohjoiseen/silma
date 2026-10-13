#include "Export.h"

#include "Engine.h"
#include "Filters.h"
#include "Heal.h"
#include "Upright.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
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

float outputSharpeningAmount(OutputSharpening level)
{
    switch (level) {
    case OutputSharpening::None: return 0.0f;
    case OutputSharpening::Low: return 0.4f;
    case OutputSharpening::Standard: return 0.75f;
    case OutputSharpening::High: return 1.2f;
    }
    return 0.0f;
}

void outputSharpen(ImageF& img, OutputSharpening level)
{
    outputSharpen(img, outputSharpeningAmount(level));
}

QString exportPhoto(const PhotoInfo& p, const DevelopSettings& s, const QString& importName, int seq, const ExportOptions& o,
                    const QMap<qint64, HealPatch>& patches, QStringList* warnings)
{
    DevelopEngine engine(p.path);
    addHealPatches(engine, patches);
    if (warnings) {
        // Black edges of a pixel or so are invisible (constrained crops can end a fraction of a
        // pixel past the image); from 2 pixels on they are reported.
        if (const EmptyCrop e = emptyCropBorders(engine.geometry(s, true), s.crop); e.depthPx >= 2 || e.fraction >= 0.005) {
            const QString amount = e.fraction >= 0.005 ? QStringLiteral("about %1% of the image").arg(e.fraction * 100, 0, 'f', e.fraction < 0.1 ? 1 : 0)
                                                       : QStringLiteral("up to %1 px wide along the edge").arg(std::lround(e.depthPx));
            *warnings << p.fileName + QStringLiteral(": empty (black) borders in the crop, ") + amount;
        }
        if (!s.profile.isEmpty() && !engine.raw().rendered && !engine.profile(s).valid())
            *warnings << p.fileName + QStringLiteral(": camera profile \"%1\" is not installed, exported with Silmä's own colour").arg(s.profile);
    }
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
        // Render at full size (up to 4x the target), so the develop sharpening works at the
        // scale it is set for, then Lanczos-downscale in linear light.
        int w4 = std::min(full.width(), w * 4), h4 = std::min(full.height(), h * 4);
        ImageF big = engine.renderEncoded(s, {.width = w4, .height = h4, .highQuality = true});
#pragma omp parallel for schedule(static)
        for (size_t i = 0; i < big.data.size(); ++i) {
            const float v = big.data[i];
            big.data[i] = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
        }
        img = resizeLanczos(big, w, h);
#pragma omp parallel for schedule(static)
        for (size_t i = 0; i < img.data.size(); ++i) {
            const float v = std::clamp(img.data[i], 0.0f, 1.0f); // Lanczos overshoot
            img.data[i] = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        }
    }
    outputSharpen(img, o.sharpening);

    QString path = o.path;
    if (path.isEmpty()) {
        const QString base = expandExportPattern(o.pattern, p, importName, seq);
        path = QDir(o.folder).filePath(base + ".jpg");
        if (!o.overwrite)
            for (int i = 2; QFileInfo::exists(path); ++i)
                path = QDir(o.folder).filePath(QStringLiteral("%1-%2.jpg").arg(base).arg(i));
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    writeJpeg(path, img, o.quality, p.path, p.lens);
    return path;
}

QString exportPlannedPath(const PhotoInfo& p, const QString& importName, int seq, const ExportOptions& o)
{
    return QDir(o.folder).filePath(expandExportPattern(o.pattern, p, importName, seq) + ".jpg");
}

std::optional<QStringList> resolveExportPaths(const QStringList& planned,
                                              const std::function<ExistingFileAnswer(const QString& path, int left)>& ask)
{
    QStringList out;
    QSet<QString> taken; // by earlier photos of this export
    auto key = [](const QString& path) { return QFileInfo(path).absoluteFilePath(); };
    auto freeName = [&](const QString& path) {
        const QFileInfo fi(path);
        const QString base = fi.dir().filePath(fi.completeBaseName());
        QString p = path;
        for (int i = 2; QFileInfo::exists(p) || taken.contains(key(p)); ++i)
            p = QStringLiteral("%1-%2.%3").arg(base).arg(i).arg(fi.suffix());
        return p;
    };
    int left = 0;
    for (const QString& p : planned)
        left += QFileInfo::exists(p);
    std::optional<ExistingFileChoice> forAll;
    for (const QString& p : planned) {
        QString path = p;
        if (QFileInfo::exists(p)) {
            ExistingFileChoice c;
            if (forAll) {
                c = *forAll;
            } else {
                const ExistingFileAnswer a = ask(p, left);
                c = a.choice;
                if (a.applyToAll)
                    forAll = c;
            }
            --left;
            if (c == ExistingFileChoice::Cancel)
                return std::nullopt;
            if (c == ExistingFileChoice::Skip) {
                out << QString();
                continue;
            }
            if (c == ExistingFileChoice::Rename || taken.contains(key(p)))
                path = freeName(p);
        } else if (taken.contains(key(p))) {
            path = freeName(p);
        }
        taken.insert(key(path));
        out << path;
    }
    return out;
}

} // namespace silma
