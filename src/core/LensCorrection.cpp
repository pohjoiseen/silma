#include "LensCorrection.h"

#include <QDataStream>
#include <QIODevice>
#include <algorithm>
#include <cmath>
#include <lensfun/lensfun.h>
#include <mutex>

namespace silma {

namespace {

constexpr int kGridLong = 128;

lfDatabase* lensDatabase()
{
    static lfDatabase* db = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        db = new lfDatabase();
        db->Load();
    });
    return db;
}

// ---- DNG opcodes -----------------------------------------------------------

struct WarpRect {
    int planes = 0;
    double k[3][6]{}; // kr0..kr3, kt0, kt1 per plane
    double cx = 0.5, cy = 0.5;
};
struct VignetteRadial {
    double k[5]{};
    double cx = 0.5, cy = 0.5;
};
struct GainMap {
    quint32 top = 0, left = 0, bottom = 0, right = 0, plane = 0, planes = 1;
    quint32 pointsV = 0, pointsH = 0, mapPlanes = 1;
    double spacingV = 0, spacingH = 0, originV = 0, originH = 0;
    std::vector<float> gains;

    float sample(double relX, double relY, int plane) const
    {
        if (pointsV == 0 || pointsH == 0)
            return 1.0f;
        double fy = spacingV > 0 ? (relY - originV) / spacingV : 0;
        double fx = spacingH > 0 ? (relX - originH) / spacingH : 0;
        fy = std::clamp(fy, 0.0, double(pointsV - 1));
        fx = std::clamp(fx, 0.0, double(pointsH - 1));
        int y0 = int(fy), x0 = int(fx);
        int y1 = std::min<int>(y0 + 1, pointsV - 1), x1 = std::min<int>(x0 + 1, pointsH - 1);
        double ty = fy - y0, tx = fx - x0;
        int mp = std::min<int>(plane, mapPlanes - 1);
        auto g = [&](int y, int x) { return gains[(size_t(y) * pointsH + x) * mapPlanes + mp]; };
        return float((g(y0, x0) * (1 - tx) + g(y0, x1) * tx) * (1 - ty) + (g(y1, x0) * (1 - tx) + g(y1, x1) * tx) * ty);
    }
};

struct DngLens {
    std::vector<WarpRect> warps;
    std::vector<VignetteRadial> vignettes;
    std::vector<GainMap> gainMaps;
    bool any() const { return !warps.empty() || !vignettes.empty() || !gainMaps.empty(); }
};

void parseOpcodeList(const QByteArray& blob, DngLens& out)
{
    if (blob.size() < 4)
        return;
    QDataStream s(blob);
    s.setByteOrder(QDataStream::BigEndian);
    s.setFloatingPointPrecision(QDataStream::DoublePrecision);
    quint32 count;
    s >> count;
    for (quint32 i = 0; i < count && s.status() == QDataStream::Ok; ++i) {
        quint32 id, ver, flags, bytes;
        s >> id >> ver >> flags >> bytes;
        qint64 start = s.device()->pos();
        switch (id) {
        case 1: { // WarpRectilinear
            WarpRect w;
            quint32 n;
            s >> n;
            w.planes = int(std::min<quint32>(n, 3));
            for (quint32 p = 0; p < n; ++p)
                for (int k = 0; k < 6; ++k) {
                    double v;
                    s >> v;
                    if (p < 3)
                        w.k[p][k] = v;
                }
            s >> w.cx >> w.cy;
            out.warps.push_back(w);
            break;
        }
        case 3: { // FixVignetteRadial
            VignetteRadial v;
            for (double& k : v.k)
                s >> k;
            s >> v.cx >> v.cy;
            out.vignettes.push_back(v);
            break;
        }
        case 9: { // GainMap
            GainMap g;
            s >> g.top >> g.left >> g.bottom >> g.right >> g.plane >> g.planes;
            quint32 rowPitch, colPitch;
            s >> rowPitch >> colPitch >> g.pointsV >> g.pointsH;
            s >> g.spacingV >> g.spacingH >> g.originV >> g.originH;
            s >> g.mapPlanes;
            if (g.mapPlanes == 0 || qint64(g.pointsV) * g.pointsH * g.mapPlanes > 4'000'000)
                break;
            g.gains.resize(size_t(g.pointsV) * g.pointsH * g.mapPlanes);
            s.setFloatingPointPrecision(QDataStream::SinglePrecision);
            for (float& f : g.gains)
                s >> f;
            s.setFloatingPointPrecision(QDataStream::DoublePrecision);
            out.gainMaps.push_back(std::move(g));
            break;
        }
        default:
            break; // unsupported opcodes are skipped (flags may mark them optional)
        }
        s.device()->seek(start + bytes);
    }
}

} // namespace

