#pragma once
// Photos derived from others (AI denoise, HDR, panorama), stored as linear DNGs in the
// import folder of the (first) source photo and added to the batch.

#include "Batch.h"
#include "Develop.h"
#include "DngWriter.h"

#include <QString>
#include <atomic>
#include <functional>

namespace silma {

using StepProgress = std::function<void(const QString& step, int done, int total)>;

// Writes `cameraLinear` as a DNG next to the source and adds it to the batch.
// Initial develop settings are copied from the source. Returns the new photo id.
qint64 addDerivedPhoto(Batch& b, const PhotoInfo& source, const QList<qint64>& sources, const QString& suffix,
                       const ImageF& cameraLinear, const LinearDngInfo& info, const QString& label,
                       const QString& settingsJson, bool floatingPoint = false);

// Develop settings of an AI-denoised copy, from those of its source.
DevelopSettings aiDenoisedSettings(const DevelopSettings& source);
// AI denoise: returns the new photo id. Throws on failure / cancel.
qint64 createAiDenoised(Batch& b, qint64 photoId, const QString& modelId, double strength, const StepProgress& progress = {},
                        const std::atomic<bool>* cancel = nullptr, QString* providerUsed = nullptr);

// Re-encodes a raw as linear DNG without changes (round-trip testing).
qint64 createLinearCopy(Batch& b, qint64 photoId);

} // namespace silma
