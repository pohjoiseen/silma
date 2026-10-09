#include "Importer.h"

#include "Develop.h"
#include "Metadata.h"
#include "RawDecoder.h"
#include "RenderedImage.h"

#include <QColorSpace>

#include <QBuffer>
#include <QDirIterator>
#include <QFileInfo>
#include <QImage>
#include <QTransform>
#include <QtConcurrent>
#include <map>

namespace silma {

namespace {

struct Probe {
    QString source;
    MetadataSummary meta;
    qint64 size = 0;
    QString error;
};

QString dateName(const QDateTime& t, const QString& fallbackFile)
{
    QDateTime d = t.isValid() ? t : QFileInfo(fallbackFile).lastModified();
    return d.toString(QStringLiteral("yyyy-MM-dd"));
}

QString uniqueDestination(const QDir& dir, const QString& fileName)
{
    QString candidate = dir.absoluteFilePath(fileName);
    if (!QFileInfo::exists(candidate))
        return candidate;
    QFileInfo fi(fileName);
    for (int i = 1;; ++i) {
        candidate = dir.absoluteFilePath(QStringLiteral("%1-%2.%3").arg(fi.completeBaseName()).arg(i).arg(fi.suffix()));
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
}

} // namespace

bool isPhotoFileName(const QString& fileName)
{
    return isRawFileName(fileName) || isRenderedFileName(fileName);
}

QStringList collectPhotoFiles(const QStringList& paths)
{
    QStringList out;
    for (const QString& p : paths) {
        QFileInfo fi(p);
        if (fi.isDir()) {
            QDirIterator it(p, QDir::Files, QDirIterator::Subdirectories);
            QStringList found;
            while (it.hasNext()) {
                QString f = it.next();
                if (isPhotoFileName(f))
                    found << f;
            }
            found.sort();
            out << found;
        } else if (fi.isFile()) {
            out << fi.absoluteFilePath();
        }
    }
    return out;
}

QByteArray thumbnailFromEmbeddedPreview(const QString& rawPath, int orientation, int maxSize)
{
    QImage img;
    if (isRenderedFileName(rawPath)) {
        img = loadRenderedImage(rawPath); // upright already
        if (img.colorSpace().isValid())
            img.convertToColorSpace(QColorSpace::SRgb);
        orientation = 1;
    }
    QByteArray jpeg = img.isNull() ? extractEmbeddedPreview(rawPath) : QByteArray();
    if (img.isNull() && (jpeg.isEmpty() || !img.loadFromData(jpeg, "JPEG"))) {
        // No usable preview: fall back to a fast half-size raw decode with a neutral look.
        RawImage r = decodeRaw(rawPath, {.halfSize = true});
        int f = std::max(1, std::max(r.image.width, r.image.height) / (maxSize * 2));
        ImageF small = downscale(r.image, f);
        img = QImage(small.width, small.height, QImage::Format_RGB888);
        for (int y = 0; y < small.height; ++y) {
            uchar* line = img.scanLine(y);
            const float* s = small.row(y);
            for (int x = 0; x < small.width * 3; ++x) {
                float v = std::clamp(s[x] * 2.0f, 0.0f, 1.0f);
                v = v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
                line[x] = uchar(v * 255.0f + 0.5f);
            }
        }
    }
    img = img.scaled(maxSize, maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // Some cameras store the preview already rotated; only rotate landscape previews.
    if (orientation >= 5 && img.width() > img.height())
        img = img.transformed(QTransform().rotate(orientation == 6 ? 90 : -90));
    else if (orientation == 3)
        img = img.transformed(QTransform().rotate(180));
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", 85);
    return out;
}

ImportResult importFiles(Batch& batch, const QStringList& files, const ImportOptions& opts, const ProgressFn& progress)
{
    ImportResult res;
    // RAW+JPEG pairs: the JPEG/HEIC next to a raw of the same name is the camera's sidecar.
    QSet<QString> rawBases;
    for (const QString& f : files)
        if (isRawFileName(f)) {
            const QFileInfo fi(f);
            rawBases.insert(fi.absolutePath() + '/' + fi.completeBaseName().toLower());
        }
    QStringList photos;
    for (const QString& f : files) {
        const QFileInfo fi(f);
        if (isRenderedFileName(f) && rawBases.contains(fi.absolutePath() + '/' + fi.completeBaseName().toLower()))
            res.sidecars << f;
        else
            photos << f;
    }
    const int total = int(photos.size());

    // Read metadata in parallel first: it decides the destination folders.
    QList<Probe> probes = QtConcurrent::blockingMapped(photos, [](const QString& f) {
        Probe p;
        p.source = f;
        QFileInfo fi(f);
        p.size = fi.size();
        if (!fi.isFile()) {
            p.error = QStringLiteral("not a file");
            return p;
        }
        if (!isPhotoFileName(f)) {
            p.error = QStringLiteral("not a supported photo file");
            return p;
        }
        try {
            p.meta = readMetadataSummary(f);
        } catch (const std::exception& e) {
            p.error = QString::fromUtf8(e.what());
        }
        return p;
    });

    // Duplicate detection: same original file name, size and capture time anywhere in the batch.
    QSet<QString> existing;
    auto key = [](const QString& name, qint64 size, const QDateTime& t) {
        return name.toLower() + '|' + QString::number(size) + '|' + t.toString(Qt::ISODate);
    };
    for (const auto& p : batch.photos())
        existing.insert(key(p.fileName, p.fileSize, p.captureTime));

    QString singleName = opts.importName;
    if (singleName.isEmpty() && !opts.splitByDate) {
        QDateTime earliest;
        for (const auto& p : probes)
            if (p.error.isEmpty() && p.meta.captureTime.isValid() && (!earliest.isValid() || p.meta.captureTime < earliest))
                earliest = p.meta.captureTime;
        singleName = dateName(earliest, photos.value(0));
    }

    std::map<QString, ImportInfo> importsByName;
    auto importFor = [&](const QString& name) -> ImportInfo {
        auto it = importsByName.find(name);
        if (it != importsByName.end())
            return it->second;
        auto im = batch.importByName(name);
        ImportInfo info = im ? *im : batch.addImport(name);
        importsByName[name] = info;
        if (!res.importNames.contains(name))
            res.importNames << name;
        return info;
    };

    struct Pending {
        qint64 id;
        QString path;
        int orientation;
    };
    QList<Pending> pending;

    int done = 0;
    for (const Probe& pr : probes) {
        if (progress)
            progress(done, total, pr.source);
        ++done;
        if (!pr.error.isEmpty()) {
            res.errors << pr.source + ": " + pr.error;
            continue;
        }
        QFileInfo fi(pr.source);
        QString k = key(fi.fileName(), pr.size, pr.meta.captureTime);
        if (existing.contains(k)) {
            res.skipped << pr.source;
            continue;
        }
        QString name = singleName.isEmpty() ? dateName(pr.meta.captureTime, pr.source) : singleName;
        ImportInfo im;
        try {
            im = importFor(name);
        } catch (const std::exception& e) {
            res.errors << pr.source + ": " + QString::fromUtf8(e.what());
            continue;
        }
        QDir dir(im.path);
        QString dest;
        if (QFileInfo(pr.source).absolutePath() == dir.absolutePath()) {
            dest = fi.absoluteFilePath(); // already in place
        } else {
            dest = uniqueDestination(dir, fi.fileName());
            bool ok = opts.move ? QFile::rename(pr.source, dest) : false;
            if (!ok) {
                ok = QFile::copy(pr.source, dest);
                if (ok) {
                    QFile df(dest);
                    if (df.open(QIODevice::ReadWrite))
                        df.setFileTime(fi.lastModified(), QFileDevice::FileModificationTime);
                    if (opts.move)
                        QFile::remove(pr.source);
                }
            }
            if (!ok) {
                res.errors << pr.source + ": cannot copy to " + dest;
                continue;
            }
        }
        PhotoInfo p;
        p.importId = im.id;
        p.fileName = fi.fileName();
        p.path = dest;
        p.fileSize = pr.size;
        p.captureTime = pr.meta.captureTime;
        p.make = pr.meta.make;
        p.model = pr.meta.model;
        p.lens = pr.meta.lens;
        p.width = pr.meta.width;
        p.height = pr.meta.height;
        p.orientation = pr.meta.orientation;
        const bool rendered = isRenderedFileName(dest);
        if (rendered) {
            p.kind = PhotoKind::Rendered;
            if (p.orientation >= 5 && p.orientation <= 8)
                std::swap(p.width, p.height);
            p.orientation = 1; // decoded upright
        }
        p.iso = pr.meta.iso;
        p.exposureTime = pr.meta.exposureTime;
        p.fNumber = pr.meta.fNumber;
        p.focalLength = pr.meta.focalLength;
        try {
            qint64 id = rendered ? batch.addPhoto(p, DevelopSettings::defaults(true).toJsonString()) : batch.addPhoto(p);
            res.added << id;
            existing.insert(k);
            pending.append({id, dest, p.orientation});
        } catch (const std::exception& e) {
            res.errors << pr.source + ": " + QString::fromUtf8(e.what());
        }
    }
    if (progress)
        progress(total, total, QString());

    // Thumbnails from embedded previews (fast); rendered thumbnails replace them later.
    QList<QByteArray> thumbs = QtConcurrent::blockingMapped(pending, [](const Pending& p) {
        try {
            return thumbnailFromEmbeddedPreview(p.path, p.orientation);
        } catch (...) {
            return QByteArray();
        }
    });
    for (int i = 0; i < pending.size(); ++i)
        if (!thumbs[i].isEmpty())
            batch.setThumbnail(pending[i].id, thumbs[i], 0);
    return res;
}

} // namespace silma
