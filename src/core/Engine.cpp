#include "Engine.h"

#include "Denoise.h"
#include "Filters.h"
#include "Heal.h"
#include "Metadata.h"

#include <QFileInfo>
#include <algorithm>
#include <cmath>

namespace silma {

namespace {

constexpr float kGrey = 0.18f;
constexpr int kContextLongEdge = 768;

inline float smoothstep(float e0, float e1, float x)
{
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// Global tone curve: scene-linear -> display-linear, parameterized by contrast, whites, blacks.
// A log-logistic curve normalized to reach 1.0 at the white point, tabulated over log2(x).
class ToneCurve {
public:
    static constexpr float kMinEv = -16, kMaxEv = 8;
    static constexpr int kSize = 4096;

    explicit ToneCurve(const DevelopSettings& s)
    {
        double c = 1.4 * std::pow(2.0, s.contrast / 100.0 * 0.6);
        double whiteEv = 2.5 - s.whites / 100.0 * 1.25;
        m_white = kGrey * std::pow(2.0, whiteEv);
        double blackOff = -s.blacks / 100.0 * 0.006;
        const double gOut = 0.18; // middle grey stays middle grey (sRGB ~118)
        double a = std::pow(double(kGrey), c), b = std::pow(m_white, c);
        double S = a * b * (1 - gOut) / (gOut * b - a);
        auto f = [&](double x) {
            double xc = std::pow(std::max(x, 0.0), c);
            return xc / (xc + S);
        };
        double fw = f(m_white);
        m_lut.resize(kSize);
        for (int i = 0; i < kSize; ++i) {
            double ev = kMinEv + (kMaxEv - kMinEv) * i / (kSize - 1);
            double x = std::pow(2.0, ev);
            // Black point shift keeps the white point fixed.
            double xb = (x - blackOff) / (1 - blackOff / m_white);
            m_lut[i] = float(std::clamp(f(xb) / fw, 0.0, 1.0));
        }
        m_zero = float(std::clamp(f(-blackOff / (1 - blackOff / m_white)) / fw, 0.0, 1.0));
    }

    float operator()(float x) const
    {
        if (x <= 0)
            return m_zero;
        float ev = std::log2(x);
        float f = (ev - kMinEv) / (kMaxEv - kMinEv) * (kSize - 1);
        if (f <= 0)
            return std::max(m_zero, m_lut[0]);
        if (f >= kSize - 1)
            return m_lut[kSize - 1];
        int i = int(f);
        float t = f - i;
        return m_lut[i] * (1 - t) + m_lut[i + 1] * t;
    }
    // Same curve evaluated from log2 luminance, returning display-encoded value (tone position).
    float positionFromLog(float ev) const { return srgbEncode((*this)(std::exp2(ev))); }
    double whitePoint() const { return m_white; }

private:
    std::vector<float> m_lut;
    float m_zero = 0;
    double m_white = 1;
};

QString geometryKey(const DevelopSettings& s, bool ignoreCrop)
{
    return QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12,%13")
        .arg(s.quarterTurns).arg(s.lensDistortion).arg(s.lensVignetting).arg(s.perspVertical).arg(s.perspHorizontal).arg(s.rotate).arg(s.scale)
        .arg(ignoreCrop ? 0 : s.crop.x()).arg(ignoreCrop ? 0 : s.crop.y()).arg(ignoreCrop ? 1 : s.crop.width()).arg(ignoreCrop ? 1 : s.crop.height())
        .arg(ignoreCrop).arg(s.processVersion);
}

PlaneF luminanceLog(const ImageF& img)
{
    PlaneF L(img.width, img.height);
    const Vec3& w = color::kRec2020Luma;
#pragma omp parallel for schedule(static)
    for (int y = 0; y < img.height; ++y) {
        const float* p = img.row(y);
        float* l = L.row(y);
        for (int x = 0; x < img.width; ++x, p += 3)
            l[x] = std::log2(std::max(float(w[0] * p[0] + w[1] * p[1] + w[2] * p[2]), 1e-6f));
    }
    return L;
}

} // namespace

double defaultBaselineExposure(const RawImage& raw)
{
    // Raw files are exposed with highlight headroom; camera JPEGs push the midtones up.
    // DNGs carry their own BaselineExposure which is added separately.
    (void)raw;
    return 0.8;
}

DevelopEngine::DevelopEngine(const QString& path, const EngineLoadOptions& opts) : m_path(path)
{
    m_raw = std::make_shared<RawImage>(decodeRaw(path, {.halfSize = opts.halfSize}));
    m_levelOffset = m_raw->halfSize ? 1 : 0;
    m_color = std::make_unique<CameraColor>(m_raw->color);

    LensQuery q;
    q.cameraMake = m_raw->make;
    q.cameraModel = m_raw->model;
    double focal35 = 0;
    try {
        MetadataSummary md = readMetadataSummary(path);
        q.lens = md.lens;
        q.focalLength = md.focalLength;
        q.aperture = md.fNumber;
        focal35 = md.focalLength35;
        if (focal35 <= 0 && md.focalLength > 0)
            focal35 = md.focalLength * 1.5;
    } catch (...) {
    }
    m_lens = LensCorrection::build(*m_raw, q);
    m_geoSrc.sensorWidth = m_raw->fullWidth;
    m_geoSrc.sensorHeight = m_raw->fullHeight;
    if (m_raw->cropW > 0 && m_raw->cropH > 0)
        m_geoSrc.frame = QRectF(double(m_raw->cropX) / m_raw->fullWidth, double(m_raw->cropY) / m_raw->fullHeight,
                                double(m_raw->cropW) / m_raw->fullWidth, double(m_raw->cropH) / m_raw->fullHeight);
    m_geoSrc.orientation = m_raw->orientation;
    m_geoSrc.focalLength35 = focal35;
    m_geoSrc.lens = m_lens;
    m_geoSrc.valid = m_raw->valid;
    m_pyramid.resize(8);
}

std::shared_ptr<const ImageF> DevelopEngine::level(int lvl) const
{
    std::lock_guard lock(m_mutex);
    int idx = std::max(0, lvl - m_levelOffset);
    if (!m_pyramid[0])
        m_pyramid[0] = std::shared_ptr<ImageF>(m_raw, &m_raw->image); // aliases the decoded image
    if (!m_pyramid[idx]) {
        // Build missing levels from the previous one (box 2x).
        for (int i = 1; i <= idx; ++i)
            if (!m_pyramid[i])
                m_pyramid[i] = std::make_shared<ImageF>(downscale(*m_pyramid[i - 1], 2));
    }
    return m_pyramid[idx];
}

void DevelopEngine::addHealPatch(qint64 id, const HealPatch& patch)
{
    std::lock_guard lock(m_mutex);
    m_patches[id] = std::make_shared<HealPatch>(patch);
}

bool DevelopEngine::hasHealPatch(qint64 id) const
{
    std::lock_guard lock(m_mutex);
    return m_patches.count(id) > 0;
}

void DevelopEngine::setSensorOverlay(std::shared_ptr<const HealPatch> overlay)
{
    std::lock_guard lock(m_mutex);
    m_overlay = std::move(overlay);
    ++m_overlayGeneration;
}

void DevelopEngine::applyHeals(const DevelopSettings& s) const
{
    QString key;
    {
        std::lock_guard lock(m_mutex);
        if (m_overlay)
            key = QStringLiteral("overlay %1;").arg(m_overlayGeneration);
    }
    for (const auto& h : s.heals) {
        key += QStringLiteral("%1,%2,%3,%4").arg(h.x).arg(h.y).arg(h.radius).arg(h.patchId);
        for (const QPointF& p : h.path)
            key += QStringLiteral(",%1:%2").arg(p.x()).arg(p.y());
        key += ';';
    }
    std::lock_guard lock(m_mutex);
    if (key == m_healKey)
        return;
    m_healKey = key;
    std::shared_ptr<ImageF> base;
    if (key.isEmpty()) {
        base = std::shared_ptr<ImageF>(m_raw, &m_raw->image);
    } else {
        base = std::make_shared<ImageF>(m_raw->image);
        if (m_overlay && !m_raw->halfSize) {
            const QRect r = m_overlay->rect.intersected(QRect(0, 0, base->width, base->height));
            for (int y = r.top(); y <= r.bottom(); ++y)
                std::copy_n(m_overlay->data.px(r.left() - m_overlay->rect.left(), y - m_overlay->rect.top()), size_t(r.width()) * 3, base->px(r.left(), y));
        }
        double scale = m_raw->halfSize ? double(m_raw->image.width) / m_raw->fullWidth : 1.0;
        for (const auto& h : s.heals) {
            auto it = m_patches.find(h.patchId);
            if (it != m_patches.end())
                pasteHealPatch(*base, scale, m_raw->fullWidth, m_raw->fullHeight, h, *it->second, m_raw->color.appliedMul);
        }
    }
    // Readers holding the old levels keep them alive through their shared pointers.
    for (auto& l : m_pyramid)
        l.reset();
    m_pyramid[0] = base;
    m_ctxCache = {};
    m_mainCache = {};
}

int DevelopEngine::levelFor(double srcPxPerOutPx) const
{
    int lvl = srcPxPerOutPx > 1 ? int(std::floor(std::log2(srcPxPerOutPx))) : 0;
    return std::clamp(lvl, m_levelOffset, 7 + m_levelOffset);
}

WbValues DevelopEngine::asShotWb() const
{
    WbValues w;
    w.mul = m_raw->color.asShotMul;
    m_color->tempTintFor(w.mul, w.temperature, w.tint);
    return w;
}

WbValues DevelopEngine::wbFromTempTint(double temperature, double tint) const
{
    WbValues w;
    w.temperature = temperature;
    w.tint = tint;
    w.mul = m_color->multipliersFor(temperature, tint);
    return w;
}

std::optional<WbValues> DevelopEngine::wbFromPoint(const DevelopSettings& s, QPointF cropPoint) const
{
    float src[6], gain[3];
    if (!geometry(s).mapCrop(cropPoint.x(), cropPoint.y(), src, gain))
        return std::nullopt;
    // Average a small patch (about 1/400 of the long edge across) to smooth out noise.
    const int lvl = levelFor(std::max(m_raw->fullWidth, m_raw->fullHeight) / 2000.0);
    auto imgPtr = level(lvl);
    const ImageF& img = *imgPtr;
    const int cx = int(src[2] * img.width), cy = int(src[3] * img.height), r = 2;
    double sum[3] = {0, 0, 0};
    int n = 0;
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x) {
            if (x < 0 || y < 0 || x >= img.width || y >= img.height)
                continue;
            const float* p = img.px(x, y);
            if (std::max({p[0], p[1], p[2]}) > 0.95f)
                return std::nullopt; // clipped: the colour is unknown
            for (int c = 0; c < 3; ++c)
                sum[c] += p[c];
            ++n;
        }
    if (n == 0)
        return std::nullopt;
    // Back to raw camera values (the decoded data is white-balanced with appliedMul).
    const Vec3 applied = m_raw->color.appliedMul;
    double cam[3];
    for (int c = 0; c < 3; ++c)
        cam[c] = sum[c] / n / applied[c];
    if (std::min({cam[0], cam[1], cam[2]}) < 0.002)
        return std::nullopt; // too dark to tell
    WbValues w;
    w.mul = {cam[1] / cam[0], 1.0, cam[1] / cam[2]};
    m_color->tempTintFor(w.mul, w.temperature, w.tint);
    w.temperature = std::clamp(w.temperature, 2000.0, 50000.0);
    w.tint = std::clamp(w.tint, -150.0, 150.0);
    w.mul = m_color->multipliersFor(w.temperature, w.tint);
    return w;
}

WbValues DevelopEngine::autoWb() const
{
    {
        std::lock_guard lock(m_mutex);
        if (m_autoWb)
            return *m_autoWb;
    }
    // Grey-world on near-neutral, well-exposed pixels in log-chroma space, iteratively
    // narrowing the neutral window around the current estimate.
    int lvl = levelFor(std::max(m_raw->fullWidth, m_raw->fullHeight) / 512.0);
    auto imgPtr = level(lvl);
    const ImageF& img = *imgPtr;
    const Vec3 applied = m_raw->color.appliedMul;
    std::vector<std::array<float, 3>> samples;
    samples.reserve(img.pixelCount());
    for (size_t i = 0; i < img.pixelCount(); ++i) {
        const float* p = img.data.data() + i * 3;
        if (std::max({p[0], p[1], p[2]}) > 0.95f || std::min({p[0], p[1], p[2]}) < 0.005f)
            continue;
        // Back to raw camera values (un-white-balanced).
        float r = p[0] / float(applied[0]), g = p[1] / float(applied[1]), b = p[2] / float(applied[2]);
        samples.push_back({std::log(r / g), std::log(b / g), std::log(g)});
    }
    WbValues w = asShotWb();
    if (samples.size() > 100) {
        // Start from the as-shot estimate.
        double er = -std::log(w.mul[0]), eb = -std::log(w.mul[2]);
        for (double sigma : {0.8, 0.5, 0.3, 0.2, 0.15}) {
            double sr = 0, sb = 0, sw = 0;
            for (const auto& s : samples) {
                double dr = s[0] - er, db = s[1] - eb;
                double wgt = std::exp(-(dr * dr + db * db) / (2 * sigma * sigma)) * std::exp(0.5 * s[2]);
                sr += wgt * s[0];
                sb += wgt * s[1];
                sw += wgt;
            }
            if (sw <= 0)
                break;
            er = sr / sw;
            eb = sb / sw;
        }
        w.mul = {std::exp(-er), 1.0, std::exp(-eb)};
        m_color->tempTintFor(w.mul, w.temperature, w.tint);
        // Re-derive multipliers from the clamped temperature/tint for consistency.
        w.temperature = std::clamp(w.temperature, 2000.0, 50000.0);
        w.tint = std::clamp(w.tint, -150.0, 150.0);
        w.mul = m_color->multipliersFor(w.temperature, w.tint);
    }
    std::lock_guard lock(m_mutex);
    m_autoWb = w;
    return w;
}

WbValues DevelopEngine::effectiveWb(const DevelopSettings& s) const
{
    switch (s.wbMode) {
    case WbMode::AsShot: return asShotWb();
    case WbMode::Auto: return autoWb();
    case WbMode::Custom: return wbFromTempTint(s.temperature, s.tint);
    }
    return asShotWb();
}

Geometry DevelopEngine::geometry(const DevelopSettings& s, bool ignoreCrop) const
{
    return Geometry(m_geoSrc, s, ignoreCrop);
}

QSize DevelopEngine::outputSize(const DevelopSettings& s) const
{
    QSizeF c = geometry(s).cropSize();
    return QSize(std::max(1, int(std::round(c.width()))), std::max(1, int(std::round(c.height()))));
}

ImageF DevelopEngine::warpCached(const Geometry& g, const DevelopSettings& s, int lvl, int w, int h, const WarpOptions& o) const
{
    QString key = geometryKey(s, false) + QStringLiteral("|%1|%2|%3|%4,%5,%6,%7|%8|%9,%10")
                                              .arg(lvl).arg(w).arg(h).arg(o.roi.x()).arg(o.roi.y()).arg(o.roi.width()).arg(o.roi.height())
                                              .arg(o.bicubic).arg(g.crop().width()).arg(g.crop().height());
    {
        std::lock_guard lock(m_mutex);
        if (m_mainCache.key == key && m_mainCache.img)
            return *m_mainCache.img;
    }
    auto img = std::make_shared<ImageF>(warp(*level(lvl), g, w, h, o));
    // Only cache preview-sized images; export renders are one-off.
    if (size_t(w) * h <= 12'000'000) {
        std::lock_guard lock(m_mutex);
        m_mainCache = {key, img};
    }
    return *img;
}

void DevelopEngine::colorStage(ImageF& img, const DevelopSettings& s, const WbValues& wb) const
{
    const Vec3& applied = m_raw->color.appliedMul;
    float ratio[3];
    for (int c = 0; c < 3; ++c)
        ratio[c] = float(wb.mul[c] / applied[c]);
    // Normalize so that the lowest-ratio channel keeps its scale.
    const float minRatio = std::min({ratio[0], ratio[1], ratio[2]});
    for (float& r : ratio)
        r /= minRatio;
    // Highlight reconstruction. Channel c clips at applied[c] in the decoded data; where some
    // channels are (nearly) clipped and others not (often green in white clouds), the clipped
    // ones get at least the mean of the others, so the unclipped channels' detail stays, and
    // the colour fades to neutral; where all are clipped, white at the highest clip level. Blended in from just below the clip
    // level: demosaicing leaves values a little under it next to clipped ones (a hard
    // threshold leaves magenta fringes there).
    float clipLo[3], clipHi[3], clipWb[3];
    for (int c = 0; c < 3; ++c) {
        clipLo[c] = float(applied[c]) * 0.90f;
        clipHi[c] = float(applied[c]) * 0.995f;
        clipWb[c] = float(applied[c]) * ratio[c];
    }
    const float maxClip = std::max({clipWb[0], clipWb[1], clipWb[2]});
    auto balanced = [&](const float* p, float& r, float& g, float& b) {
        float v[3] = {p[0] * ratio[0], p[1] * ratio[1], p[2] * ratio[2]};
        if (p[0] > clipLo[0] || p[1] > clipLo[1] || p[2] > clipLo[2]) {
            float w[3], ws = 0, es = 0;
            for (int c = 0; c < 3; ++c) {
                w[c] = smoothstep(clipLo[c], clipHi[c], p[c]);
                ws += 1 - w[c];
                es += (1 - w[c]) * v[c];
            }
            const float all = std::min({w[0], w[1], w[2]});
            const float est = ws > 1e-4f ? es / ws : maxClip;
            float most = 0, mean = 0;
            for (int c = 0; c < 3; ++c) {
                float t = v[c] + w[c] * (std::max(est, v[c]) - v[c]);
                v[c] = std::min(t + all * (maxClip - t), std::max(maxClip, v[c]));
                most = std::max(most, w[c]);
                mean += v[c] / 3;
            }
            // The estimate keeps the brightness, not the colour: towards neutral as channels clip.
            for (int c = 0; c < 3; ++c)
                v[c] += most * (mean - v[c]);
        }
        r = v[0];
        g = v[1];
        b = v[2];
    };
    double ev = s.exposure + m_raw->color.baselineExposure + defaultBaselineExposure(*m_raw);
    if (const ResolvedProfile prof = profile(s); prof.valid()) {
        // DNG profile: forward matrix to ProPhoto, hue/saturation map, exposure, look tables.
        const ProfileTransform pt(prof, m_raw->color, wb.mul);
        const float gain = float(std::pow(2.0, ev + pt.baselineExposureOffset()));
        float A[9], B[9];
        for (int i = 0; i < 9; ++i) {
            A[i] = float(pt.camToProPhoto()[i / 3][i % 3]);
            B[i] = float(color::proPhotoToRec2020()[i / 3][i % 3]);
        }
#pragma omp parallel for schedule(static)
        for (int y = 0; y < img.height; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 3) {
                float r, g, b;
                balanced(p, r, g, b);
                float q[3] = {A[0] * r + A[1] * g + A[2] * b, A[3] * r + A[4] * g + A[5] * b, A[6] * r + A[7] * g + A[8] * b};
                pt.applyHueSatMap(q);
                q[0] *= gain;
                q[1] *= gain;
                q[2] *= gain;
                pt.applyLook(q);
                p[0] = B[0] * q[0] + B[1] * q[1] + B[2] * q[2];
                p[1] = B[3] * q[0] + B[4] * q[1] + B[5] * q[2];
                p[2] = B[6] * q[0] + B[7] * q[1] + B[8] * q[2];
            }
        }
        return;
    }
    Mat3 m = m_color->camToRec2020(wb.mul);
    float gain = float(std::pow(2.0, ev));
    float M[9];
    for (int i = 0; i < 9; ++i)
        M[i] = float(m[i / 3][i % 3]) * gain;
#pragma omp parallel for schedule(static)
    for (int y = 0; y < img.height; ++y) {
        float* p = img.row(y);
        for (int x = 0; x < img.width; ++x, p += 3) {
            float r, g, b;
            balanced(p, r, g, b);
            p[0] = M[0] * r + M[1] * g + M[2] * b;
            p[1] = M[3] * r + M[4] * g + M[5] * b;
            p[2] = M[6] * r + M[7] * g + M[8] * b;
        }
    }
}

