#pragma once
// The geometric part of the pipeline as one inverse mapping, from output (crop) coordinates
// back to source sensor coordinates:
//
//   crop (normalized) -> canvas (transformed frame) -> [perspective/rotate/scale]^-1
//   -> oriented frame -> [lens auto-zoom] -> [EXIF orientation]^-1 -> sensor frame
//   -> lens correction (distortion, CA per channel) -> source position
//
// All images are resampled once.

#include "Color.h"
#include "Develop.h"
#include "Image.h"
#include "LensCorrection.h"

#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <memory>

namespace silma {

struct GeometrySource {
    int sensorWidth = 0, sensorHeight = 0; // full-resolution dimensions
    int orientation = 1;
    double focalLength35 = 0; // for perspective; 0 = unknown
    std::shared_ptr<const LensCorrection> lens;
};

class Geometry {
public:
    Geometry(const GeometrySource& src, const DevelopSettings& s, bool ignoreCrop = false);

    // Size of the oriented frame / transformed canvas in full-resolution pixels.
    QSizeF canvasSize() const { return {m_wo, m_ho}; }
    // Size of the crop in full-resolution pixels.
    QSizeF cropSize() const { return {m_crop.width() * m_wo, m_crop.height() * m_ho}; }
    QRectF crop() const { return m_crop; }

    // Normalized crop coordinates -> normalized source (sensor) coordinates per channel + gains.
    // Returns false if the point falls outside the source image.
    bool mapCrop(double u, double v, float* srcXY, float* gain) const;
    // Normalized canvas coordinates -> same as above.
    bool mapCanvas(double cu, double cv, float* srcXY, float* gain) const;
    // Forward: normalized oriented-frame coordinates (undistorted) -> normalized canvas.
    QPointF frameToCanvas(QPointF f) const;
    // Inverse: normalized canvas -> normalized oriented-frame coordinates.
    QPointF canvasToFrame(QPointF c) const;
    // True when the canvas point maps to valid image data.
    bool canvasValid(QPointF c) const;

    // Inverse of mapCrop's green channel: normalized sensor -> normalized crop (Newton).
    QPointF sensorToCrop(QPointF sensor) const;

    double lensZoom() const { return m_lensZoom; }
    // Focal length in full-resolution pixels used for the perspective homography.
    double focalPx() const { return m_focalPx; }

private:
    GeometrySource m_src;
    double m_wo = 0, m_ho = 0; // oriented frame size (px)
    QRectF m_crop;
    Mat3 m_h{}, m_hinv{}; // centred-pixel homography (frame -> canvas), inverse
    double m_scale = 1;
    double m_lensZoom = 1;
    double m_focalPx = 1000;
    float m_distAmount = 1, m_vigAmount = 1;
};

struct WarpOptions {
    bool bicubic = false;
    QRectF roi{0, 0, 1, 1}; // region of the crop to render (normalized)
};

// Resamples `src` (a pyramid level covering the whole sensor) into an output image of the
// given size. Pixels outside the source are 0. Gains (vignetting) are applied.
ImageF warp(const ImageF& src, const Geometry& g, int outWidth, int outHeight, const WarpOptions& opts);

// Builds the perspective homography (centred pixel coordinates, frame -> canvas).
Mat3 perspectiveMatrix(double verticalDeg, double horizontalDeg, double rotateDeg, double focalPx);

} // namespace silma
