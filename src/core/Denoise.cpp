#include "Denoise.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace silma {

namespace {

constexpr float kGamma = 1.0f / 2.4f;
constexpr int kLevels = 5;
// Std of unit white noise in each B3-spline à-trous detail band.
constexpr float kBandNoise[kLevels] = {0.889f, 0.200f, 0.086f, 0.041f, 0.020f};

// One à-trous B3-spline smoothing step with hole size `step`.
PlaneF atrousSmooth(const PlaneF& in, int step)
{
    static const float k[5] = {1.f / 16, 4.f / 16, 6.f / 16, 4.f / 16, 1.f / 16};
    const int w = in.width, h = in.height;
    PlaneF tmp(w, h), out(w, h);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < h; ++y) {
        const float* s = in.row(y);
        float* d = tmp.row(y);
        for (int x = 0; x < w; ++x) {
            float acc = 0;
            for (int i = -2; i <= 2; ++i) {
                int xx = x + i * step;
                xx = xx < 0 ? -xx : (xx >= w ? 2 * w - 2 - xx : xx); // mirror
                xx = std::clamp(xx, 0, w - 1);
                acc += k[i + 2] * s[xx];
            }
            d[x] = acc;
        }
    }
#pragma omp parallel for schedule(static)
    for (int y = 0; y < h; ++y) {
        float* d = out.row(y);
        const float* rows[5];
        for (int i = -2; i <= 2; ++i) {
            int yy = y + i * step;
            yy = yy < 0 ? -yy : (yy >= h ? 2 * h - 2 - yy : yy);
            rows[i + 2] = tmp.row(std::clamp(yy, 0, h - 1));
        }
        for (int x = 0; x < w; ++x)
            d[x] = k[0] * rows[0][x] + k[1] * rows[1][x] + k[2] * rows[2][x] + k[3] * rows[3][x] + k[4] * rows[4][x];
    }
    return out;
}

float medianAbs(std::vector<float>& v)
{
    if (v.empty())
        return 0;
    auto mid = v.begin() + v.size() / 2;
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

// Wavelet shrinkage of a plane. thresholds[l] multiply sigma * bandNoise[l].
void waveletShrink(PlaneF& p, float sigma, const float* thresholds, int levels)
{
    PlaneF cur = p;
    PlaneF result(p.width, p.height, 0.0f);
    for (int l = 0; l < levels; ++l) {
        PlaneF smooth = atrousSmooth(cur, 1 << l);
        float t = sigma * kBandNoise[l] * thresholds[l];
#pragma omp parallel for schedule(static)
        for (int i = 0; i < int(cur.data.size()); ++i) {
            float d = cur.data[i] - smooth.data[i];
            // Soft threshold with a smooth knee (less "plasticky" than hard soft-thresholding).
            float a = std::abs(d);
            float g = a > t ? (a - t) / a : 0.0f;
            if (t > 0 && a > 0)
                g = std::max(g, (a * a) / (a * a + 4 * t * t) * 0.15f);
            result.data[i] += d * (t > 0 ? g : 1.0f);
        }
        cur = std::move(smooth);
    }
#pragma omp parallel for schedule(static)
    for (int i = 0; i < int(p.data.size()); ++i)
        p.data[i] = result.data[i] + cur.data[i];
}

} // namespace

float estimateNoiseSigma(const PlaneF& p)
{
    PlaneF s = atrousSmooth(p, 1);
    std::vector<float> d;
    d.reserve(p.data.size() / 4 + 1);
    // Subsample for speed.
    for (size_t i = 0; i < p.data.size(); i += 4)
        d.push_back(std::abs(p.data[i] - s.data[i]));
    return medianAbs(d) / 0.6745f / kBandNoise[0];
}

void denoise(ImageF& img, const DevelopSettings& s, double pxScale)
{
    const float lum = float(s.nrLuminance / 100.0), col = float(s.nrColor / 100.0);
    if ((lum <= 0 && col <= 0) || img.width < 16 || img.height < 16)
        return;
    // Downscaled previews average noise away; skip below ~1/3 scale.
    if (pxScale < 0.3)
        return;
    const int w = img.width, h = img.height;
    PlaneF Y(w, h), Cb(w, h), Cr(w, h);
#pragma omp parallel for schedule(static)
    for (int i = 0; i < w * h; ++i) {
        const float* px = img.data.data() + size_t(i) * 3;
        float r = std::pow(std::max(px[0], 0.0f), kGamma), g = std::pow(std::max(px[1], 0.0f), kGamma), b = std::pow(std::max(px[2], 0.0f), kGamma);
        float y = 0.25f * r + 0.5f * g + 0.25f * b;
        Y.data[i] = y;
        Cb.data[i] = b - y;
        Cr.data[i] = r - y;
    }
    // Fewer levels at reduced scale: coarse noise is already averaged out.
    int levels = pxScale >= 0.75 ? kLevels : 3;
    if (lum > 0) {
        float sigma = estimateNoiseSigma(Y);
        float detail = float(s.nrLumaDetail / 100.0);
        float th[kLevels];
        for (int l = 0; l < kLevels; ++l) {
            // Strength scales thresholds; detail relaxes the finest bands.
            float base = 3.0f * lum;
            float relax = l == 0 ? 1.0f - 0.6f * detail : (l == 1 ? 1.0f - 0.3f * detail : 1.0f);
            th[l] = base * relax * (l >= 3 ? 0.7f : 1.0f);
        }
        waveletShrink(Y, sigma, th, levels);
    }
    if (col > 0) {
        float th[kLevels];
        for (int l = 0; l < kLevels; ++l)
            th[l] = 6.0f * col * (l < 3 ? 1.0f : 0.8f);
        float sb = estimateNoiseSigma(Cb), sr = estimateNoiseSigma(Cr);
        waveletShrink(Cb, sb, th, levels);
        waveletShrink(Cr, sr, th, levels);
    }
#pragma omp parallel for schedule(static)
    for (int i = 0; i < w * h; ++i) {
        float y = Y.data[i], b = Cb.data[i] + y, r = Cr.data[i] + y;
        float g = (y - 0.25f * r - 0.25f * b) / 0.5f;
        float* px = img.data.data() + size_t(i) * 3;
        // Back to linear (sign-preserving for slight negatives).
        auto lin = [](float v) { return v > 0 ? std::pow(v, 2.4f) : 0.0f; };
        px[0] = lin(r);
        px[1] = lin(g);
        px[2] = lin(b);
    }
}

} // namespace silma