ImageF DevelopEngine::contextImage(const DevelopSettings& s, const WbValues& wb, bool ignoreCrop) const
{
    ImageF ctx = *contextCamera(s, ignoreCrop);
    colorStage(ctx, s, wb);
    return ctx;
}

std::shared_ptr<const ImageF> DevelopEngine::contextCamera(const DevelopSettings& s, bool ignoreCrop) const
{
    Geometry g = geometry(s, ignoreCrop);
    QSizeF cs = g.cropSize();
    double k = kContextLongEdge / std::max(cs.width(), cs.height());
    int w = std::max(8, int(std::round(cs.width() * k))), h = std::max(8, int(std::round(cs.height() * k)));
    QString key = geometryKey(s, ignoreCrop);
    std::shared_ptr<ImageF> cam;
    {
        std::lock_guard lock(m_mutex);
        if (m_ctxCache.key == key)
            cam = m_ctxCache.img;
    }
    if (!cam) {
        int lvl = levelFor(1.0 / k);
        cam = std::make_shared<ImageF>(warp(*level(lvl), g, w, h, {}));
        std::lock_guard lock(m_mutex);
        m_ctxCache = {key, cam};
    }
    return cam;
}

namespace {

// ---- Dehaze ----------------------------------------------------------------------
// Haze model (Koschmieder): I = J*t + A*(1 - t), with airlight colour A and transmission
// t. The dark channel prior (He et al.) estimates haze from the minimum channel over a
// neighbourhood: haze-free content almost always has some dark channel locally. Like
// common dehaze implementations the model works on gamma-encoded values (here relative
// to the airlight, so A = 1), which makes it act on mid-tones too. The transmission
// map comes from the low-res context and is refined edge-aware with a guided filter on
// luminance, so it can be evaluated at any output resolution.

constexpr float kHazeGamma = 2.2f;

struct Dehaze {
    float amount = 0;   // -1..1
    float A[3] = {1, 1, 1};
    float lift = 1;     // mid-tone lift (power on encoded values) compensating some darkening
    PlaneF tA, tB;      // guided filter coefficients: t = tA * log2(Y) + tB
    bool valid() const { return amount != 0 && !tA.empty(); }
};

void applyDehaze(ImageF& img, const Dehaze& d, const QRectF& roi);

Dehaze computeDehaze(const ImageF& ctx, double amount)
{
    Dehaze d;
    // Stronger response at low slider values.
    d.amount = float(std::copysign(std::pow(std::abs(amount), 0.7), amount));
    const int w = ctx.width, h = ctx.height, n = w * h;
    const int longEdge = std::max(w, h);
    const int patch = std::max(1, int(longEdge * 0.012));
    const Vec3& lw = color::kRec2020Luma;
    auto lum = [&](int i) {
        const float* p = ctx.data.data() + size_t(i) * 3;
        return float(lw[0] * p[0] + lw[1] * p[1] + lw[2] * p[2]);
    };
    // Raw dark channel to locate the haziest region.
    PlaneF minc(w, h);
    for (int i = 0; i < n; ++i) {
        const float* p = ctx.data.data() + size_t(i) * 3;
        minc.data[i] = std::max(0.0f, std::min({p[0], p[1], p[2]}));
    }
    PlaneF dark = minFilter(minc, patch);
    // Airlight: mean colour of the brightest pixels among the top 0.2% dark channel.
    std::vector<int> idx(n);
    for (int i = 0; i < n; ++i)
        idx[i] = i;
    const int top = std::max(1, n / 500);
    std::nth_element(idx.begin(), idx.begin() + top, idx.end(), [&](int a, int b) { return dark.data[a] > dark.data[b]; });
    std::sort(idx.begin(), idx.begin() + top, [&](int a, int b) { return lum(a) > lum(b); });
    const int pick = std::max(1, top / 4);
    double A[3] = {0, 0, 0};
    for (int k = 0; k < pick; ++k)
        for (int c = 0; c < 3; ++c)
            A[c] += ctx.data[size_t(idx[k]) * 3 + c];
    double Ay = 0;
    for (int c = 0; c < 3; ++c) {
        A[c] /= pick;
        Ay += lw[c] * A[c];
    }
    if (Ay <= 1e-6)
        return {};
    // Removing haze: no brighter than about white (blown skies would leave all other
    // content looking haze-free). Adding haze: a light grey veil, not a white-out.
    Ay = std::min(Ay, amount > 0 ? 1.0 : 0.55);
    double Ay0 = 0;
    for (int c = 0; c < 3; ++c)
        Ay0 += lw[c] * A[c];
    for (int c = 0; c < 3; ++c) {
        double v = A[c] * Ay / Ay0;
        // Keep the airlight close to neutral: a strongly tinted estimate would shift colours.
        d.A[c] = float(std::max(Ay * 0.5 + v * 0.5, Ay * 0.2));
    }
    // Encoded dark channel relative to the airlight -> haze density.
    for (int i = 0; i < n; ++i) {
        const float* p = ctx.data.data() + size_t(i) * 3;
        float m = std::min({p[0] / d.A[0], p[1] / d.A[1], p[2] / d.A[2]});
        minc.data[i] = std::pow(std::clamp(m, 0.0f, 1.0f), 1.0f / kHazeGamma);
    }
    PlaneF haze = minFilter(minc, patch);
    // Transmission. Removing haze keeps a minimum transmission (noise, skies); adding
    // haze uses a fairly even veil, a bit denser where there already is haze.
    PlaneF t(w, h);
    const float a = d.amount;
    for (int i = 0; i < n; ++i) {
        float hz = haze.data[i];
        t.data[i] = a > 0 ? 1.0f - 0.85f * a * hz : 1.0f + a * (0.35f + 0.3f * hz);
    }
    PlaneF L(w, h);
    for (int i = 0; i < n; ++i)
        L.data[i] = std::log2(std::max(lum(i), 1e-6f));
    guidedFilterCoefficients(L, t, std::max(2, int(longEdge * 0.03)), 0.05f, d.tA, d.tB);
    if (a > 0) {
        // Removing haze darkens; give back part of it with a power curve on the encoded
        // values (keeps black and the airlight level, so bright skies don't blow out).
        ImageF after = ctx;
        applyDehaze(after, d, QRectF(0, 0, 1, 1));
        const float ay = float(lw[0] * d.A[0] + lw[1] * d.A[1] + lw[2] * d.A[2]);
        double before = 0, now = 0;
        int count = 0;
        for (int i = 0; i < n; ++i) {
            float Y0 = lum(i);
            if (Y0 <= 0)
                continue; // outside the image (transform borders)
            const float* q = after.data.data() + size_t(i) * 3;
            float Y1 = float(lw[0] * q[0] + lw[1] * q[1] + lw[2] * q[2]);
            before += std::log(std::clamp(Y0 / ay, 1e-4f, 1.0f));
            now += std::log(std::clamp(Y1 / ay, 1e-4f, 1.0f));
            ++count;
        }
        if (count > 0 && now < 0 && before < 0) {
            double k = before / now; // power that would restore the mean
            d.lift = float(std::clamp(1.0 + 0.35 * (k - 1.0), 0.6, 1.0));
        }
    }
    return d;
}

void applyDehaze(ImageF& img, const Dehaze& d, const QRectF& roi)
{
    if (!d.valid())
        return;
    const Vec3& lw = color::kRec2020Luma;
    const float a = d.amount;
#pragma omp parallel for schedule(static)
    for (int y = 0; y < img.height; ++y) {
        float* p = img.row(y);
        float v = float(roi.y() + (y + 0.5) / img.height * roi.height());
        for (int x = 0; x < img.width; ++x, p += 3) {
            float u = float(roi.x() + (x + 0.5) / img.width * roi.width());
            float Y = std::max(float(lw[0] * p[0] + lw[1] * p[1] + lw[2] * p[2]), 1e-6f);
            float t = sampleNormalized(d.tA, u, v) * std::log2(Y) + sampleNormalized(d.tB, u, v);
            t = std::clamp(t, a > 0 ? 0.3f : 0.0f, 1.0f);
            for (int c = 0; c < 3; ++c) {
                float e = std::pow(std::max(p[c], 0.0f) / d.A[c], 1.0f / kHazeGamma);
                float j;
                if (a > 0) {
                    j = (e - 1.0f) / t + 1.0f;
                    // Soft floor instead of clipping: keeps some shadow detail.
                    float fl = e * 0.25f;
                    j = j > fl * 2 ? j : fl + (j > 0 ? j * j / (4 * fl + 1e-12f) : 0.0f);
                } else {
                    j = e * t + (1.0f - t);
                }
                if (d.lift != 1 && j > 0 && j < 1)
                    j = std::pow(j, d.lift);
                p[c] = d.A[c] * std::pow(j, kHazeGamma);
            }
        }
    }
}

// ---- Texture ---------------------------------------------------------------------
// Medium-frequency detail (roughly 1-16 px at 24 MP): enhanced or smoothed on log
// luminance. Finer than clarity; leaves pixel-level noise alone. Scales are relative
// to the photo size so previews match exports.

void textureStage(ImageF& img, const DevelopSettings& s, double outScale, double fullLongEdge)
{
    if (s.texture == 0)
        return;
    const double unit = fullLongEdge / 6000.0 * outScale; // output px per "24 MP pixel"
    const float fine = float(1.0 * unit), coarse = float(16.0 * unit);
    if (coarse < 0.6f)
        return; // tiny thumbnail: nothing to see
    PlaneF L(img.width, img.height);
    const Vec3& lw = color::kRec2020Luma;
    for (size_t i = 0; i < L.data.size(); ++i) {
        const float* p = img.data.data() + i * 3;
        L.data[i] = std::log2(std::max(float(lw[0] * p[0] + lw[1] * p[1] + lw[2] * p[2]), 1e-6f));
    }
    PlaneF bf = gaussianBlur(L, fine), bc = gaussianBlur(L, coarse);
    const float amount = float(s.texture / 100.0) * (s.texture > 0 ? 1.7f : 1.0f);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < img.height; ++y) {
        float* p = img.row(y);
        for (int x = 0; x < img.width; ++x, p += 3) {
            size_t i = size_t(y) * img.width + x;
            float band = bf.data[i] - bc.data[i];
            band = band / (1.0f + std::abs(band) / 0.6f); // limit halos at strong edges
            // Less in deep shadows, where it would mostly lift noise.
            float wgt = smoothstep(-11.0f, -7.0f, L.data[i]);
            float k = std::exp2(amount * band * wgt);
            p[0] *= k;
            p[1] *= k;
            p[2] *= k;
        }
    }
}

