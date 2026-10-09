#pragma once
// Photo merges producing linear DNGs: HDR (bracketed exposures) and cylindrical panoramas.

#include "Batch.h"
#include "Derived.h"

#include <QImage>
#include <QList>
#include <QRectF>
#include <atomic>
#include <memory>

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

// A photo to stitch, with its quarter turns (develop setting) on top of the file orientation.
struct PanoramaSource {
    PhotoInfo photo;
    int quarterTurns = 0;
};
QList<PanoramaSource> panoramaSources(Batch& b, const QList<qint64>& ids);

// A quick, small stitch of photos for a preview (half-size decodes, the working resolution
// of the registration), rendered like the panorama DNG would be. Throws like createPanorama
// (e.g. when the photos do not connect).
class PanoramaPreview {
public:
    PanoramaPreview(const QList<PanoramaSource>& sources, const StepProgress& progress = {}, const std::atomic<bool>* cancel = nullptr);
    ~PanoramaPreview();
    // The whole stitched area (uncropped), long edge at most `longEdge`.
    QImage render(bool autoTone, int longEdge) const;
    // The auto crop (normalized), as the panorama would get it.
    QRectF autoCrop() const;
    QSize size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Largest axis-aligned rectangle of non-zero mask pixels (8-bit mask, row-major).
QRect largestRectangle(const std::vector<uint8_t>& mask, int width, int height);

} // namespace silma
