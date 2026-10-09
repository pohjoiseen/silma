#include "Image.h"

#include <algorithm>
#include <cmath>

namespace silma {

ImageF downscale(const ImageF& src, int factor)
{
    if (factor <= 1)
        return src;
    ImageF dst(src.width / factor, src.height / factor);
    const float norm = 1.0f / float(factor * factor);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < dst.height; ++y) {
        float* out = dst.row(y);
        for (int x = 0; x < dst.width; ++x) {
            float r = 0, g = 0, b = 0;
            for (int dy = 0; dy < factor; ++dy) {
                const float* in = src.px(x * factor, y * factor + dy);
                for (int dx = 0; dx < factor; ++dx, in += 3) {
                    r += in[0];
                    g += in[1];
                    b += in[2];
                }
            }
            out[x * 3 + 0] = r * norm;
            out[x * 3 + 1] = g * norm;
            out[x * 3 + 2] = b * norm;
        }
    }
    return dst;
}

ImageF resize(const ImageF& src, int width, int height)
{
    if (width == src.width && height == src.height)
        return src;
    ImageF dst(width, height);
    const double sx = double(src.width) / width, sy = double(src.height) / height;
    if (sx >= 1.0 && sy >= 1.0) {
        // Area averaging: each destination pixel integrates its source footprint.
#pragma omp parallel for schedule(static)
        for (int y = 0; y < height; ++y) {
            double y0 = y * sy, y1 = (y + 1) * sy;
            for (int x = 0; x < width; ++x) {
                double x0 = x * sx, x1 = (x + 1) * sx;
                double acc[3] = {0, 0, 0}, wsum = 0;
                for (int iy = int(y0); iy < std::min(int(std::ceil(y1)), src.height); ++iy) {
                    double wy = std::min(y1, iy + 1.0) - std::max(y0, double(iy));
                    for (int ix = int(x0); ix < std::min(int(std::ceil(x1)), src.width); ++ix) {
                        double w = wy * (std::min(x1, ix + 1.0) - std::max(x0, double(ix)));
                        const float* p = src.px(ix, iy);
                        acc[0] += w * p[0];
                        acc[1] += w * p[1];
                        acc[2] += w * p[2];
                        wsum += w;
                    }
                }
                float* o = dst.px(x, y);
                for (int c = 0; c < 3; ++c)
                    o[c] = float(acc[c] / wsum);
            }
        }
    } else {
#pragma omp parallel for schedule(static)
        for (int y = 0; y < height; ++y) {
            float fy = std::clamp(float((y + 0.5) * sy - 0.5), 0.0f, float(src.height - 1));
            int y0 = int(fy), y1 = std::min(y0 + 1, src.height - 1);
            float ty = fy - y0;
            for (int x = 0; x < width; ++x) {
                float fx = std::clamp(float((x + 0.5) * sx - 0.5), 0.0f, float(src.width - 1));
                int x0 = int(fx), x1 = std::min(x0 + 1, src.width - 1);
                float tx = fx - x0;
                const float *a = src.px(x0, y0), *b = src.px(x1, y0), *c = src.px(x0, y1), *d = src.px(x1, y1);
                float* o = dst.px(x, y);
                for (int k = 0; k < 3; ++k)
                    o[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
            }
        }
    }
    return dst;
}

namespace {

// Lanczos-3 weights of the source samples for each destination index along one axis.
struct Taps {
    std::vector<int> first, count;
    std::vector<float> weights; // count[i] weights from offset[i]
    std::vector<size_t> offset;
};

Taps lanczosTaps(int srcLen, int dstLen)
{
    auto sinc = [](double x) { return std::abs(x) < 1e-9 ? 1.0 : std::sin(M_PI * x) / (M_PI * x); };
    const double scale = double(srcLen) / dstLen; // >= 1: the kernel widens with the factor
    const double support = 3.0 * scale;
    Taps t;
    t.first.resize(dstLen);
    t.count.resize(dstLen);
    t.offset.resize(dstLen);
    for (int i = 0; i < dstLen; ++i) {
        const double center = (i + 0.5) * scale - 0.5;
        const int lo = int(std::floor(center - support)) + 1, hi = int(std::ceil(center + support)) - 1;
        t.first[i] = lo;
        t.count[i] = hi - lo + 1;
        t.offset[i] = t.weights.size();
        double sum = 0;
        for (int j = lo; j <= hi; ++j) {
            const double d = (j - center) / scale;
            const double w = std::abs(d) < 3.0 ? sinc(d) * sinc(d / 3.0) : 0.0;
            t.weights.push_back(float(w));
            sum += w;
        }
        for (int k = 0; k < t.count[i]; ++k)
            t.weights[t.offset[i] + k] /= float(sum);
    }
    return t;
}

} // namespace

ImageF resizeLanczos(const ImageF& src, int width, int height)
{
    if (width > src.width || height > src.height)
        return resize(src, width, height);
    if (width == src.width && height == src.height)
        return src;
    const Taps tx = lanczosTaps(src.width, width), ty = lanczosTaps(src.height, height);
    // Horizontal pass, then vertical; edges clamp.
    ImageF mid(width, src.height);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < src.height; ++y)
        for (int x = 0; x < width; ++x) {
            float acc[3] = {0, 0, 0};
            for (int k = 0; k < tx.count[x]; ++k) {
                const float w = tx.weights[tx.offset[x] + k];
                const float* p = src.px(std::clamp(tx.first[x] + k, 0, src.width - 1), y);
                acc[0] += w * p[0];
                acc[1] += w * p[1];
                acc[2] += w * p[2];
            }
            std::copy_n(acc, 3, mid.px(x, y));
        }
    ImageF dst(width, height);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            float acc[3] = {0, 0, 0};
            for (int k = 0; k < ty.count[y]; ++k) {
                const float w = ty.weights[ty.offset[y] + k];
                const float* p = mid.px(x, std::clamp(ty.first[y] + k, 0, src.height - 1));
                acc[0] += w * p[0];
                acc[1] += w * p[1];
                acc[2] += w * p[2];
            }
            std::copy_n(acc, 3, dst.px(x, y));
        }
    return dst;
}