struct LocalTone {
    PlaneF baseA, baseB, clarA, clarB;
    float topEv = -100; // log2 luminance of the brightest areas (99.9th percentile)
};

LocalTone computeLocalTone(const ImageF& ctx, const DevelopSettings& s)
{
    LocalTone lt;
    PlaneF L = luminanceLog(ctx);
    if (s.highlights < 0 && !L.data.empty()) {
        std::vector<float> v = L.data;
        const size_t k = std::min(v.size() - 1, size_t(v.size() * 0.999));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        lt.topEv = v[k];
    }
    int longEdge = std::max(ctx.width, ctx.height);
    if (s.highlights != 0 || s.shadows != 0)
        guidedFilterCoefficients(L, std::max(2, int(longEdge * 0.035)), 0.30f, lt.baseA, lt.baseB);
    if (s.clarity != 0)
        guidedFilterCoefficients(L, std::max(1, int(longEdge * 0.012)), 0.06f, lt.clarA, lt.clarB);
    return lt;
}

void toneStage(ImageF& img, const LocalTone& lt, const QRectF& roi, const DevelopSettings& s)
{
    const ToneCurve curve(s);
    const float logWhite = float(std::log2(curve.whitePoint()));
    const bool local = !lt.baseA.empty(), clar = !lt.clarA.empty();
    const float sh = float(s.shadows / 100.0), hl = float(s.highlights / 100.0), cl = float(s.clarity / 100.0);
    // Extra pull per EV above the upper midtones, bringing the brightest areas (topEv) from
    // above white to just below it (at -100).
    const float topX = lt.topEv - logWhite;
    const float topSlope = topX > -0.7f + 0.1f ? std::max(0.0f, (topX + 0.05f - 1.0f) / (topX + 0.7f)) : 0.0f;
    const float lw[3] = {float(color::kRec2020Luma[0]), float(color::kRec2020Luma[1]), float(color::kRec2020Luma[2])};
    // Tone position LUT over log2 luminance for weights.
    constexpr int kPos = 512;
    constexpr float kPosMin = -14, kPosMax = 6;
    std::vector<float> pos(kPos);
    for (int i = 0; i < kPos; ++i)
        pos[i] = curve.positionFromLog(kPosMin + (kPosMax - kPosMin) * i / (kPos - 1));
    auto position = [&](float ev) {
        float f = std::clamp((ev - kPosMin) / (kPosMax - kPosMin) * (kPos - 1), 0.0f, float(kPos - 1));
        int i = std::min(int(f), kPos - 2);
        float t = f - i;
        return pos[i] * (1 - t) + pos[i + 1] * t;
    };

#pragma omp parallel for schedule(static)
    for (int y = 0; y < img.height; ++y) {
        float* p = img.row(y);
        float v = float(roi.y() + (y + 0.5) / img.height * roi.height());
        for (int x = 0; x < img.width; ++x, p += 3) {
            float r = std::max(p[0], 0.0f), g = std::max(p[1], 0.0f), b = std::max(p[2], 0.0f);
            if (local || clar) {
                float u = float(roi.x() + (x + 0.5) / img.width * roi.width());
                float Y = std::max(lw[0] * r + lw[1] * g + lw[2] * b, 1e-6f);
                float L = std::log2(Y);
                float dL = 0;
                if (local) {
                    float B = sampleNormalized(lt.baseA, u, v) * L + sampleNormalized(lt.baseB, u, v);
                    float t = position(B);
                    float ws = 1.0f - smoothstep(0.0f, 0.62f, t);
                    float wh = smoothstep(0.38f, 1.0f, t);
                    dL += sh * 1.7f * ws;
                    if (hl < 0) {
                        // About an even pull from the upper midtones up, so that blown areas stay
                        // near white and clouds keep their brightness against the sky (fitted to
                        // Lightroom's Highlights -100: 1 EV on skies and clouds, none on grass),
                        // more above the white point when the brightest areas are further up, so
                        // that they come down to just below white with their detail.
                        const float x = B - logWhite;
                        float pull = smoothstep(-2.2f, -0.7f, x);
                        if (x > -0.7f)
                            pull += topSlope * (x + 0.7f);
                        dL += hl * pull;
                    } else {
                        dL += hl * wh * 0.9f;
                    }
                }
                if (clar) {
                    float Bc = sampleNormalized(lt.clarA, u, v) * L + sampleNormalized(lt.clarB, u, v);
                    float D = std::clamp(L - Bc, -1.5f, 1.5f);
                    float t = position(Bc);
                    float wm = 1.0f - std::pow(2.0f * t - 1.0f, 2.0f);
                    dL += cl * 1.1f * D * std::max(wm, 0.0f);
                }
                float k = std::exp2(dL);
                r *= k;
                g *= k;
                b *= k;
            }
            // Per-channel curve, then restore the hue of the input (mid channel ratio).
            float in[3] = {r, g, b};
            float out[3] = {curve(r), curve(g), curve(b)};
            int imax = r >= g ? (r >= b ? 0 : 2) : (g >= b ? 1 : 2);
            int imin = r < g ? (r < b ? 0 : 2) : (g < b ? 1 : 2);
            if (imax != imin) {
                int imid = 3 - imax - imin;
                float span = in[imax] - in[imin];
                float ratio = span > 1e-9f ? (in[imid] - in[imin]) / span : 0.0f;
                out[imid] = out[imin] + ratio * (out[imax] - out[imin]);
            }
            p[0] = out[0];
            p[1] = out[1];
            p[2] = out[2];
        }
    }
}

