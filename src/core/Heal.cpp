#include "Heal.h"

#include "AiDenoise.h"
#include "Batch.h"
#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace silma {

namespace {

constexpr int kModelSize = 512;
constexpr double kFeather = 0.25; // feather width as a fraction of the radius

} // namespace

HealPatch generateHealPatch(const RawImage& raw, const HealSpot& spot, QString* providerUsed)
{
    if (raw.halfSize)
        throw std::runtime_error("Heal needs a full-resolution decode");
    const int W = raw.image.width, H = raw.image.height;
    const double cx = spot.x * W, cy = spot.y * H, r = spot.radius * std::max(W, H);
    if (r < 1)
        throw std::runtime_error("Heal spot too small");
    // Context around the spot: about 3x the diameter, at least the model size.
    int S = int(std::ceil(std::max(double(kModelSize), r * 2 * 3.0)));
    S = std::min(S, std::min(W, H));
    int x0 = std::clamp(int(std::round(cx - S / 2.0)), 0, W - S), y0 = std::clamp(int(std::round(cy - S / 2.0)), 0, H - S);
    QRect rect(x0, y0, S, S);

    ImageF crop(S, S);
    for (int y = 0; y < S; ++y)
        std::copy_n(raw.image.px(x0, y0 + y), S * 3, crop.row(y));
    // Display-like encoding adapted to the local brightness (invertible).
    double mean = 0;
    for (float v : crop.data)
        mean += v;
    mean = std::max(1e-4, mean / crop.data.size());
    const double gain = 0.18 / mean;
    auto enc = [&](float v) { return std::pow(std::clamp(float(v * gain), 0.0f, 16.0f) / 16.0f, 1.0f / 4.0f); };
    auto dec = [&](float e) { return float(std::pow(std::clamp(e, 0.0f, 1.0f), 4.0f) * 16.0f / gain); };
    ImageF small = resize(crop, kModelSize, kModelSize);
    for (float& v : small.data)
        v = enc(v);
    PlaneF mask(kModelSize, kModelSize, 0.0f);
    const double k = double(kModelSize) / S;
    const double mcx = (cx - x0) * k, mcy = (cy - y0) * k, mr = r * k * (1 + kFeather) + 2;
    for (int y = 0; y < kModelSize; ++y)
        for (int x = 0; x < kModelSize; ++x)
            if (std::hypot(x + 0.5 - mcx, y + 0.5 - mcy) <= mr)
                mask.at(x, y) = 1;
    ImageF filled = runAiInpaint(small, mask, providerUsed);
    // Back to the crop resolution and linear values; keep the original outside the mask
    // area (the paste feathers between them).
    for (float& v : filled.data)
        v = dec(v);
    ImageF up = resize(filled, S, S);
    HealPatch p;
    p.rect = rect;
    p.data = std::move(up);
    return p;
}

QByteArray encodePatchData(const ImageF& img)
{
    QByteArray out(qsizetype(img.data.size() * sizeof(_Float16)), Qt::Uninitialized);
    auto* d = reinterpret_cast<_Float16*>(out.data());
    for (size_t i = 0; i < img.data.size(); ++i)
        d[i] = _Float16(std::min(img.data[i], 60000.0f));
    return out;
}

ImageF decodePatchData(const QByteArray& data, int w, int h)
{
    ImageF img(w, h);
    if (data.size() != qsizetype(img.data.size() * sizeof(_Float16)))
        return ImageF();
    auto* s = reinterpret_cast<const _Float16*>(data.constData());
    for (size_t i = 0; i < img.data.size(); ++i)
        img.data[i] = float(s[i]);
    return img;
}

void pasteHealPatch(ImageF& img, double scale, int sensorW, int sensorH, const HealSpot& spot, const HealPatch& patch)
{
    if (patch.data.empty())
        return;
    const double cx = spot.x * sensorW * scale, cy = spot.y * sensorH * scale, r = spot.radius * std::max(sensorW, sensorH) * scale;
    const double rOut = r * (1 + kFeather);
    const int x0 = std::max(0, int(std::floor(cx - rOut))), x1 = std::min(img.width - 1, int(std::ceil(cx + rOut)));
    const int y0 = std::max(0, int(std::floor(cy - rOut))), y1 = std::min(img.height - 1, int(std::ceil(cy + rOut)));
    const double ps = 1.0 / scale; // image px -> sensor px
#pragma omp parallel for schedule(static)
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            if (d >= rOut)
                continue;
            double a = d <= r ? 1.0 : 1.0 - (d - r) / (rOut - r);
            a = a * a * (3 - 2 * a);
            // Bilinear sample of the patch at the corresponding sensor position.
            double sx = (x + 0.5) * ps - patch.rect.x() - 0.5, sy = (y + 0.5) * ps - patch.rect.y() - 0.5;
            if (sx < 0 || sy < 0 || sx > patch.data.width - 1 || sy > patch.data.height - 1)
                continue;
            int ix = int(sx), iy = int(sy);
            int ix1 = std::min(ix + 1, patch.data.width - 1), iy1 = std::min(iy + 1, patch.data.height - 1);
            float tx = float(sx - ix), ty = float(sy - iy);
            float* o = img.px(x, y);
            for (int c = 0; c < 3; ++c) {
                float v = (patch.data.px(ix, iy)[c] * (1 - tx) + patch.data.px(ix1, iy)[c] * tx) * (1 - ty) +
                          (patch.data.px(ix, iy1)[c] * (1 - tx) + patch.data.px(ix1, iy1)[c] * tx) * ty;
                o[c] = float(o[c] * (1 - a) + std::min(v, 1.0f) * a);
            }
        }
}

qint64 storeHealPatch(Batch& b, qint64 photoId, const HealPatch& p)
{
    return b.addPatch(photoId, p.rect, encodePatchData(p.data));
}

QMap<qint64, HealPatch> loadHealPatches(Batch& b, const DevelopSettings& s)
{
    QMap<qint64, HealPatch> out;
    for (const auto& h : s.heals) {
        if (out.contains(h.patchId))
            continue;
        QRect r;
        QByteArray data;
        if (b.patch(h.patchId, &r, &data))
            out.insert(h.patchId, {r, decodePatchData(data, r.width(), r.height())});
    }
    return out;
}

void addHealPatches(DevelopEngine& e, const QMap<qint64, HealPatch>& patches)
{
    for (auto it = patches.begin(); it != patches.end(); ++it)
        if (!e.hasHealPatch(it.key()))
            e.addHealPatch(it.key(), it.value());
}

} // namespace silma
