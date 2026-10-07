#include "Color.h"

#include <algorithm>

namespace silma {

Mat3 matMul(const Mat3& a, const Mat3& b)
{
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                r[i][j] += a[i][k] * b[k][j];
    return r;
}

Vec3 matMul(const Mat3& a, const Vec3& v)
{
    return {a[0][0] * v[0] + a[0][1] * v[1] + a[0][2] * v[2], a[1][0] * v[0] + a[1][1] * v[1] + a[1][2] * v[2],
            a[2][0] * v[0] + a[2][1] * v[1] + a[2][2] * v[2]};
}

Mat3 matInverse(const Mat3& m)
{
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                 m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::abs(det) < 1e-12)
        return Mat3{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    double id = 1.0 / det;
    Mat3 r;
    r[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * id;
    r[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
    r[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
    r[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * id;
    r[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
    r[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
    r[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * id;
    r[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
    r[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
    return r;
}

Mat3 matDiag(const Vec3& v) { return Mat3{{{v[0], 0, 0}, {0, v[1], 0}, {0, 0, v[2]}}}; }

bool matIsZero(const Mat3& m)
{
    for (const auto& r : m)
        for (double v : r)
            if (v != 0)
                return false;
    return true;
}

namespace color {
const Mat3 kRec2020ToXyz = {{{0.6369580, 0.1446169, 0.1688810}, {0.2627002, 0.6779981, 0.0593017}, {0.0000000, 0.0280727, 1.0609851}}};
const Mat3 kXyzToRec2020 = matInverse(kRec2020ToXyz);
const Mat3 kXyzToSrgb = {{{3.2404542, -1.5371385, -0.4985314}, {-0.9692660, 1.8760108, 0.0415560}, {0.0556434, -0.2040259, 1.0572252}}};
const Mat3 kRec2020ToSrgb = matMul(kXyzToSrgb, kRec2020ToXyz);
const Vec3 kRec2020Luma = {0.2627002, 0.6779981, 0.0593017};
} // namespace color

Vec3 xyToXyz(XY xy) { return {xy.x / xy.y, 1.0, (1.0 - xy.x - xy.y) / xy.y}; }

XY xyzToXy(const Vec3& v)
{
    double s = v[0] + v[1] + v[2];
    if (s <= 0)
        return {0.3127, 0.3290};
    return {v[0] / s, v[1] / s};
}

namespace {

// Robertson's isotemperature lines: reciprocal megakelvin, u, v, slope.
struct RuvT {
    double r, u, v, t;
};
const RuvT kTempTable[] = {
    {0, 0.18006, 0.26352, -0.24341},   {10, 0.18066, 0.26589, -0.25479},  {20, 0.18133, 0.26846, -0.26876},
    {30, 0.18208, 0.27119, -0.28539},  {40, 0.18293, 0.27407, -0.30470},  {50, 0.18388, 0.27709, -0.32675},
    {60, 0.18494, 0.28021, -0.35156},  {70, 0.18611, 0.28342, -0.37915},  {80, 0.18740, 0.28668, -0.40955},
    {90, 0.18880, 0.28997, -0.44278},  {100, 0.19032, 0.29326, -0.47888}, {125, 0.19462, 0.30141, -0.58204},
    {150, 0.19962, 0.30921, -0.70471}, {175, 0.20525, 0.31647, -0.84901}, {200, 0.21142, 0.32312, -1.0182},
    {225, 0.21807, 0.32909, -1.2168},  {250, 0.22511, 0.33439, -1.4512},  {275, 0.23247, 0.33904, -1.7298},
    {300, 0.24010, 0.34308, -2.0637},  {325, 0.24792, 0.34655, -2.4681},  {350, 0.25591, 0.34951, -2.9641},
    {375, 0.26400, 0.35200, -3.5814},  {400, 0.27218, 0.35407, -4.3633},  {425, 0.28039, 0.35577, -5.3762},
    {450, 0.28863, 0.35714, -6.7262},  {475, 0.29685, 0.35823, -8.5955},  {500, 0.30505, 0.35907, -11.324},
    {525, 0.31320, 0.35968, -15.628},  {550, 0.32129, 0.36011, -23.325},  {575, 0.32931, 0.36038, -40.770},
    {600, 0.33724, 0.36051, -116.45},
};
constexpr double kTintScale = -3000.0;

} // namespace

XY tempTintToXy(double temperature, double tint)
{
    double r = 1.0e6 / std::clamp(temperature, 1000.0, 1.0e6);
    double offset = tint * (1.0 / kTintScale);
    XY result{0.3127, 0.3290};
    for (int i = 0; i < 30; ++i) {
        if (r < kTempTable[i + 1].r || i == 29) {
            double f = (kTempTable[i + 1].r - r) / (kTempTable[i + 1].r - kTempTable[i].r);
            double u = kTempTable[i].u * f + kTempTable[i + 1].u * (1.0 - f);
            double v = kTempTable[i].v * f + kTempTable[i + 1].v * (1.0 - f);
            double uu1 = 1.0, vv1 = kTempTable[i].t;
            double uu2 = 1.0, vv2 = kTempTable[i + 1].t;
            double len1 = std::sqrt(1.0 + vv1 * vv1), len2 = std::sqrt(1.0 + vv2 * vv2);
            uu1 /= len1;
            vv1 /= len1;
            uu2 /= len2;
            vv2 /= len2;
            double uu3 = uu1 * f + uu2 * (1.0 - f), vv3 = vv1 * f + vv2 * (1.0 - f);
            double len3 = std::sqrt(uu3 * uu3 + vv3 * vv3);
            uu3 /= len3;
            vv3 /= len3;
            u += uu3 * offset;
            v += vv3 * offset;
            result.x = 1.5 * u / (u - 4.0 * v + 2.0);
            result.y = v / (u - 4.0 * v + 2.0);
            break;
        }
    }
    return result;
}

void xyToTempTint(XY xy, double& temperature, double& tint)
{
    // Direct port of the DNG SDK algorithm (dng_temperature::Set_xy_coord).
    double u = 2.0 * xy.x / (1.5 - xy.x + 6.0 * xy.y);
    double v = 3.0 * xy.y / (1.5 - xy.x + 6.0 * xy.y);
    double lastDt = 0, lastDv = 0, lastDu = 0;
    temperature = 6504;
    tint = 0;
    for (int i = 1; i <= 30; ++i) {
        double du = 1.0, dv = kTempTable[i].t;
        double len = std::sqrt(1.0 + dv * dv);
        du /= len;
        dv /= len;
        double uu = u - kTempTable[i].u, vv = v - kTempTable[i].v;
        double dt = -uu * dv + vv * du;
        if (dt <= 0.0 || i == 30) {
            if (dt > 0.0)
                dt = 0.0;
            dt = -dt;
            double f = i == 1 ? 0.0 : dt / (lastDt + dt);
            temperature = 1.0e6 / (kTempTable[i - 1].r * f + kTempTable[i].r * (1.0 - f));
            uu = u - (kTempTable[i - 1].u * f + kTempTable[i].u * (1.0 - f));
            vv = v - (kTempTable[i - 1].v * f + kTempTable[i].v * (1.0 - f));
            du = du * (1.0 - f) + lastDu * f;
            dv = dv * (1.0 - f) + lastDv * f;
            len = std::sqrt(du * du + dv * dv);
            du /= len;
            dv /= len;
            tint = (uu * du + vv * dv) * kTintScale;
            break;
        }
        lastDt = dt;
        lastDu = du;
        lastDv = dv;
    }
}

Mat3 bradford(const Vec3& src, const Vec3& dst)
{
    static const Mat3 B = {{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
    static const Mat3 Bi = matInverse(B);
    Vec3 s = matMul(B, src), d = matMul(B, dst);
    return matMul(Bi, matMul(matDiag({d[0] / s[0], d[1] / s[1], d[2] / s[2]}), B));
}

double illuminantTemperature(int code)
{
    switch (code) {
    case 1: case 4: case 9: return 5500;
    case 2: case 14: return 4150;
    case 3: return 2850;
    case 10: return 6500;
    case 11: return 7500;
    case 12: return 6430;
    case 13: return 5000;
    case 15: return 3450;
    case 17: return 2856;
    case 18: return 4874;
    case 19: return 6774;
    case 20: return 5503;
    case 21: return 6504;
    case 22: return 7504;
    case 23: return 5003;
    case 24: return 3200;
    default: return 0;
    }
}

CameraColor::CameraColor(const RawColorInfo& info) : m_info(info)
{
    const auto& d = info.dng;
    if (d[0].valid && d[1].valid) {
        double t1 = illuminantTemperature(d[0].illuminant), t2 = illuminantTemperature(d[1].illuminant);
        if (t1 > 0 && t2 > 0 && std::abs(t1 - t2) > 1) {
            m_dual = true;
            // Keep m_t1 < m_t2.
            bool swap = t1 > t2;
            m_t1 = swap ? t2 : t1;
            m_t2 = swap ? t1 : t2;
            m_m1 = swap ? d[1].colorMatrix : d[0].colorMatrix;
            m_m2 = swap ? d[0].colorMatrix : d[1].colorMatrix;
        }
    }
    if (!m_dual && matIsZero(m_info.xyzToCam)) {
        if (d[1].valid)
            m_info.xyzToCam = d[1].colorMatrix;
        else if (d[0].valid)
            m_info.xyzToCam = d[0].colorMatrix;
        else // Last resort: treat the camera as sRGB.
            m_info.xyzToCam = color::kXyzToSrgb;
    }
}

Mat3 CameraColor::xyzToCam(double temperature) const
{
    if (!m_dual)
        return m_info.xyzToCam;
    // Interpolate linearly in inverse temperature, as the DNG specification describes.
    double g;
    if (temperature <= m_t1)
        g = 1.0;
    else if (temperature >= m_t2)
        g = 0.0;
    else
        g = (1.0 / temperature - 1.0 / m_t2) / (1.0 / m_t1 - 1.0 / m_t2);
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i][j] = g * m_m1[i][j] + (1.0 - g) * m_m2[i][j];
    return r;
}

Vec3 CameraColor::multipliersFor(double temperature, double tint) const
{
    XY xy = tempTintToXy(temperature, tint);
    Vec3 neutral = matMul(xyzToCam(temperature), xyToXyz(xy));
    Vec3 mul{neutral[1] / neutral[0], 1.0, neutral[1] / neutral[2]};
    for (auto& v : mul)
        if (!(v > 0) || !std::isfinite(v))
            v = 1.0;
    return mul;
}

XY CameraColor::whiteXyFor(const Vec3& mul) const
{
    Vec3 neutral{1.0 / mul[0], 1.0 / mul[1], 1.0 / mul[2]};
    // Iterate because the matrix depends on the temperature (DNG SDK approach).
    XY xy{0.3127, 0.3290};
    double t = 6504;
    for (int i = 0; i < 30; ++i) {
        XY next = xyzToXy(matMul(matInverse(xyzToCam(t)), neutral));
        double tint;
        xyToTempTint(next, t, tint);
        bool done = std::abs(next.x - xy.x) + std::abs(next.y - xy.y) < 1e-7;
        xy = next;
        if (done || !m_dual)
            break;
    }
    return xy;
}

void CameraColor::tempTintFor(const Vec3& mul, double& temperature, double& tint) const
{
    xyToTempTint(whiteXyFor(mul), temperature, tint);
}

Mat3 CameraColor::camToRec2020(const Vec3& mul) const
{
    XY white = whiteXyFor(mul);
    double t, tint;
    xyToTempTint(white, t, tint);
    Mat3 camToXyz = matInverse(xyzToCam(t));
    Vec3 whiteXyz = xyToXyz(white);
    // camWB -> camRaw -> XYZ (scene) -> XYZ adapted to D65 -> Rec.2020
    Mat3 m = matMul(color::kXyzToRec2020, matMul(bradford(whiteXyz, color::kD65), matMul(camToXyz, matDiag({1.0 / mul[0], 1.0 / mul[1], 1.0 / mul[2]}))));
    // Normalize so that camera neutral (1,1,1) maps to Rec.2020 (1,1,1) exactly.
    Vec3 w = matMul(m, Vec3{1, 1, 1});
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[i][j] /= w[i];
    return m;
}

// ---- OkLab ----------------------------------------------------------------

namespace {
// Linear Rec.2020 -> LMS (OkLab M1 composed with Rec.2020 -> XYZ).
const Mat3 kXyzToLms = {{{0.8189330101, 0.3618667424, -0.1288597137}, {0.0329845436, 0.9293118715, 0.0361456387}, {0.0482003018, 0.2643662691, 0.6338517070}}};
const Mat3 kRec2020ToLms = matMul(kXyzToLms, color::kRec2020ToXyz);
const Mat3 kLmsToRec2020 = matInverse(kRec2020ToLms);
const Mat3 kLmsToLab = {{{0.2104542553, 0.7936177850, -0.0040720468}, {1.9779984951, -2.4285922050, 0.4505937099}, {0.0259040371, 0.7827717662, -0.8086757660}}};
const Mat3 kLabToLms = matInverse(kLmsToLab);

struct FMat {
    float m[9];
    explicit FMat(const Mat3& a)
    {
        for (int i = 0; i < 9; ++i)
            m[i] = float(a[i / 3][i % 3]);
    }
};
const FMat fRgbToLms(kRec2020ToLms), fLmsToRgb(kLmsToRec2020), fLmsToLab(kLmsToLab), fLabToLms(kLabToLms);
} // namespace

void rec2020ToOklab(const float* rgb, float* lab)
{
    const float* a = fRgbToLms.m;
    float l = std::cbrt(a[0] * rgb[0] + a[1] * rgb[1] + a[2] * rgb[2]);
    float m = std::cbrt(a[3] * rgb[0] + a[4] * rgb[1] + a[5] * rgb[2]);
    float s = std::cbrt(a[6] * rgb[0] + a[7] * rgb[1] + a[8] * rgb[2]);
    const float* b = fLmsToLab.m;
    lab[0] = b[0] * l + b[1] * m + b[2] * s;
    lab[1] = b[3] * l + b[4] * m + b[5] * s;
    lab[2] = b[6] * l + b[7] * m + b[8] * s;
}

void oklabToRec2020(const float* lab, float* rgb)
{
    const float* b = fLabToLms.m;
    float l = b[0] * lab[0] + b[1] * lab[1] + b[2] * lab[2];
    float m = b[3] * lab[0] + b[4] * lab[1] + b[5] * lab[2];
    float s = b[6] * lab[0] + b[7] * lab[1] + b[8] * lab[2];
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    const float* a = fLmsToRgb.m;
    rgb[0] = a[0] * l + a[1] * m + a[2] * s;
    rgb[1] = a[3] * l + a[4] * m + a[5] * s;
    rgb[2] = a[6] * l + a[7] * m + a[8] * s;
}

} // namespace silma