// The base look adds some chroma, like camera profiles do over a plain colour matrix.
constexpr float kBaseChroma = 1.12f;

// `baseChroma`: kBaseChroma for Silmä's own colour, 1 with a camera profile (it has its own).
void presenceStage(ImageF& img, const DevelopSettings& s, float baseChroma)
{
    const float sat = float(s.saturation / 100.0), vib = float(s.vibrance / 100.0);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < img.height; ++y) {
        float* p = img.row(y);
        for (int x = 0; x < img.width; ++x, p += 3) {
            float lab[3];
            float rgb[3] = {std::max(p[0], 0.0f), std::max(p[1], 0.0f), std::max(p[2], 0.0f)};
            rec2020ToOklab(rgb, lab);
            float C = std::hypot(lab[1], lab[2]);
            if (C < 1e-6f)
                continue;
            float k = baseChroma * (1.0f + sat);
            if (vib > 0) {
                // Boost muted colours most; protect skin tones (orange hues).
                float h = std::atan2(lab[2], lab[1]);
                float skin = 1.0f - 0.65f * std::exp(-std::pow((h - 0.95f) / 0.4f, 2.0f));
                float muted = std::pow(1.0f - std::min(C / 0.22f, 1.0f), 1.5f);
                k *= 1.0f + vib * 1.2f * muted * skin;
            } else if (vib < 0) {
                float muted = 1.0f - 0.5f * std::min(C / 0.25f, 1.0f);
                k *= std::max(0.0f, 1.0f + vib * muted);
            }
            k = std::max(k, 0.0f);
            lab[1] *= k;
            lab[2] *= k;
            oklabToRec2020(lab, p);
        }
    }
}

