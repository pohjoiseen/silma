#include "Filters.h"

#include <algorithm>
#include <cmath>

namespace silma {

namespace {

// 1-D running box mean along rows (horizontal) for a plane.
void boxRows(const PlaneF& in, PlaneF& out, int r)
{
    const int w = in.width;
#pragma omp parallel for schedule(static)
    for (int y = 0; y < in.height; ++y) {
        const float* s = in.row(y);
        float* d = out.row(y);
        double acc = 0;
        int lo = 0, hi = -1; // current window [lo, hi]
        for (int x = 0; x < w; ++x) {
            int nlo = std::max(0, x - r), nhi = std::min(w - 1, x + r);
            while (hi < nhi)
                acc += s[++hi];
            while (lo < nlo)
                acc -= s[lo++];
            d[x] = float(acc / (hi - lo + 1));
        }
    }
}

void boxCols(const PlaneF& in, PlaneF& out, int r)
{
    const int w = in.width, h = in.height;
    // Process in column strips for cache friendliness.
    constexpr int strip = 64;
#pragma omp parallel for schedule(static)
    for (int x0 = 0; x0 < w; x0 += strip) {
        int x1 = std::min(w, x0 + strip);
        std::vector<double> acc(x1 - x0, 0.0);
        int lo = 0, hi = -1;
        for (int y = 0; y < h; ++y) {
            int nlo = std::max(0, y - r), nhi = std::min(h - 1, y + r);
            while (hi < nhi) {
                ++hi;
                const float* s = in.row(hi);
                for (int x = x0; x < x1; ++x)
                    acc[x - x0] += s[x];
            }
            while (lo < nlo) {
                const float* s = in.row(lo);
                for (int x = x0; x < x1; ++x)
                    acc[x - x0] -= s[x];
                ++lo;
            }
            float inv = 1.0f / float(hi - lo + 1);
            float* d = out.row(y);
            for (int x = x0; x < x1; ++x)
                d[x] = float(acc[x - x0]) * inv;
        }
    }
}

} // namespace

PlaneF boxMean(const PlaneF& in, int r)
{
    if (r <= 0)
        return in;
    PlaneF tmp(in.width, in.height), out(in.width, in.height);
    boxRows(in, tmp, r);
    boxCols(tmp, out, r);
    return out;
}

PlaneF gaussianBlur(const PlaneF& in, float sigma)
{
    if (sigma <= 0.05f)
        return in;
    if (sigma > 3.0f) {
        // Three box passes approximate a gaussian (box radius from the variance).
        int r = std::max(1, int(std::round((std::sqrt(4.0 * sigma * sigma + 1.0) - 1.0) / 2.0)));
        return boxMean(boxMean(boxMean(in, r), r), r);
    }
    int r = int(std::ceil(sigma * 3));
    std::vector<float> k(2 * r + 1);
    float sum = 0;
    for (int i = -r; i <= r; ++i)
        sum += k[i + r] = std::exp(-0.5f * i * i / (sigma * sigma));
    for (float& v : k)
        v /= sum;
    PlaneF tmp(in.width, in.height), out(in.width, in.height);
    const int w = in.width, h = in.height;
#pragma omp parallel for schedule(static)
    for (int y = 0; y < h; ++y) {
        const float* s = in.row(y);
        float* d = tmp.row(y);
        for (int x = 0; x < w; ++x) {
            float acc = 0;
            for (int i = -r; i <= r; ++i)
                acc += k[i + r] * s[std::clamp(x + i, 0, w - 1)];
            d[x] = acc;
        }
    }
#pragma omp parallel for schedule(static)
    for (int y = 0; y < h; ++y) {
        float* d = out.row(y);
        for (int x = 0; x < w; ++x)
            d[x] = 0;
        for (int i = -r; i <= r; ++i) {
            const float* s = tmp.row(std::clamp(y + i, 0, h - 1));
            float kv = k[i + r];
            for (int x = 0; x < w; ++x)
                d[x] += kv * s[x];
        }
    }
    return out;
}

void guidedFilterCoefficients(const PlaneF& I, int r, float eps, PlaneF& meanA, PlaneF& meanB)
{
    PlaneF I2(I.width, I.height);
    for (size_t i = 0; i < I.data.size(); ++i)
        I2.data[i] = I.data[i] * I.data[i];
    PlaneF mI = boxMean(I, r), mI2 = boxMean(I2, r);
    PlaneF a(I.width, I.height), b(I.width, I.height);
    for (size_t i = 0; i < I.data.size(); ++i) {
        float var = std::max(0.0f, mI2.data[i] - mI.data[i] * mI.data[i]);
        a.data[i] = var / (var + eps);
        b.data[i] = mI.data[i] - a.data[i] * mI.data[i];
    }
    meanA = boxMean(a, r);
    meanB = boxMean(b, r);
}

CurveLut::CurveLut(const QList<QPointF>& ptsIn)
{
    QList<QPointF> pts = ptsIn;
    std::sort(pts.begin(), pts.end(), [](auto& a, auto& b) { return a.x() < b.x(); });
    // Drop near-duplicate x positions.
    QList<QPointF> p;
    for (const auto& q : pts)
        if (p.isEmpty() || q.x() - p.last().x() > 1e-4)
            p.append(q);
    if (p.isEmpty() || (p.size() == 2 && p[0] == QPointF(0, 0) && p[1] == QPointF(1, 1))) {
        m_identity = true;
        return;
    }
    if (p.first().x() > 0)
        p.prepend(QPointF(0, 0));
    if (p.last().x() < 1)
        p.append(QPointF(1, 1));
    bool ident = true;
    for (const auto& q : p)
        if (std::abs(q.x() - q.y()) > 1e-6)
            ident = false;
    if (ident) {
        m_identity = true;
        return;
    }
    m_identity = false;
    const int n = int(p.size());
    std::vector<double> xs(n), ys(n), d(n - 1), m(n);
    for (int i = 0; i < n; ++i) {
        xs[i] = p[i].x();
        ys[i] = p[i].y();
    }
    for (int i = 0; i < n - 1; ++i)
        d[i] = (ys[i + 1] - ys[i]) / (xs[i + 1] - xs[i]);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (int i = 1; i < n - 1; ++i)
        m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2;
    for (int i = 0; i < n - 1; ++i) {
        if (d[i] == 0) {
            m[i] = m[i + 1] = 0;
            continue;
        }
        double a = m[i] / d[i], b = m[i + 1] / d[i];
        double s = a * a + b * b;
        if (s > 9) {
            double t = 3 / std::sqrt(s);
            m[i] = t * a * d[i];
            m[i + 1] = t * b * d[i];
        }
    }
    m_lut.resize(kSize);
    int seg = 0;
    for (int k = 0; k < kSize; ++k) {
        double x = double(k) / (kSize - 1);
        while (seg < n - 2 && x > xs[seg + 1])
            ++seg;
        double h = xs[seg + 1] - xs[seg];
        double t = std::clamp((x - xs[seg]) / h, 0.0, 1.0);
        double t2 = t * t, t3 = t2 * t;
        double y = (2 * t3 - 3 * t2 + 1) * ys[seg] + (t3 - 2 * t2 + t) * h * m[seg] + (-2 * t3 + 3 * t2) * ys[seg + 1] + (t3 - t2) * h * m[seg + 1];
        m_lut[k] = float(std::clamp(y, 0.0, 1.0));
    }
}

} // namespace silma