std::shared_ptr<const LensCorrection> LensCorrection::build(const RawImage& raw, const LensQuery& q)
{
    std::shared_ptr<LensCorrection> lc(new LensCorrection());
    const int W = raw.fullWidth > 0 ? raw.fullWidth : raw.image.width;
    const int H = raw.fullHeight > 0 ? raw.fullHeight : raw.image.height;
    const double aspect = double(W) / H;
    lc->m_gw = aspect >= 1 ? kGridLong + 1 : int(kGridLong * aspect) + 2;
    lc->m_gh = aspect >= 1 ? int(kGridLong / aspect) + 2 : kGridLong + 1;
    const int gw = lc->m_gw, gh = lc->m_gh;
    lc->m_src.resize(size_t(gw) * gh * 6);
    lc->m_gain.assign(size_t(gw) * gh * 3, 1.0f);
    auto nodeU = [&](int i) { return float(i) / (gw - 1); };
    auto nodeV = [&](int j) { return float(j) / (gh - 1); };
    for (int j = 0; j < gh; ++j)
        for (int i = 0; i < gw; ++i)
            for (int c = 0; c < 3; ++c) {
                lc->m_src[(size_t(j) * gw + i) * 6 + c * 2] = nodeU(i);
                lc->m_src[(size_t(j) * gw + i) * 6 + c * 2 + 1] = nodeV(j);
            }

    // DNG opcode based corrections take priority: they are made for this exact camera.
    DngLens dng;
    parseOpcodeList(raw.opcodes.list3, dng);
    if (dng.any()) {
        auto maxDist = [&](double cxp, double cyp) {
            double d = 0;
            for (double x : {0.0, double(W)})
                for (double y : {0.0, double(H)})
                    d = std::max(d, std::hypot(x - cxp, y - cyp));
            return d;
        };
#pragma omp parallel for schedule(static)
        for (int j = 0; j < gh; ++j) {
            for (int i = 0; i < gw; ++i) {
                float* src = &lc->m_src[(size_t(j) * gw + i) * 6];
                float* gain = &lc->m_gain[(size_t(j) * gw + i) * 3];
                double px = nodeU(i) * W, py = nodeV(j) * H;
                // Gain maps come after the warp in the list: evaluate at the destination.
                for (const auto& g : dng.gainMaps)
                    for (int c = 0; c < 3; ++c)
                        if (c >= int(g.plane) && c < int(g.plane + g.planes))
                            gain[c] *= g.sample(nodeU(i), nodeV(j), c - g.plane);
                for (int c = 0; c < 3; ++c) {
                    double sx = px, sy = py;
                    for (const auto& w : dng.warps) {
                        int p = w.planes == 1 ? 0 : std::min(c, w.planes - 1);
                        const double* k = w.k[p];
                        double cxp = w.cx * W, cyp = w.cy * H, m = maxDist(cxp, cyp);
                        double dx = (sx - cxp) / m, dy = (sy - cyp) / m;
                        double r2 = dx * dx + dy * dy;
                        double f = k[0] + r2 * (k[1] + r2 * (k[2] + r2 * k[3]));
                        double dxt = k[4] * 2 * dx * dy + k[5] * (r2 + 2 * dx * dx);
                        double dyt = k[5] * 2 * dx * dy + k[4] * (r2 + 2 * dy * dy);
                        sx = cxp + m * (f * dx + dxt);
                        sy = cyp + m * (f * dy + dyt);
                    }
                    src[c * 2] = float(sx / W);
                    src[c * 2 + 1] = float(sy / H);
                    // Vignetting precedes the warp: evaluate at the source position.
                    for (const auto& v : dng.vignettes) {
                        double cxp = v.cx * W, cyp = v.cy * H, m = maxDist(cxp, cyp);
                        double r2 = (std::pow(sx - cxp, 2) + std::pow(sy - cyp, 2)) / (m * m);
                        double g = 1 + r2 * (v.k[0] + r2 * (v.k[1] + r2 * (v.k[2] + r2 * (v.k[3] + r2 * v.k[4]))));
                        gain[c] *= float(g);
                    }
                }
            }
        }
        lc->m_identity = false;
        lc->m_description = QStringLiteral("Embedded DNG lens profile");
        return lc;
    }

    // lensfun
    if (q.lens.isEmpty() || q.focalLength <= 0)
        return lc;
    // lensfun's database lookups and modifiers are not thread-safe: photos decoded at the same
    // time (parallel exports) randomly found no lens and went without corrections.
    static std::mutex lensfunMutex;
    std::lock_guard lensfunLock(lensfunMutex);
    lfDatabase* db = lensDatabase();
    QByteArray make = q.cameraMake.toUtf8(), model = q.cameraModel.toUtf8(), lensName = q.lens.toUtf8();
    const lfCamera** cams = db->FindCamerasExt(make.constData(), model.constData());
    const lfCamera* cam = cams ? cams[0] : nullptr;
    const lfLens** lenses = db->FindLenses(cam, nullptr, lensName.constData());
    if (!lenses) {
        lf_free(cams);
        return lc;
    }
    const lfLens* lens = lenses[0];
    float crop = cam ? cam->CropFactor : lens->CropFactor;
    {
        lfModifier mod(lens, crop, W, H);
        float aperture = q.aperture > 0 ? float(q.aperture) : 8.0f;
        int done = mod.Initialize(lens, LF_PF_F32, float(q.focalLength), aperture, 1000.0f, 1.0f, LF_RECTILINEAR,
                                  LF_MODIFY_TCA | LF_MODIFY_VIGNETTING | LF_MODIFY_DISTORTION, false);
        if (done & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION)) {
#pragma omp parallel for schedule(static)
            for (int j = 0; j < gh; ++j) {
                for (int i = 0; i < gw; ++i) {
                    float res[6];
                    // lensfun places pixel centres on integer coordinates.
                    float x = nodeU(i) * W - 0.5f, y = nodeV(j) * H - 0.5f;
                    if (mod.ApplySubpixelGeometryDistortion(x, y, 1, 1, res)) {
                        float* src = &lc->m_src[(size_t(j) * gw + i) * 6];
                        for (int c = 0; c < 3; ++c) {
                            src[c * 2] = (res[c * 2] + 0.5f) / W;
                            src[c * 2 + 1] = (res[c * 2 + 1] + 0.5f) / H;
                        }
                    }
                }
            }
        }
        if (done & LF_MODIFY_VIGNETTING) {
#pragma omp parallel for schedule(static)
            for (int j = 0; j < gh; ++j) {
                for (int i = 0; i < gw; ++i) {
                    const float* src = &lc->m_src[(size_t(j) * gw + i) * 6];
                    float px[3] = {1.0f, 1.0f, 1.0f};
                    mod.ApplyColorModification(px, src[2] * W - 0.5f, src[3] * H - 0.5f, 1, 1, LF_CR_3(RED, GREEN, BLUE), sizeof(px));
                    float* gain = &lc->m_gain[(size_t(j) * gw + i) * 3];
                    for (int c = 0; c < 3; ++c)
                        gain[c] = px[c];
                }
            }
        }
        if (done) {
            lc->m_identity = false;
            lc->m_description = QStringLiteral("lensfun: %1").arg(QString::fromUtf8(lens->Model));
        }
    }
    lf_free(lenses);
    lf_free(cams);
    return lc;
}