// Point curves in display-encoded space, then Rec.2020 -> sRGB with gamut mapping; output encoded.
// `profileCurve`: a look profile's tone curve, applied before the user's curves.
void outputStage(ImageF& img, const DevelopSettings& s, const CurvePoints& profileCurve = {})
{
    const CurveLut master(s.curveRgb.points), cr(s.curveR.points), cg(s.curveG.points), cb(s.curveB.points), pc(profileCurve.points);
    const bool anyCurve = !master.identity() || !cr.identity() || !cg.identity() || !cb.identity() || !pc.identity();
    float M[9];
    for (int i = 0; i < 9; ++i)
        M[i] = float(color::kRec2020ToSrgb[i / 3][i % 3]);
    const float lw[3] = {0.2126f, 0.7152f, 0.0722f};
#pragma omp parallel for schedule(static)
    for (int y = 0; y < img.height; ++y) {
        float* p = img.row(y);
        for (int x = 0; x < img.width; ++x, p += 3) {
            float r = M[0] * p[0] + M[1] * p[1] + M[2] * p[2];
            float g = M[3] * p[0] + M[4] * p[1] + M[5] * p[2];
            float b = M[6] * p[0] + M[7] * p[1] + M[8] * p[2];
            // Out-of-gamut negatives: desaturate towards luminance (keeps luminance).
            float Y = lw[0] * r + lw[1] * g + lw[2] * b;
            float mn = std::min({r, g, b});
            if (mn < 0 && Y > 0) {
                float t = Y / (Y - mn);
                r = Y + (r - Y) * t;
                g = Y + (g - Y) * t;
                b = Y + (b - Y) * t;
            }
            float mx = std::max({r, g, b});
            if (mx > 1.0f) {
                // Bright saturated colours: blend towards white rather than hard clipping.
                float Yc = std::min(Y, 1.0f);
                float t = (1.0f - Yc) / std::max(mx - Yc, 1e-6f);
                r = Yc + (r - Yc) * t;
                g = Yc + (g - Yc) * t;
                b = Yc + (b - Yc) * t;
            }
            float e[3] = {srgbEncode(std::clamp(r, 0.0f, 1.0f)), srgbEncode(std::clamp(g, 0.0f, 1.0f)), srgbEncode(std::clamp(b, 0.0f, 1.0f))};
            if (anyCurve) {
                e[0] = cr(master(pc(e[0])));
                e[1] = cg(master(pc(e[1])));
                e[2] = cb(master(pc(e[2])));
            }
            p[0] = e[0];
            p[1] = e[1];
            p[2] = e[2];
        }
    }
}

