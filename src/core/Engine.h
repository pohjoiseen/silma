#pragma once
// DevelopEngine: holds a decoded photo and renders it with develop settings.
//
// Pipeline (all in float):
//   source pyramid level (camera RGB, as-shot WB)
//   -> geometry warp (lens, orientation, perspective, crop)      [Geometry]
//   -> white balance re-scale + highlight clip -> camera->Rec.2020 matrix -> exposure
//   -> dehaze (dark channel prior, transmission from the context) -> texture
//   -> local tone (highlights/shadows, clarity) from a low-res context of the whole crop
//   -> global tone curve (contrast, whites, blacks) per channel with hue preservation
//   -> vibrance / saturation (OkLab) -> point tone curve (display-encoded)
//   -> Rec.2020 -> sRGB gamut mapping -> sharpening -> encode

#include "Color.h"
#include "Develop.h"
#include "Geometry.h"
#include "Image.h"
#include "LensCorrection.h"
#include "RawDecoder.h"

#include <QImage>
#include <QRectF>
#include <QString>
#include <array>
#include <optional>
#include <memory>
#include <map>
#include <mutex>

namespace silma {

struct HealPatch;

struct WbValues {
    double temperature = 5500, tint = 0;
    Vec3 mul{1, 1, 1};
};

struct RenderRequest {
    int width = 0, height = 0;   // output size in pixels
    QRectF roi{0, 0, 1, 1};      // region of the crop to render (normalized crop coords)
    bool highQuality = false;    // bicubic resampling (export)
    bool ignoreCrop = false;     // render the whole transformed canvas (crop tool)
};

using Histogram = std::array<std::array<quint32, 256>, 3>;

struct RenderResult {
    QImage image; // Format_RGB888, sRGB
    Histogram histogram{};
    WbValues wb;
};

struct EngineLoadOptions {
    bool halfSize = false; // fast preview decode
};

class DevelopEngine {
public:
    // Throws std::runtime_error when the file cannot be decoded.
    explicit DevelopEngine(const QString& path, const EngineLoadOptions& opts = {});

    QString path() const { return m_path; }
    bool isHalfSize() const { return m_raw->halfSize; }
    const RawImage& raw() const { return *m_raw; }
    QString lensDescription() const { return m_lens ? m_lens->description() : QString(); }

    WbValues asShotWb() const;
    WbValues autoWb() const;
    WbValues effectiveWb(const DevelopSettings& s) const;
    WbValues wbFromTempTint(double temperature, double tint) const;
    // White balance that makes the area around `cropPoint` (normalized output coordinates)
    // neutral; nullopt where it is clipped, too dark or outside the image.
    std::optional<WbValues> wbFromPoint(const DevelopSettings& s, QPointF cropPoint) const;

    Geometry geometry(const DevelopSettings& s, bool ignoreCrop = false) const;
    // Full-resolution size of the cropped output.
    QSize outputSize(const DevelopSettings& s) const;

    RenderResult render(const DevelopSettings& s, const RenderRequest& req) const;
    // Final image as display-encoded sRGB floats (0..1), for export.
    ImageF renderEncoded(const DevelopSettings& s, const RenderRequest& req, WbValues* wbOut = nullptr) const;

    // Geometry-only render (lens corrections, orientation, transform, crop): camera RGB
    // white-balanced with the decode multipliers, no colour or tone processing.
    ImageF warpedCamera(const DevelopSettings& s, int width, int height, bool highQuality, bool ignoreCrop = true) const;

    // Heal patches must be added before rendering settings that reference them.
    void addHealPatch(qint64 id, const HealPatch& patch);
    bool hasHealPatch(qint64 id) const;

    // Automatic tone: returns settings with exposure/contrast/highlights/shadows/whites/blacks set.
    DevelopSettings autoTone(const DevelopSettings& s) const;

private:
    std::shared_ptr<const ImageF> level(int lvl) const;
    void applyHeals(const DevelopSettings& s) const;
    int levelFor(double srcPxPerOutPx) const;
    ImageF warpCached(const Geometry& g, const DevelopSettings& s, int lvl, int w, int h, const WarpOptions& o) const;
    void colorStage(ImageF& img, const DevelopSettings& s, const WbValues& wb) const;
    ImageF contextImage(const DevelopSettings& s, const WbValues& wb, bool ignoreCrop) const;

    QString m_path;
    std::shared_ptr<RawImage> m_raw;
    std::shared_ptr<const LensCorrection> m_lens;
    std::unique_ptr<CameraColor> m_color;
    GeometrySource m_geoSrc;
    int m_levelOffset = 0; // 1 when decoded at half size

    mutable std::mutex m_mutex;
    mutable std::vector<std::shared_ptr<ImageF>> m_pyramid;
    mutable std::map<qint64, std::shared_ptr<HealPatch>> m_patches;
    mutable QString m_healKey;
    mutable std::optional<WbValues> m_autoWb;
    struct WarpCache {
        QString key;
        std::shared_ptr<ImageF> img;
    };
    mutable WarpCache m_ctxCache, m_mainCache;
};

// Exposure added to every render so the default look roughly matches camera JPEGs.
double defaultBaselineExposure(const RawImage& raw);
// Inverse of what default settings do to raw data after the colour matrix (default exposure,
// tone curve and base chroma): turns display-referred linear Rec.2020 (a decoded JPEG) into
// data that default settings render back unchanged.
void undoDefaultLook(ImageF& displayLinear);

// Converts encoded float sRGB to an 8-bit QImage (RGB888) and fills a histogram.
QImage toQImage(const ImageF& encoded, Histogram* hist = nullptr);

} // namespace silma
