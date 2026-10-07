#pragma once
// Lens corrections (distortion, lateral chromatic aberration, vignetting) from the lensfun
// database or from DNG opcode lists, sampled onto a coarse grid that maps undistorted
// image positions to source positions per colour channel.

#include "RawDecoder.h"

#include <QString>
#include <memory>
#include <vector>

namespace silma {

struct LensQuery {
    QString cameraMake, cameraModel, lens;
    double focalLength = 0, aperture = 0;
};

class LensCorrection {
public:
    // Builds the correction for a decoded raw (full-res dimensions are used). Never throws;
    // returns an identity correction when nothing is known about the lens.
    static std::shared_ptr<const LensCorrection> build(const RawImage& raw, const LensQuery& q);

    bool isIdentity() const { return m_identity; }
    QString description() const { return m_description; }

    // (u, v): normalized position [0,1]^2 in the corrected (sensor-oriented) frame.
    // srcXY receives source positions (normalized) for R, G, B: x0,y0,x1,y1,x2,y2.
    // distAmount scales geometric distortion correction (1 = profile), TCA is always applied.
    // gain receives per-channel vignetting gains scaled by vigAmount (as an exponent).
    void map(float u, float v, float distAmount, float vigAmount, float* srcXY, float* gain) const;

    // Smallest zoom factor (>= 1) such that the corrected frame has no undefined borders.
    double autoScale(float distAmount) const;

private:
    LensCorrection() = default;
    void sampleBilinear(const std::vector<float>& grid, int comps, float u, float v, float* out) const;

    bool m_identity = true;
    QString m_description;
    int m_gw = 0, m_gh = 0;
    std::vector<float> m_src;  // gw*gh*6: normalized source positions per channel
    std::vector<float> m_gain; // gw*gh*3
};

} // namespace silma