// Unsharp mask on encoded luminance; radius in output pixels.
void sharpenStage(ImageF& img, const DevelopSettings& s, double outScale)
{
    if (s.sharpenAmount <= 0)
        return;
    float sigma = float(s.sharpenRadius * outScale);
    if (sigma < 0.25f)
        return; // invisible at this zoom level
    PlaneF L(img.width, img.height);
    for (size_t i = 0; i < L.data.size(); ++i) {
        const float* p = img.data.data() + i * 3;
        L.data[i] = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
    }
    PlaneF blur = gaussianBlur(L, sigma);
    PlaneF mask;
    if (s.sharpenMasking > 0) {
        // Edge mask: gradient magnitude of a slightly smoothed luminance.
        PlaneF sm = gaussianBlur(L, std::max(1.0f, sigma));
        mask = PlaneF(img.width, img.height, 1.0f);
        float thr = float(s.sharpenMasking / 100.0) * 0.08f;
        for (int y = 1; y < img.height - 1; ++y)
            for (int x = 1; x < img.width - 1; ++x) {
                float gx = sm.at(x + 1, y) - sm.at(x - 1, y), gy = sm.at(x, y + 1) - sm.at(x, y - 1);
                float g = std::hypot(gx, gy);
                mask.at(x, y) = smoothstep(thr * 0.5f, thr * 1.5f, g);
            }
    }
    const float amount = float(s.sharpenAmount / 100.0) * 1.6f;
    // Detail: low values suppress large (halo-prone) differences.
    const float limit = 0.02f + float(s.sharpenDetail / 100.0) * 0.25f;
#pragma omp parallel for schedule(static)
    for (int y = 0; y < img.height; ++y) {
        float* p = img.row(y);
        for (int x = 0; x < img.width; ++x, p += 3) {
            size_t i = size_t(y) * img.width + x;
            float d = L.data[i] - blur.data[i];
            d = d / (1.0f + std::abs(d) / limit);
            float k = amount * d * (mask.empty() ? 1.0f : mask.data[i]);
            p[0] = std::clamp(p[0] + k, 0.0f, 1.0f);
            p[1] = std::clamp(p[1] + k, 0.0f, 1.0f);
            p[2] = std::clamp(p[2] + k, 0.0f, 1.0f);
        }
    }
}

} // namespace

