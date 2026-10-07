#include "Derived.h"

#include "AiDenoise.h"
#include "Develop.h"
#include "Importer.h"
#include "Metadata.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <stdexcept>

namespace silma {

namespace {

QString uniquePath(const QString& dir, const QString& base)
{
    QString p = QDir(dir).filePath(base + ".dng");
    for (int i = 2; QFileInfo::exists(p); ++i)
        p = QDir(dir).filePath(QStringLiteral("%1-%2.dng").arg(base).arg(i));
    return p;
}

} // namespace

qint64 addDerivedPhoto(Batch& b, const PhotoInfo& src, const QList<qint64>& sources, const QString& suffix, const ImageF& img,
                       const LinearDngInfo& info, const QString& label, const QString& settingsJson, bool floatingPoint)
{
    auto im = b.importById(src.importId);
    if (!im)
        throw std::runtime_error("Source import not found");
    QString path = uniquePath(im->path, QFileInfo(src.fileName).completeBaseName() + "-" + suffix);
    writeLinearDng(path, img, info, floatingPoint);
    PhotoInfo p = src;
    p.id = 0;
    p.fileName = QFileInfo(path).fileName();
    p.path = path;
    p.fileSize = QFileInfo(path).size();
    p.width = img.width;
    p.height = img.height;
    p.rejected = false;
    p.kind = PhotoKind::DerivedDng;
    QJsonArray ids;
    for (qint64 id : sources)
        ids.append(id);
    p.derivedFrom = QString::fromUtf8(QJsonDocument(ids).toJson(QJsonDocument::Compact));
    qint64 id = b.addPhoto(p, settingsJson);
    if (!label.isEmpty())
        b.pushHistory(id, label, settingsJson);
    try {
        b.setThumbnail(id, thumbnailFromEmbeddedPreview(path, info.orientation), 0);
    } catch (...) {
    }
    return id;
}

qint64 createAiDenoised(Batch& b, qint64 photoId, const QString& modelId, double strength, const StepProgress& progress,
                        const std::atomic<bool>* cancel, QString* providerUsed)
{
    auto src = b.photo(photoId);
    if (!src)
        throw std::runtime_error("No such photo");
    if (!aiModelInstalled(modelId)) {
        if (progress)
            progress(QStringLiteral("Downloading model"), 0, 0);
        downloadAiModel(modelId, [&](qint64 r, qint64 t) {
            if (progress)
                progress(QStringLiteral("Downloading model"), int(r / 1024), int(t / 1024));
        });
    }
    if (progress)
        progress(QStringLiteral("Decoding"), 0, 0);
    RawImage raw = decodeRaw(src->path);
    if (cancel && *cancel)
        throw std::runtime_error("Cancelled");
    ImageF den = aiDenoiseLinear(modelId, raw.image, strength, [&](int d, int t) {
        if (progress)
            progress(QStringLiteral("Denoising"), d, t);
    }, cancel, providerUsed);
    if (progress)
        progress(QStringLiteral("Writing DNG"), 0, 0);
    LinearDngInfo info = dngInfoFromRaw(raw, src->path, src->lens);
    // Inherit the source's develop settings; the AI result needs little classic NR.
    DevelopSettings s = DevelopSettings::fromJsonString(b.settings(photoId));
    s.nrLuminance = 0;
    s.nrColor = std::min(s.nrColor, 10.0);
    return addDerivedPhoto(b, *src, {photoId}, "Denoise", unWhiteBalance(den, raw.color), info,
                           QStringLiteral("AI Denoise (%1)").arg(modelId), s.toJsonString());
}

qint64 createLinearCopy(Batch& b, qint64 photoId)
{
    auto src = b.photo(photoId);
    if (!src)
        throw std::runtime_error("No such photo");
    RawImage raw = decodeRaw(src->path);
    LinearDngInfo info = dngInfoFromRaw(raw, src->path, src->lens);
    return addDerivedPhoto(b, *src, {photoId}, "Linear", unWhiteBalance(raw.image, raw.color), info, QString(), b.settings(photoId));
}

} // namespace silma
