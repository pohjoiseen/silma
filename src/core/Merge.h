#pragma once
// Photo merges producing linear DNGs: HDR (bracketed exposures) and cylindrical panoramas.

#include "Batch.h"
#include "Derived.h"

#include <QList>
#include <atomic>

namespace silma {

struct HdrOptions {
    bool align = true;
    bool deghost = true;
    bool autoTone = true;
};

struct PanoOptions {
    bool autoCrop = true;
    bool autoTone = true;
};

// Both return the new photo id; throw std::runtime_error on failure or cancel.
qint64 createHdrMerge(Batch& b, const QList<qint64>& ids, const HdrOptions& o, const StepProgress& progress = {},
                      const std::atomic<bool>* cancel = nullptr);
qint64 createPanorama(Batch& b, const QList<qint64>& ids, const PanoOptions& o, const StepProgress& progress = {},
                      const std::atomic<bool>* cancel = nullptr);

// Largest axis-aligned rectangle of non-zero mask pixels (8-bit mask, row-major).
QRect largestRectangle(const std::vector<uint8_t>& mask, int width, int height);

} // namespace silma