void undoDefaultLook(ImageF& img)
{
    const DevelopSettings defaults;
    const ToneCurve curve(defaults);
    // Inverse curve: tabulated over display values (monotonic, so by bisection).
    constexpr int kInv = 8192;
    std::vector<float> inv(kInv);
    for (int i = 0; i < kInv; ++i) {
        const float target = float(i) / (kInv - 1);
        float lo = std::exp2(ToneCurve::kMinEv), hi = std::exp2(ToneCurve::kMaxEv);
        if (curve(lo) >= target) {
            inv[i] = 0;
            continue;
        }
        for (int k = 0; k < 60; ++k) {
            const float mid = std::sqrt(lo * hi);
            (curve(mid) < target ? lo : hi) = mid;
        }
        inv[i] = std::sqrt(lo * hi);
    }
    auto invCurve = [&](float v) {
        const float f = std::clamp(v, 0.0f, 1.0f) * (kInv - 1);
        const int i = std::min(int(f), kInv - 2);
        const float t = f - i;
        return inv[i] * (1 - t) + inv[i + 1] * t;
    };
    const float gain = float(std::pow(2.0, -defaultBaselineExposure(RawImage{})));
#pragma omp parallel for schedule(static)
    for (int y = 0; y < img.height; ++y) {
        float* p = img.row(y);
        for (int x = 0; x < img.width; ++x, p += 3) {
            // Base chroma (presenceStage) first, as it comes last in rendering.
            float rgb[3] = {std::max(p[0], 0.0f), std::max(p[1], 0.0f), std::max(p[2], 0.0f)}, lab[3];
            rec2020ToOklab(rgb, lab);
            lab[1] /= kBaseChroma;
            lab[2] /= kBaseChroma;
            oklabToRec2020(lab, rgb);
            // Tone curve (toneStage): per channel, with the mid channel's position between the
            // others kept, exactly as rendering does it.
            float in[3] = {std::max(rgb[0], 0.0f), std::max(rgb[1], 0.0f), std::max(rgb[2], 0.0f)};
            float out[3] = {invCurve(in[0]), invCurve(in[1]), invCurve(in[2])};
            const int imax = in[0] >= in[1] ? (in[0] >= in[2] ? 0 : 2) : (in[1] >= in[2] ? 1 : 2);
            const int imin = in[0] < in[1] ? (in[0] < in[2] ? 0 : 2) : (in[1] < in[2] ? 1 : 2);
            if (imax != imin) {
                const int imid = 3 - imax - imin;
                const float span = in[imax] - in[imin];
                const float ratio = span > 1e-9f ? (in[imid] - in[imin]) / span : 0.0f;
                out[imid] = out[imin] + ratio * (out[imax] - out[imin]);
            }
            for (int c = 0; c < 3; ++c)
                p[c] = out[c] * gain;
        }
    }
}

ImageF DevelopEngine::renderEncoded(const DevelopSettings& s, const RenderRequest& req, WbValues* wbOut) const
{
    applyHeals(s);
    Geometry g = geometry(s, req.ignoreCrop);
    QSizeF cs = g.cropSize();
    int w = req.width, h = req.height;
    if (w <= 0 || h <= 0) {
        w = std::max(1, int(std::round(cs.width() * req.roi.width())));
        h = std::max(1, int(std::round(cs.height() * req.roi.height())));
    }
    // Full-res source pixels per output pixel.
    double pxPer = cs.width() * req.roi.width() / w;
    int lvl = levelFor(pxPer);
    WarpOptions wo;
    wo.roi = req.roi;
    wo.bicubic = req.highQuality;
    ImageF img;
    if (req.ignoreCrop)
        img = warp(*level(lvl), g, w, h, wo);
    else
        img = warpCached(g, s, lvl, w, h, wo);

    WbValues wb = effectiveWb(s);
    if (wbOut)
        *wbOut = wb;
    colorStage(img, s, wb);
    denoise(img, s, 1.0 / pxPer);
    const bool localTone = s.highlights != 0 || s.shadows != 0 || s.clarity != 0;
    ImageF ctx;
    if (localTone || s.dehaze != 0)
        ctx = contextImage(s, wb, req.ignoreCrop);
    if (s.dehaze != 0) {
        Dehaze dh = computeDehaze(ctx, s.dehaze / 100.0);
        applyDehaze(img, dh, req.roi);
        applyDehaze(ctx, dh, QRectF(0, 0, 1, 1));
    }
    textureStage(img, s, 1.0 / pxPer, std::max(cs.width(), cs.height()));
    LocalTone lt;
    if (localTone)
        lt = computeLocalTone(ctx, s);
    toneStage(img, lt, req.roi, s);
    const ResolvedProfile prof = profile(s);
    presenceStage(img, s, prof.valid() ? 1.0f : kBaseChroma);
    outputStage(img, s, prof.look ? prof.look->toneCurve : CurvePoints{});
    sharpenStage(img, s, 1.0 / pxPer);
    if (req.outputSharpening > 0 && pxPer > 1)
        outputSharpen(img, req.outputSharpening * float(std::min(pxPer - 1.0, 1.0)));
    return img;
}

