#pragma once

#include "Image.h"

#include <QPointF>
#include <QList>
#include <algorithm>
#include <vector>

namespace silma {

// Box mean over a (2r+1)^2 window, edges handled by shrinking the window.
PlaneF boxMean(const PlaneF& in, int r);
// Gaussian blur (separable kernel for small sigma, three box passes otherwise).
PlaneF gaussianBlur(const PlaneF& in, float sigma);

// Self-guided filter (He et al.). Returns the smoothed coefficient planes (mean a, mean b),
// so the filter output at any resolution is a*I + b with a, b bilinearly upsampled.
void guidedFilterCoefficients(const PlaneF& guide, int r, float eps, PlaneF& meanA, PlaneF& meanB);
// Guided filter of `input` with a separate guide: output = meanA*guide + meanB.
void guidedFilterCoefficients(const PlaneF& guide, const PlaneF& input, int r, float eps, PlaneF& meanA, PlaneF& meanB);

// Minimum over a (2r+1)^2 window (separable; edges shrink the window).
PlaneF minFilter(const PlaneF& in, int r);

// Bilinear sample of a plane at normalized coordinates (0..1 spanning the plane).
inline float sampleNormalized(const PlaneF& p, float u, float v)
{
    float x = u * p.width - 0.5f, y = v * p.height - 0.5f;
    x = x < 0 ? 0 : (x > p.width - 1 ? float(p.width - 1) : x);
    y = y < 0 ? 0 : (y > p.height - 1 ? float(p.height - 1) : y);
    int x0 = int(x), y0 = int(y);
    int x1 = x0 + 1 < p.width ? x0 + 1 : x0, y1 = y0 + 1 < p.height ? y0 + 1 : y0;
    float tx = x - x0, ty = y - y0;
    return (p.at(x0, y0) * (1 - tx) + p.at(x1, y0) * tx) * (1 - ty) + (p.at(x0, y1) * (1 - tx) + p.at(x1, y1) * tx) * ty;
}

// Monotone cubic (Fritsch-Carlson) interpolation through control points, tabulated.
class CurveLut {
public:
    static constexpr int kSize = 4096;
    CurveLut() = default;
    explicit CurveLut(const QList<QPointF>& points);
    bool identity() const { return m_identity; }
    float operator()(float x) const
    {
        if (m_identity)
            return x;
        float f = std::clamp(x, 0.0f, 1.0f) * (kSize - 1);
        int i = std::min(int(f), kSize - 2);
        float t = f - i;
        return m_lut[i] * (1 - t) + m_lut[i + 1] * t;
    }

private:
    bool m_identity = true;
    std::vector<float> m_lut;
};

} // namespace silma