void LensCorrection::sampleBilinear(const std::vector<float>& grid, int comps, float u, float v, float* out) const
{
    float fx = std::clamp(u, 0.0f, 1.0f) * (m_gw - 1), fy = std::clamp(v, 0.0f, 1.0f) * (m_gh - 1);
    int x0 = std::min(int(fx), m_gw - 2), y0 = std::min(int(fy), m_gh - 2);
    float tx = fx - x0, ty = fy - y0;
    const float* a = &grid[(size_t(y0) * m_gw + x0) * comps];
    const float* b = a + comps;
    const float* c = a + size_t(m_gw) * comps;
    const float* d = c + comps;
    for (int k = 0; k < comps; ++k)
        out[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
}

void LensCorrection::map(float u, float v, float distAmount, float vigAmount, float* srcXY, float* gain) const
{
    if (m_identity) {
        for (int c = 0; c < 3; ++c) {
            srcXY[c * 2] = u;
            srcXY[c * 2 + 1] = v;
            gain[c] = 1.0f;
        }
        return;
    }
    // Outside the frame the grid is extrapolated linearly from the edge displacement.
    float uc = std::clamp(u, 0.0f, 1.0f), vc = std::clamp(v, 0.0f, 1.0f);
    float s[6];
    sampleBilinear(m_src, 6, uc, vc, s);
    // Displacement of the green channel is scaled by the distortion amount;
    // red/blue keep their offsets relative to green (chromatic aberration).
    float gdx = s[2] - uc, gdy = s[3] - vc;
    for (int c = 0; c < 3; ++c) {
        float dx = s[c * 2] - s[2], dy = s[c * 2 + 1] - s[3];
        srcXY[c * 2] = u + distAmount * gdx + dx;
        srcXY[c * 2 + 1] = v + distAmount * gdy + dy;
    }
    sampleBilinear(m_gain, 3, uc, vc, gain);
    if (vigAmount != 1.0f)
        for (int c = 0; c < 3; ++c)
            gain[c] = std::pow(std::max(gain[c], 1e-3f), vigAmount);
}

double LensCorrection::autoScale(float distAmount) const
{
    if (m_identity)
        return 1.0;
    auto inside = [&](double k) {
        // Shrink the frame about its centre by 1/k and check all border samples map inside.
        const int n = 64;
        for (int i = 0; i <= n; ++i) {
            double t = double(i) / n;
            const double pts[4][2] = {{t, 0}, {t, 1}, {0, t}, {1, t}};
            for (const auto& p : pts) {
                float u = float(0.5 + (p[0] - 0.5) / k), v = float(0.5 + (p[1] - 0.5) / k);
                float src[6], g[3];
                map(u, v, distAmount, 1.0f, src, g);
                for (int c = 0; c < 3; ++c)
                    if (src[c * 2] < -1e-4f || src[c * 2] > 1.0001f || src[c * 2 + 1] < -1e-4f || src[c * 2 + 1] > 1.0001f)
                        return false;
            }
        }
        return true;
    };
    if (inside(1.0))
        return 1.0;
    double lo = 1.0, hi = 1.5;
    if (!inside(hi))
        return hi;
    for (int it = 0; it < 24; ++it) {
        double mid = 0.5 * (lo + hi);
        (inside(mid) ? hi : lo) = mid;
    }
    return hi;
}

} // namespace silma