ImageF DevelopEngine::warpedCamera(const DevelopSettings& s, int w, int h, bool hq, bool ignoreCrop) const
{
    Geometry g = geometry(s, ignoreCrop);
    double pxPer = g.cropSize().width() / w;
    WarpOptions wo;
    wo.bicubic = hq;
    applyHeals(s);
    return warp(*level(levelFor(pxPer)), g, w, h, wo);
}

RenderResult DevelopEngine::render(const DevelopSettings& s, const RenderRequest& req) const
{
    RenderResult r;
    ImageF enc = renderEncoded(s, req, &r.wb);
    r.image = toQImage(enc, &r.histogram);
    return r;
}

QImage toQImage(const ImageF& enc, Histogram* hist)
{
    QImage img(enc.width, enc.height, QImage::Format_RGB888);
    if (hist)
        for (auto& c : *hist)
            c.fill(0);
    for (int y = 0; y < enc.height; ++y) {
        uchar* line = img.scanLine(y);
        const float* p = enc.row(y);
        for (int x = 0; x < enc.width * 3; ++x) {
            // Small ordered dither avoids banding in smooth skies.
            float d = ((x / 3 + y * 3) % 4 - 1.5f) * 0.25f;
            int v = std::clamp(int(p[x] * 255.0f + 0.5f + d * 0.5f), 0, 255);
            line[x] = uchar(v);
            if (hist)
                ++(*hist)[x % 3][v];
        }
    }
    return img;
}

ResolvedProfile DevelopEngine::profile(const DevelopSettings& s) const
{
    if (s.profile.isEmpty() || m_raw->rendered)
        return {};
    return resolveProfile(s.profile, m_raw->make, m_raw->model);
}

QStringList DevelopEngine::availableProfiles() const
{
    return m_raw->rendered ? QStringList() : profilesForCamera(m_raw->make, m_raw->model);
}

QList<ProfileChoice> DevelopEngine::profileChoices() const
{
    return m_raw->rendered ? QList<ProfileChoice>() : silma::profileChoices(m_raw->make, m_raw->model);
}

DevelopSettings DevelopEngine::autoTone(const DevelopSettings& in) const
{
    DevelopSettings s = in;
    applyHeals(s);
    s.exposure = s.contrast = s.highlights = s.shadows = s.whites = s.blacks = 0;
    WbValues wb = effectiveWb(s);
    ImageF ctx = contextImage(s, wb, false);
    if (s.dehaze != 0)
        applyDehaze(ctx, computeDehaze(ctx, s.dehaze / 100.0), QRectF(0, 0, 1, 1));
    PlaneF L = luminanceLog(ctx);
    const auto cam = contextCamera(s, false);
    std::vector<float> v;
    v.reserve(L.data.size());
    size_t blown = 0;
    const Vec3& clip = m_raw->color.appliedMul;
    for (size_t i = 0; i < L.data.size(); ++i) {
        const float* p = ctx.data.data() + i * 3;
        if (p[0] == 0 && p[1] == 0 && p[2] == 0)
            continue; // outside the image (transform borders)
        v.push_back(L.data[i]);
        const float* c = cam->data.data() + i * 3;
        // Clipped in the raw: a channel at its clip plateau. Merged HDRs are scaled so that
        // only their brightest value reaches it, so large areas there are rare.
        if (c[0] >= 0.98f * clip[0] || c[1] >= 0.98f * clip[1] || c[2] >= 0.98f * clip[2])
            ++blown;
    }
    if (v.size() < 64)
        return in;
    std::sort(v.begin(), v.end());
    auto pct = [&](double q) { return v[std::min(v.size() - 1, size_t(q * (v.size() - 1)))]; };
    const float logGrey = std::log2(kGrey);
    // Exposure: bring a blend of the median and mean log luminance to middle grey.
    double mean = 0;
    for (float x : v)
        mean += x;
    mean /= v.size();
    double key = 0.6 * pct(0.5) + 0.4 * mean;
    double exposure = std::clamp(logGrey - key - 0.1, -4.0, 4.0);
    // Damp corrections: shadows/highlights below also brighten, and most scenes are not grey.
    exposure *= exposure > 0 ? 0.7 : 0.85;
    s.exposure = std::round(exposure * 20) / 20;

    double hi = pct(0.995) + s.exposure - logGrey; // EV above grey after exposure
    double lo = pct(0.01) + s.exposure - logGrey;
    double spread = pct(0.95) - pct(0.05);
    // Highlights: pull down when the bright end goes past the white point.
    double highlights = std::clamp(-(hi - 2.0) * 30.0, -70.0, 0.0);
    // Blown areas: somewhat less of a pull (a white overcast sky should stay white; the slider
    // keeps it near white), though where only some channels are clipped there is detail to
    // bring back: 0.5x with 10% blown.
    highlights /= 1.0 + double(blown) / v.size() / 0.10;
    s.highlights = std::round(highlights);
    // Shadows: lift when a lot of the image sits deep in the shadows, or the darkest tenth
    // is well below grey (a shaded foreground).
    s.shadows = std::round(std::max(std::clamp(-(lo + 4.0) * 18.0, 0.0, 70.0), std::clamp((-(pct(0.10) + s.exposure - logGrey) - 1.5) * 25.0, 0.0, 50.0)));
    // Whites/blacks: set the end points.
    // (Highlights -100 brings the bright end down by about 1 EV.)
    double hiAfter = hi + s.highlights / 100.0;
    s.whites = std::round(std::clamp((2.3 - hiAfter) * 25.0, -40.0, 40.0));
    s.blacks = std::round(std::clamp((-5.5 - lo) * 12.0, -40.0, 30.0));
    // Contrast: flat scenes get more, contrasty ones less; most look flat without some.
    s.contrast = std::round(std::clamp((6.0 - spread) * 6.0 + 18.0, -5.0, 45.0));
    return s;
}

} // namespace silma
