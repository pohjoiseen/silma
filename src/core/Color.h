#pragma once

#include "RawDecoder.h"

#include <array>
#include <cmath>

namespace silma {

using Vec3 = std::array<double, 3>;

Mat3 matMul(const Mat3& a, const Mat3& b);
Vec3 matMul(const Mat3& a, const Vec3& v);
Mat3 matInverse(const Mat3& m);
Mat3 matDiag(const Vec3& v);
bool matIsZero(const Mat3& m);

namespace color {
extern const Mat3 kRec2020ToXyz;
extern const Mat3 kXyzToRec2020;
extern const Mat3 kXyzToSrgb;
extern const Mat3 kRec2020ToSrgb;
extern const Vec3 kRec2020Luma; // luminance weights
constexpr Vec3 kD65 = {0.95047, 1.0, 1.08883};
} // namespace color

struct XY {
    double x, y;
};
Vec3 xyToXyz(XY xy);
XY xyzToXy(const Vec3& v);
// Correlated colour temperature + tint (DNG SDK convention) <-> chromaticity.
XY tempTintToXy(double temperature, double tint);
void xyToTempTint(XY xy, double& temperature, double& tint);
Mat3 bradford(const Vec3& srcWhiteXyz, const Vec3& dstWhiteXyz);
double illuminantTemperature(int exifLightSource);

// Camera colour model built from raw colour info.
class CameraColor {
public:
    explicit CameraColor(const RawColorInfo& info);
    // XYZ -> camera matrix appropriate for an illuminant of the given temperature.
    Mat3 xyzToCam(double temperature) const;
    // White-balance multipliers (green = 1) neutralizing the given illuminant.
    Vec3 multipliersFor(double temperature, double tint) const;
    // Illuminant temperature/tint for given multipliers.
    void tempTintFor(const Vec3& mul, double& temperature, double& tint) const;
    // Matrix mapping white-balanced camera RGB (neutral = 1,1,1) to linear Rec.2020 (D65).
    Mat3 camToRec2020(const Vec3& mul) const;
    // Chromaticity of the illuminant that `mul` neutralizes.
    XY whiteXyFor(const Vec3& mul) const;
    // Weight of the first calibration illuminant's data at a temperature (1 for a single matrix).
    double matrixWeight(double temperature) const;

private:
    RawColorInfo m_info;
    bool m_dual = false;
    double m_t1 = 0, m_t2 = 0;
    Mat3 m_m1{}, m_m2{};
};

// sRGB transfer functions
inline float srgbEncode(float v)
{
    return v <= 0.0031308f ? 12.92f * v : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}
inline float srgbDecode(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

// OkLab from/to linear Rec.2020
void rec2020ToOklab(const float* rgb, float* lab);
void oklabToRec2020(const float* lab, float* rgb);

} // namespace silma
