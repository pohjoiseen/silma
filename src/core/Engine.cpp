#include "Engine.h"

#include "Denoise.h"
#include "Filters.h"
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
    return QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11,%12")
        .arg(s.lensDistortion).arg(s.lensVignetting).arg(s.perspVertical).arg(s.perspHorizontal).arg(s.rotate).arg(s.scale)
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
    return raw.isDng ? 0.75 : 0.8;
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
    m_geoSrc.orientation = m_raw->orientation;
    m_geoSrc.focalLength35 = focal35;
    m_geoSrc.lens = m_lens;
    m_pyramid.resize(8);
}

const ImageF& DevelopEngine::level(int lvl) const
{
    std::lock_guard lock(m_mutex);
    int idx = std::max(0, lvl - m_levelOffset);
    if (idx == 0)
        return m_raw->image;
    if (!m_pyramid[idx]) {
        // Build missing levels from the previous one (box 2x).
        for (int i = 1; i <= idx; ++i)
            if (!m_pyramid[i])
                m_pyramid[i] = std::make_shared<ImageF>(downscale(i == 1 ? m_raw->image : *m_pyramid[i - 1], 2));
    }
    return *m_pyramid[idx];
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
    const ImageF& img = level(lvl);
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
    auto img = std::make_shared<ImageF>(warp(level(lvl), g, w, h, o));
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
    // Data clipped at 1.0 per channel becomes ratio[c]; clip all channels at the lowest so
    // blown highlights stay neutral instead of turning magenta/cyan.
    const float clip = std::min({ratio[0], ratio[1], ratio[2]});
    // Normalize so that the lowest-ratio channel keeps its scale (white stays at 1.0).
    for (float& r : ratio)
        r /= clip;
    Mat3 m = m_color->camToRec2020(wb.mul);
    double ev = s.exposure + m_raw->color.baselineExposure + defaultBaselineExposure(*m_raw);
    float gain = float(std::pow(2.0, ev));
    float M[9];
    for (int i = 0; i < 9; ++i)
        M[i] = float(m[i / 3][i % 3]) * gain;
#pragma omp parallel for schedule(static)
    for (int y = 0; y < img.height; ++y) {
        float* p = img.row(y);
        for (int x = 0; x < img.width; ++x, p += 3) {
            float r = std::min(p[0] * ratio[0], 1.0f), g = std::min(p[1] * ratio[1], 1.0f), b = std::min(p[2] * ratio[2], 1.0f);
            p[0] = M[0] * r + M[1] * g + M[2] * b;
            p[1] = M[3] * r + M[4] * g + M[5] * b;
            p[2] = M[6] * r + M[7] * g + M[8] * b;
        }
    }
}

ImageF DevelopEngine::contextImage(const DevelopSettings& s, const WbValues& wb, bool ignoreCrop) const
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
        cam = std::make_shared<ImageF>(warp(level(lvl), g, w, h, {}));
        std::lock_guard lock(m_mutex);
        m_ctxCache = {key, cam};
    }
    ImageF ctx = *cam;
    colorStage(ctx, s, wb);
    return ctx;
}

namespace {

struct LocalTone {
    PlaneF baseA, baseB, clarA, clarB;
};

LocalTone computeLocalTone(const ImageF& ctx, const DevelopSettings& s)
{
    LocalTone lt;
    PlaneF L = luminanceLog(ctx);
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
                    if (hl < 0)
                        dL += hl * wh * (1.3f + 0.9f * std::max(0.0f, B - logWhite + 0.5f));
                    else
                        dL += hl * wh * 0.9f;
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

void presenceStage(ImageF& img, const DevelopSettings& s)
{
    // The base look adds some chroma, like camera profiles do over a plain colour matrix.
    constexpr float kBaseChroma = 1.12f;
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
            float k = kBaseChroma * (1.0f + sat);
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
void outputStage(ImageF& img, const DevelopSettings& s)
{
    const CurveLut master(s.curveRgb.points), cr(s.curveR.points), cg(s.curveG.points), cb(s.curveB.points);
    const bool anyCurve = !master.identity() || !cr.identity() || !cg.identity() || !cb.identity();
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
                e[0] = cr(master(e[0]));
                e[1] = cg(master(e[1]));
                e[2] = cb(master(e[2]));
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

ImageF DevelopEngine::renderEncoded(const DevelopSettings& s, const RenderRequest& req, WbValues* wbOut) const
{
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
        img = warp(level(lvl), g, w, h, wo);
    else
        img = warpCached(g, s, lvl, w, h, wo);

    WbValues wb = effectiveWb(s);
    if (wbOut)
        *wbOut = wb;
    colorStage(img, s, wb);
    denoise(img, s, 1.0 / pxPer);
    LocalTone lt;
    if (s.highlights != 0 || s.shadows != 0 || s.clarity != 0)
        lt = computeLocalTone(contextImage(s, wb, req.ignoreCrop), s);
    toneStage(img, lt, req.roi, s);
    presenceStage(img, s);
    outputStage(img, s);
    sharpenStage(img, s, 1.0 / pxPer);
    return img;
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

DevelopSettings DevelopEngine::autoTone(const DevelopSettings& in) const
{
    DevelopSettings s = in;
    s.exposure = s.contrast = s.highlights = s.shadows = s.whites = s.blacks = 0;
    WbValues wb = effectiveWb(s);
    ImageF ctx = contextImage(s, wb, false);
    PlaneF L = luminanceLog(ctx);
    std::vector<float> v;
    v.reserve(L.data.size());
    for (size_t i = 0; i < L.data.size(); ++i) {
        const float* p = ctx.data.data() + i * 3;
        if (p[0] == 0 && p[1] == 0 && p[2] == 0)
            continue; // outside the image (transform borders)
        v.push_back(L.data[i]);
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
    s.highlights = std::round(std::clamp(-(hi - 2.0) * 40.0, -100.0, 0.0));
    // Shadows: lift when a lot of the image sits deep in the shadows.
    s.shadows = std::round(std::clamp(-(lo + 4.0) * 18.0, 0.0, 70.0));
    // Whites/blacks: set the end points.
    double hiAfter = hi + s.highlights / 100.0 * 1.6;
    s.whites = std::round(std::clamp((2.3 - hiAfter) * 25.0, -40.0, 40.0));
    s.blacks = std::round(std::clamp((-5.5 - lo) * 12.0, -40.0, 30.0));
    // Contrast: flat scenes get some, contrasty ones less.
    s.contrast = std::round(std::clamp((6.0 - spread) * 6.0, -20.0, 30.0));
    return s;
}

} // namespace silma
