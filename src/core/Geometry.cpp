#include "Geometry.h"

#include <algorithm>
#include <cmath>

namespace silma {

namespace {

constexpr double kMaxPerspectiveDeg = 25.0; // slider +-100 maps to this tilt

inline Vec3 applyH(const Mat3& h, double x, double y)
{
    return {h[0][0] * x + h[0][1] * y + h[0][2], h[1][0] * x + h[1][1] * y + h[1][2], h[2][0] * x + h[2][1] * y + h[2][2]};
}

// Oriented (display) frame centred coords -> sensor centred coords, for EXIF orientation.
inline void unorient(int o, double x, double y, double& sx, double& sy)
{
    switch (o) {
    case 2: sx = -x; sy = y; break;
    case 3: sx = -x; sy = -y; break;
    case 4: sx = x; sy = -y; break;
    case 5: sx = y; sy = x; break;
    case 6: sx = y; sy = -x; break;
    case 7: sx = -y; sy = -x; break;
    case 8: sx = -y; sy = x; break;
    default: sx = x; sy = y; break;
    }
}

} // namespace

Mat3 perspectiveMatrix(double verticalDeg, double horizontalDeg, double rotateDeg, double f)
{
    const double d2r = M_PI / 180.0;
    double a = verticalDeg * d2r, b = horizontalDeg * d2r, r = rotateDeg * d2r;
    Mat3 rx = {{{1, 0, 0}, {0, std::cos(a), -std::sin(a)}, {0, std::sin(a), std::cos(a)}}};
    Mat3 ry = {{{std::cos(b), 0, std::sin(b)}, {0, 1, 0}, {-std::sin(b), 0, std::cos(b)}}};
    Mat3 rz = {{{std::cos(r), -std::sin(r), 0}, {std::sin(r), std::cos(r), 0}, {0, 0, 1}}};
    Mat3 k = {{{f, 0, 0}, {0, f, 0}, {0, 0, 1}}};
    Mat3 ki = {{{1 / f, 0, 0}, {0, 1 / f, 0}, {0, 0, 1}}};
    Mat3 h = matMul(k, matMul(rz, matMul(rx, matMul(ry, ki))));
    // Re-centre: the frame centre maps to the canvas centre.
    Vec3 c = applyH(h, 0, 0);
    double cx = c[0] / c[2], cy = c[1] / c[2];
    Mat3 t = {{{1, 0, -cx}, {0, 1, -cy}, {0, 0, 1}}};
    return matMul(t, h);
}

Geometry::Geometry(const GeometrySource& src, const DevelopSettings& s, bool ignoreCrop) : m_src(src)
{
    bool swap = src.orientation >= 5 && src.orientation <= 8;
    m_wo = swap ? src.sensorHeight : src.sensorWidth;
    m_ho = swap ? src.sensorWidth : src.sensorHeight;
    m_crop = ignoreCrop ? QRectF(0, 0, 1, 1) : s.crop;
    m_distAmount = float(s.lensDistortion / 100.0);
    m_vigAmount = float(s.lensVignetting / 100.0);
    m_lensZoom = src.lens ? src.lens->autoScale(m_distAmount) : 1.0;
    m_scale = s.scale / 100.0;

    double diag = std::hypot(m_wo, m_ho);
    double f35 = src.focalLength35 > 0 ? src.focalLength35 : 28.0;
    m_focalPx = f35 / 43.27 * diag;
    m_h = perspectiveMatrix(-s.perspVertical / 100.0 * kMaxPerspectiveDeg, s.perspHorizontal / 100.0 * kMaxPerspectiveDeg, s.rotate, m_focalPx);
    m_hinv = matInverse(m_h);
}

QPointF Geometry::canvasToFrame(QPointF c) const
{
    double x = (c.x() - 0.5) * m_wo / m_scale, y = (c.y() - 0.5) * m_ho / m_scale;
    Vec3 p = applyH(m_hinv, x, y);
    if (p[2] <= 1e-9)
        return {-10, -10};
    return {p[0] / p[2] / m_wo + 0.5, p[1] / p[2] / m_ho + 0.5};
}

QPointF Geometry::frameToCanvas(QPointF f) const
{
    double x = (f.x() - 0.5) * m_wo, y = (f.y() - 0.5) * m_ho;
    Vec3 p = applyH(m_h, x, y);
    if (p[2] <= 1e-9)
        return {-10, -10};
    return {p[0] / p[2] * m_scale / m_wo + 0.5, p[1] / p[2] * m_scale / m_ho + 0.5};
}

bool Geometry::mapCanvas(double cu, double cv, float* srcXY, float* gain) const
{
    QPointF f = canvasToFrame({cu, cv});
    // Lens auto-zoom: the visible frame is the centre part of the corrected image.
    double fx = (f.x() - 0.5) / m_lensZoom, fy = (f.y() - 0.5) / m_lensZoom;
    double sx, sy;
    unorient(m_src.orientation, fx, fy, sx, sy);
    float u = float(sx + 0.5), v = float(sy + 0.5);
    if (m_src.lens) {
        m_src.lens->map(u, v, m_distAmount, m_vigAmount, srcXY, gain);
    } else {
        for (int c = 0; c < 3; ++c) {
            srcXY[c * 2] = u;
            srcXY[c * 2 + 1] = v;
            gain[c] = 1;
        }
    }
    constexpr float eps = 1e-4f;
    return srcXY[2] >= -eps && srcXY[2] <= 1 + eps && srcXY[3] >= -eps && srcXY[3] <= 1 + eps;
}

bool Geometry::mapCrop(double u, double v, float* srcXY, float* gain) const
{
    return mapCanvas(m_crop.x() + u * m_crop.width(), m_crop.y() + v * m_crop.height(), srcXY, gain);
}

QPointF Geometry::sensorToCrop(QPointF target) const
{
    auto f = [&](QPointF p) {
        float s[6], g[3];
        mapCrop(p.x(), p.y(), s, g);
        return QPointF(s[2], s[3]);
    };
    // Initial guess by orientation only, then Newton iterations with numeric Jacobian.
    QPointF p(0.5, 0.5);
    for (int it = 0; it < 12; ++it) {
        QPointF v = f(p) - target;
        if (std::hypot(v.x(), v.y()) < 1e-7)
            break;
        const double h = 1e-4;
        QPointF dx = (f(p + QPointF(h, 0)) - f(p)) / h, dy = (f(p + QPointF(0, h)) - f(p)) / h;
        double det = dx.x() * dy.y() - dy.x() * dx.y();
        if (std::abs(det) < 1e-12)
            break;
        // Solve J * d = v
        double ddx = (v.x() * dy.y() - dy.x() * v.y()) / det, ddy = (dx.x() * v.y() - v.x() * dx.y()) / det;
        p -= QPointF(ddx, ddy);
    }
    return p;
}

bool Geometry::canvasValid(QPointF c) const
{
    float s[6], g[3];
    return mapCanvas(c.x(), c.y(), s, g) && (!m_src.valid || m_src.valid->at(s[2], s[3]));
}

namespace {

inline float cubicWeight(float t, int i)
{
    // Catmull-Rom weights for taps at -1, 0, 1, 2.
    float t2 = t * t, t3 = t2 * t;
    switch (i) {
    case 0: return -0.5f * t3 + t2 - 0.5f * t;
    case 1: return 1.5f * t3 - 2.5f * t2 + 1.0f;
    case 2: return -1.5f * t3 + 2.0f * t2 + 0.5f * t;
    default: return 0.5f * t3 - 0.5f * t2;
    }
}

inline float sampleBilinear(const ImageF& img, float x, float y, int c)
{
    x = std::clamp(x - 0.5f, 0.0f, float(img.width - 1));
    y = std::clamp(y - 0.5f, 0.0f, float(img.height - 1));
    int x0 = int(x), y0 = int(y);
    int x1 = std::min(x0 + 1, img.width - 1), y1 = std::min(y0 + 1, img.height - 1);
    float tx = x - x0, ty = y - y0;
    float a = img.px(x0, y0)[c], b = img.px(x1, y0)[c], cc = img.px(x0, y1)[c], d = img.px(x1, y1)[c];
    return (a * (1 - tx) + b * tx) * (1 - ty) + (cc * (1 - tx) + d * tx) * ty;
}

inline float sampleBicubic(const ImageF& img, float x, float y, int c)
{
    x -= 0.5f;
    y -= 0.5f;
    int xi = int(std::floor(x)), yi = int(std::floor(y));
    float tx = x - xi, ty = y - yi;
    float wx[4], wy[4];
    for (int i = 0; i < 4; ++i) {
        wx[i] = cubicWeight(tx, i);
        wy[i] = cubicWeight(ty, i);
    }
    float acc = 0;
    for (int j = 0; j < 4; ++j) {
        int yy = std::clamp(yi - 1 + j, 0, img.height - 1);
        float racc = 0;
        for (int i = 0; i < 4; ++i) {
            int xx = std::clamp(xi - 1 + i, 0, img.width - 1);
            racc += wx[i] * img.px(xx, yy)[c];
        }
        acc += wy[j] * racc;
    }
    return std::max(acc, 0.0f);
}

} // namespace

ImageF warp(const ImageF& src, const Geometry& g, int outWidth, int outHeight, const WarpOptions& opts)
{
    ImageF out(outWidth, outHeight);
    const QRectF roi = opts.roi;
    const float sw = float(src.width), sh = float(src.height);
#pragma omp parallel for schedule(dynamic, 8)
    for (int y = 0; y < outHeight; ++y) {
        float* o = out.row(y);
        double v = roi.y() + (y + 0.5) / outHeight * roi.height();
        for (int x = 0; x < outWidth; ++x, o += 3) {
            double u = roi.x() + (x + 0.5) / outWidth * roi.width();
            float s[6], gain[3];
            if (!g.mapCrop(u, v, s, gain)) {
                o[0] = o[1] = o[2] = 0;
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                float px = s[c * 2] * sw, py = s[c * 2 + 1] * sh;
                float val = opts.bicubic ? sampleBicubic(src, px, py, c) : sampleBilinear(src, px, py, c);
                o[c] = val * gain[c];
            }
        }
    }
    return out;
}

} // namespace silma