ImageF applyOrientation(const ImageF& src, int orientation)
{
    if (orientation <= 1 || orientation > 8)
        return src;
    const bool transpose = orientation >= 5;
    ImageF dst = transpose ? ImageF(src.height, src.width) : ImageF(src.width, src.height);
    // Maps destination (x, y) to source coordinates per EXIF orientation semantics.
#pragma omp parallel for schedule(static)
    for (int y = 0; y < dst.height; ++y) {
        for (int x = 0; x < dst.width; ++x) {
            int sx, sy;
            switch (orientation) {
            case 2: sx = src.width - 1 - x; sy = y; break;
            case 3: sx = src.width - 1 - x; sy = src.height - 1 - y; break;
            case 4: sx = x; sy = src.height - 1 - y; break;
            case 5: sx = y; sy = x; break;
            case 6: sx = y; sy = src.height - 1 - x; break;
            case 7: sx = src.width - 1 - y; sy = src.height - 1 - x; break;
            case 8: sx = src.width - 1 - y; sy = x; break;
            default: sx = x; sy = y; break;
            }
            const float* s = src.px(sx, sy);
            float* d = dst.px(x, y);
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
    return dst;
}

std::shared_ptr<const ValidMask> makeValidMask(const std::vector<uint8_t>& mask, int width, int height)
{
    auto valid = std::make_shared<ValidMask>();
    const int cell = std::max(1, (std::max(width, height) + 2047) / 2048);
    valid->width = (width + cell - 1) / cell;
    valid->height = (height + cell - 1) / cell;
    valid->data.assign(size_t(valid->width) * valid->height, 1);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (mask[size_t(y) * width + x] < 128)
                valid->data[size_t(y / cell) * valid->width + x / cell] = 0;
    return valid;
}

} // namespace silma
