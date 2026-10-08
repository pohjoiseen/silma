#include "Heal.h"

#include "AiDenoise.h"
#include "Batch.h"
#include "Engine.h"

#include <QRectF>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace silma {

namespace {

constexpr int kModelSize = 512;
constexpr double kFeather = 0.25; // feather width as a fraction of the radius

// A heal's stroke in pixel coordinates of an image covering the sensor.
struct Stroke {
    std::vector<QPointF> pts;
    double r = 0;

    Stroke(const HealSpot& s, double width, double height)
    {
        pts.push_back({s.x * width, s.y * height});
        for (const QPointF& p : s.path)
            pts.push_back({p.x() * width, p.y() * height});
        r = s.radius * std::max(width, height);
    }

    // Bounding box of the stroke widened by `pad` on every side.
    QRectF bounds(double pad) const
    {
        double x0 = pts[0].x(), x1 = x0, y0 = pts[0].y(), y1 = y0;
        for (const QPointF& p : pts) {
            x0 = std::min(x0, p.x());
            x1 = std::max(x1, p.x());
            y0 = std::min(y0, p.y());
            y1 = std::max(y1, p.y());
        }
        return QRectF(QPointF(x0 - pad, y0 - pad), QPointF(x1 + pad, y1 + pad));
    }

    // Distance from p to the stroke's centre line, considering only segments whose
    // bounding box widened by `reach` contains p (others are farther than `reach`).
    double distance(QPointF p, double reach = 1e30) const
    {
        double best = 1e30;
        auto segDist = [&](QPointF a, QPointF b) {
            QPointF ab = b - a, ap = p - a;
            double len2 = ab.x() * ab.x() + ab.y() * ab.y();
            double t = len2 > 0 ? std::clamp((ap.x() * ab.x() + ap.y() * ab.y()) / len2, 0.0, 1.0) : 0.0;
            QPointF d = ap - ab * t;
            return std::hypot(d.x(), d.y());
        };
        if (pts.size() == 1)
            return std::hypot(p.x() - pts[0].x(), p.y() - pts[0].y());
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const QPointF &a = pts[i], &b = pts[i + 1];
            if (p.x() < std::min(a.x(), b.x()) - reach || p.x() > std::max(a.x(), b.x()) + reach || p.y() < std::min(a.y(), b.y()) - reach
                || p.y() > std::max(a.y(), b.y()) + reach)
                continue;
            best = std::min(best, segDist(a, b));
        }
        return best;
    }
};

// Bilinear sample of an image at pixel coordinates (centres at +0.5), clamped to its edges.
void sampleBilinear(const ImageF& img, double x, double y, float* out)
{
    x = std::clamp(x - 0.5, 0.0, double(img.width - 1));
    y = std::clamp(y - 0.5, 0.0, double(img.height - 1));
    int ix = int(x), iy = int(y), ix1 = std::min(ix + 1, img.width - 1), iy1 = std::min(iy + 1, img.height - 1);
    float tx = float(x - ix), ty = float(y - iy);
    for (int c = 0; c < 3; ++c)
        out[c] = (img.px(ix, iy)[c] * (1 - tx) + img.px(ix1, iy)[c] * tx) * (1 - ty) + (img.px(ix, iy1)[c] * (1 - tx) + img.px(ix1, iy1)[c] * tx) * ty;
}

} // namespace

double healExtent(const HealSpot& spot, int sensorWidth, int sensorHeight)
{
    Stroke st(spot, sensorWidth, sensorHeight);
    QRectF b = st.bounds(st.r);
    return std::max(b.width(), b.height()) / std::max(sensorWidth, sensorHeight);
}

HealPatch generateHealPatch(const RawImage& raw, const HealSpot& spot, QString* providerUsed)
{
    if (raw.halfSize)
        throw std::runtime_error("Heal needs a full-resolution decode");
    const int W = raw.image.width, H = raw.image.height;
    const Stroke st(spot, W, H);
    if (st.r < 1)
        throw std::runtime_error("Heal spot too small");
    const QRectF area = st.bounds(st.r);
    const double extent = std::max(area.width(), area.height());
    if (extent > kMaxHealExtent * std::max(W, H) * 1.05)
        throw std::runtime_error("Heal area too large (at most a fifth of the photo's long edge)");
    // Context around the stroke: about 3x its extent, at least the model size.
    int S = int(std::ceil(std::max(double(kModelSize), extent * 3.0)));
    S = std::min(S, std::min(W, H));
    const QPointF c = area.center();
    int x0 = std::clamp(int(std::round(c.x() - S / 2.0)), 0, W - S), y0 = std::clamp(int(std::round(c.y() - S / 2.0)), 0, H - S);

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
    // Mask: the stroke widened by the feather, plus a little for the downscale.
    const double k = double(kModelSize) / S;
    const double reach = st.r * (1 + kFeather) + 2 / k;
    PlaneF mask(kModelSize, kModelSize, 0.0f);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < kModelSize; ++y)
        for (int x = 0; x < kModelSize; ++x)
            if (st.distance({x0 + (x + 0.5) / k, y0 + (y + 0.5) / k}, reach) <= reach)
                mask.at(x, y) = 1;
    ImageF filled = runAiInpaint(small, mask, providerUsed);
    for (float& v : filled.data)
        v = dec(v);
    // Keep only the part that gets pasted (the stroke and its feather), at full resolution.
    const QRect keep = st.bounds(st.r * (1 + kFeather) + 2).toAlignedRect().intersected(QRect(x0, y0, S, S));
    HealPatch p;
    p.rect = keep;
    p.data = ImageF(keep.width(), keep.height());
#pragma omp parallel for schedule(static)
    for (int y = 0; y < keep.height(); ++y)
        for (int x = 0; x < keep.width(); ++x)
            sampleBilinear(filled, (keep.x() + x + 0.5 - x0) * k, (keep.y() + y + 0.5 - y0) * k, p.data.px(x, y));
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
    const Stroke st(spot, sensorW * scale, sensorH * scale);
    const double r = st.r, rOut = r * (1 + kFeather);
    const QRect area = st.bounds(rOut).toAlignedRect().intersected(QRect(0, 0, img.width, img.height));
    const double ps = 1.0 / scale; // image px -> sensor px
#pragma omp parallel for schedule(dynamic, 8)
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x) {
            double d = st.distance({x + 0.5, y + 0.5}, rOut);
            if (d >= rOut)
                continue;
            double a = d <= r ? 1.0 : 1.0 - (d - r) / (rOut - r);
            a = a * a * (3 - 2 * a);
            // The patch pixel at the corresponding sensor position.
            double sx = (x + 0.5) * ps - patch.rect.x(), sy = (y + 0.5) * ps - patch.rect.y();
            if (sx < 0 || sy < 0 || sx > patch.data.width || sy > patch.data.height)
                continue;
            float v[3];
            sampleBilinear(patch.data, sx, sy, v);
            float* o = img.px(x, y);
            for (int c = 0; c < 3; ++c)
                o[c] = float(o[c] * (1 - a) + std::min(v[c], 1.0f) * a);
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
