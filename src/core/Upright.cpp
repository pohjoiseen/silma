#include "Upright.h"

#include "Engine.h"
#include "Filters.h"
#include "Merge.h"

#include <algorithm>
#include <cmath>

namespace silma {

double LineSegment::length() const { return std::hypot(b.x() - a.x(), b.y() - a.y()); }

std::vector<LineSegment> detectLineSegments(const PlaneF& grayIn, double minLength)
{
    const int w = grayIn.width, h = grayIn.height;
    std::vector<LineSegment> out;
    if (w < 16 || h < 16)
        return out;
    PlaneF g = gaussianBlur(grayIn, 0.8f);
    // Gradients (Sobel).
    PlaneF mag(w, h), ang(w, h);
    for (int y = 1; y < h - 1; ++y)
        for (int x = 1; x < w - 1; ++x) {
            float gx = (g.at(x + 1, y - 1) + 2 * g.at(x + 1, y) + g.at(x + 1, y + 1)) - (g.at(x - 1, y - 1) + 2 * g.at(x - 1, y) + g.at(x - 1, y + 1));
            float gy = (g.at(x - 1, y + 1) + 2 * g.at(x, y + 1) + g.at(x + 1, y + 1)) - (g.at(x - 1, y - 1) + 2 * g.at(x, y - 1) + g.at(x + 1, y - 1));
            mag.at(x, y) = std::hypot(gx, gy);
            ang.at(x, y) = std::atan2(gy, gx); // gradient direction; the line is perpendicular
        }
    // Threshold from the magnitude distribution (keep the strongest ~15%, at least a floor).
    std::vector<float> sorted(mag.data);
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() * 85 / 100, sorted.end());
    float thr = std::max(sorted[sorted.size() * 85 / 100], 0.06f);

    std::vector<uint8_t> used(size_t(w) * h, 0);
    // Seeds in decreasing magnitude order.
    std::vector<int> seeds;
    for (int i = 0; i < w * h; ++i)
        if (mag.data[i] > thr * 1.5f)
            seeds.push_back(i);
    std::sort(seeds.begin(), seeds.end(), [&](int a, int b) { return mag.data[a] > mag.data[b]; });

    const float tol = float(M_PI / 8); // 22.5 degrees
    auto angDiff = [](float a, float b) {
        float d = std::fmod(std::abs(a - b), float(2 * M_PI));
        return std::min(d, float(2 * M_PI) - d);
    };
    std::vector<int> region;
    for (int seed : seeds) {
        if (used[seed])
            continue;
        region.clear();
        region.push_back(seed);
        used[seed] = 1;
        double sx = std::cos(ang.data[seed]), sy = std::sin(ang.data[seed]);
        float regAng = ang.data[seed];
        for (size_t k = 0; k < region.size(); ++k) {
            int px = region[k] % w, py = region[k] / w;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = px + dx, ny = py + dy;
                    if (nx < 1 || ny < 1 || nx >= w - 1 || ny >= h - 1)
                        continue;
                    int ni = ny * w + nx;
                    if (used[ni] || mag.data[ni] < thr || angDiff(ang.data[ni], regAng) > tol)
                        continue;
                    used[ni] = 1;
                    region.push_back(ni);
                    sx += std::cos(ang.data[ni]);
                    sy += std::sin(ang.data[ni]);
                    regAng = float(std::atan2(sy, sx));
                }
            if (region.size() > 20000)
                break;
        }
        if (region.size() < size_t(std::max(8.0, minLength * 0.8)))
            continue;
        // Fit a line by PCA of the region pixels (weighted by magnitude).
        double sw = 0, mx = 0, my = 0;
        for (int i : region) {
            double wgt = mag.data[i];
            sw += wgt;
            mx += wgt * (i % w);
            my += wgt * (i / w);
        }
        mx /= sw;
        my /= sw;
        double cxx = 0, cyy = 0, cxy = 0;
        for (int i : region) {
            double wgt = mag.data[i], dx = i % w - mx, dy = i / w - my;
            cxx += wgt * dx * dx;
            cyy += wgt * dy * dy;
            cxy += wgt * dx * dy;
        }
        double tr = cxx + cyy, det = cxx * cyy - cxy * cxy;
        double l1 = tr / 2 + std::sqrt(std::max(0.0, tr * tr / 4 - det)), l2 = tr / 2 - std::sqrt(std::max(0.0, tr * tr / 4 - det));
        if (l1 <= 0 || l2 / l1 > 0.02)
            continue; // not elongated enough: curve or blob
        double theta = 0.5 * std::atan2(2 * cxy, cxx - cyy);
        double dx = std::cos(theta), dy = std::sin(theta);
        // Extent along the direction.
        double tmin = 1e9, tmax = -1e9;
        for (int i : region) {
            double t = (i % w - mx) * dx + (i / w - my) * dy;
            tmin = std::min(tmin, t);
            tmax = std::max(tmax, t);
        }
        if (tmax - tmin < minLength)
            continue;
        out.push_back({QPointF(mx + dx * tmin, my + dy * tmin), QPointF(mx + dx * tmax, my + dy * tmax)});
    }
    return out;
}

namespace {

struct Seg {
    double ax, ay, bx, by, w;
};

inline void applyH(const Mat3& h, double x, double y, double& ox, double& oy)
{
    double z = h[2][0] * x + h[2][1] * y + h[2][2];
    ox = (h[0][0] * x + h[0][1] * y + h[0][2]) / z;
    oy = (h[1][0] * x + h[1][1] * y + h[1][2]) / z;
}

} // namespace

UprightResult autoUpright(const DevelopEngine& engine, const DevelopSettings& in, UprightMode mode)
{
    UprightResult res;
    // Render the lens-corrected frame without any transform.
    DevelopSettings s = in;
    s.perspVertical = s.perspHorizontal = s.rotate = 0;
    s.scale = 100;
    s.crop = QRectF(0, 0, 1, 1);
    s.sharpenAmount = 0;
    Geometry geo = engine.geometry(s, true);
    QSizeF frame = geo.canvasSize();
    double k = 2000.0 / std::max(frame.width(), frame.height());
    int w = int(frame.width() * k), h = int(frame.height() * k);
    ImageF img = engine.renderEncoded(s, {.width = w, .height = h, .ignoreCrop = true});
    PlaneF gray(w, h);
    for (size_t i = 0; i < gray.data.size(); ++i) {
        const float* p = img.data.data() + i * 3;
        gray.data[i] = 0.3f * p[0] + 0.55f * p[1] + 0.15f * p[2];
    }
    // Stretch the contrast so that dim and flat (overcast) photos give as many lines as
    // bright ones: the detector's gradient floor is absolute.
    {
        std::vector<float> v(gray.data);
        auto lo = v.begin() + v.size() / 100, hi = v.begin() + v.size() * 995 / 1000;
        std::nth_element(v.begin(), lo, v.end());
        const float l = *lo;
        std::nth_element(v.begin(), hi, v.end());
        const float scale = 1.0f / std::max(*hi - l, 0.05f);
        for (float& g : gray.data)
            g = std::clamp((g - l) * scale, 0.0f, 1.0f);
    }
    auto segs = detectLineSegments(gray, std::hypot(w, h) * 0.02);

    // Segments in centred full-resolution frame pixels (as used by the homography).
    std::vector<Seg> vs;
    for (const auto& l : segs) {
        Seg sg{(l.a.x() / w - 0.5) * frame.width(), (l.a.y() / h - 0.5) * frame.height(), (l.b.x() / w - 0.5) * frame.width(), (l.b.y() / h - 0.5) * frame.height(), 0};
        sg.w = std::hypot(sg.bx - sg.ax, sg.by - sg.ay);
        vs.push_back(sg);
    }
    if (vs.size() < 2)
        return res;
    const double f = geo.focalPx();
    const double kMax = 25.0; // must match Geometry's slider mapping
    const bool useV = mode != UprightMode::Horizontal, useH = mode != UprightMode::Vertical;
    const double d2r = M_PI / 180;

    // Each line is either a true vertical (or horizontal) whose angle after correction is
    // off by measurement noise only, or an outlier with a uniformly distributed angle.
    // The noise grows for short lines (endpoint error over the analysis-resolution length).
    // Minimizing the negative log-likelihood plus a mild prior on the angles lets many good
    // lines decide alone, while a few stray ones can't push the correction far.
    const double kModelSigma = 0.25 * d2r, kEndpointPx = 0.7, kOutlier = 0.3;
    const double kPitchSigma = 10, kYawSigma = 5, kRollSigma = 3; // degrees
    const double toAnalysis = double(w) / frame.width();
    std::vector<double> sigma(vs.size());
    for (size_t i = 0; i < vs.size(); ++i)
        sigma[i] = std::hypot(kModelSigma, kEndpointPx / (vs[i].w * toAnalysis));
    // Only lines already within kCandidate of vertical (horizontal) can become verticals
    // (horizontals): a receding edge or a diagonal would otherwise get "straightened" by an
    // extreme correction.
    const double kCandidate = 15 * d2r;
    enum Kind : uint8_t { Other, Vertical, Horizontal };
    std::vector<Kind> kind(vs.size(), Other);
    for (size_t i = 0; i < vs.size(); ++i) {
        double a = std::atan2(vs[i].by - vs[i].ay, vs[i].bx - vs[i].ax);
        if (useV && std::abs(std::remainder(a - M_PI / 2, M_PI)) < kCandidate)
            kind[i] = Vertical;
        else if (useH && std::abs(std::remainder(a, M_PI)) < kCandidate)
            kind[i] = Horizontal;
    }

    auto deviations = [&](double pitch, double yaw, double roll, std::vector<double>& dev) {
        Mat3 H = perspectiveMatrix(pitch, yaw, roll, f);
        dev.resize(vs.size());
        for (size_t i = 0; i < vs.size(); ++i) {
            if (kind[i] == Other) {
                dev[i] = M_PI;
                continue;
            }
            const Seg& sg = vs[i];
            double ax, ay, bx, by;
            applyH(H, sg.ax, sg.ay, ax, ay);
            applyH(H, sg.bx, sg.by, bx, by);
            double a = std::atan2(by - ay, bx - ax);
            dev[i] = std::abs(std::remainder(kind[i] == Vertical ? a - M_PI / 2 : a, M_PI));
        }
    };
    std::vector<double> dev;
    auto cost = [&](double pitch, double yaw, double roll) {
        deviations(pitch, yaw, roll, dev);
        const double outlierDensity = kOutlier / (M_PI / 2);
        double c = 0;
        for (size_t i = 0; i < vs.size(); ++i) {
            double z = dev[i] / sigma[i];
            c -= std::log((1 - kOutlier) * std::sqrt(2 / M_PI) / sigma[i] * std::exp(-0.5 * z * z) + outlierDensity);
        }
        return c + 0.5 * pitch * pitch / (kPitchSigma * kPitchSigma) + 0.5 * yaw * yaw / (kYawSigma * kYawSigma) + 0.5 * roll * roll / (kRollSigma * kRollSigma);
    };
    auto inliers = [&](double pitch, double yaw, double roll) {
        deviations(pitch, yaw, roll, dev);
        return int(std::count_if(dev.begin(), dev.end(), [&](double d) { return d < 1.0 * d2r; }));
    };

    // Grid search, then coordinate descent; perspective axes optional (roll-only fallback).
    auto solve = [&](bool fitV, bool fitH, double& bp, double& by, double& br, double& best) {
        best = 1e300;
        const double ps = fitV && fitH ? 1.0 : 0.5;
        for (double p = fitV ? -kMax : 0; p <= (fitV ? kMax : 0) + 1e-9; p += ps)
            for (double y = fitH ? -kMax : 0; y <= (fitH ? kMax : 0) + 1e-9; y += ps)
                for (double r = -10; r <= 10 + 1e-9; r += 0.5) {
                    double c = cost(p, y, r);
                    if (c < best) {
                        best = c;
                        bp = p;
                        by = y;
                        br = r;
                    }
                }
        for (double step : {0.25, 0.1, 0.05, 0.02, 0.01}) {
            for (int it = 0; it < 30; ++it) {
                bool improved = false;
                for (int axis = 0; axis < 3; ++axis) {
                    if ((axis == 0 && !fitV) || (axis == 1 && !fitH))
                        continue;
                    for (double dir : {-1.0, 1.0}) {
                        double p = bp + (axis == 0) * dir * step, y = by + (axis == 1) * dir * step, r = br + (axis == 2) * dir * step;
                        if (std::abs(p) > kMax || std::abs(y) > kMax || std::abs(r) > 15)
                            continue;
                        double c = cost(p, y, r);
                        if (c < best) {
                            best = c;
                            bp = p;
                            by = y;
                            br = r;
                            improved = true;
                        }
                    }
                }
                if (!improved)
                    break;
            }
        }
        return inliers(bp, by, br);
    };
    // Two parameters can always align two lines and often a few more by chance, and short
    // segments along one textured surface are not independent: a perspective correction must
    // be backed by several lines and explain them clearly better than straightening alone.
    const int kMinPerspectiveLines = 8, kMinRollLines = 6;
    const double kMinGain = 8; // negative log-likelihood units
    double bp = 0, by = 0, br = 0, rp = 0, ry = 0, rr = 0;
    double full = 0, rollOnly = 0;
    res.segmentsUsed = solve(useV, useH, bp, by, br, full);
    const int rollLines = solve(false, false, rp, ry, rr, rollOnly);
    if (res.segmentsUsed < kMinPerspectiveLines || rollOnly - full < kMinGain) {
        bp = rp;
        by = ry;
        br = rr;
        res.segmentsUsed = rollLines;
    }
    // Geometry maps slider v -> pitch = -v/100*kMax and h -> yaw = h/100*kMax.
    res.vertical = std::round(-bp / kMax * 100);
    res.horizontal = std::round(by / kMax * 100);
    res.rotate = std::round(br * 10) / 10;
    res.ok = res.segmentsUsed >= kMinRollLines;
    return res;
}

namespace {

// Masked sources: the valid area can have any shape, so search a grid of canvas samples.
QRectF maskedCropRect(const Geometry& g, double aspect)
{
    const QSizeF cs = g.canvasSize();
    // Square cells (in pixels), ~600 along the long side.
    const double cell = std::max(cs.width(), cs.height()) / 600.0;
    const int nx = std::max(4, int(cs.width() / cell)), ny = std::max(4, int(cs.height() / cell));
    std::vector<uint8_t> valid(size_t(nx) * ny);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x)
            valid[size_t(y) * nx + x] = g.canvasValid({(x + 0.5) / nx, (y + 0.5) / ny});

    QRect r;
    if (aspect <= 0) {
        r = largestRectangle(valid, nx, ny);
    } else {
        // Largest w x round(w / aspect) window without invalid cells (binary search on w,
        // summed-area table for the window test).
        std::vector<int> bad(size_t(nx + 1) * (ny + 1), 0);
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x)
                bad[size_t(y + 1) * (nx + 1) + x + 1] = !valid[size_t(y) * nx + x] + bad[size_t(y) * (nx + 1) + x + 1]
                                                         + bad[size_t(y + 1) * (nx + 1) + x] - bad[size_t(y) * (nx + 1) + x];
        // A fitting window of width w (the most central one when `central`).
        auto find = [&](int w, bool central) -> QRect {
            const int h = std::max(1, int(std::lround(w / aspect)));
            if (w > nx || h > ny)
                return {};
            QRect best;
            double bestDist = 1e30;
            for (int y = 0; y + h <= ny; ++y)
                for (int x = 0; x + w <= nx; ++x) {
                    auto at = [&](int xx, int yy) { return bad[size_t(yy) * (nx + 1) + xx]; };
                    if (at(x + w, y + h) - at(x, y + h) - at(x + w, y) + at(x, y) != 0)
                        continue;
                    if (!central)
                        return QRect(x, y, w, h);
                    double d = std::hypot(x + w / 2.0 - nx / 2.0, y + h / 2.0 - ny / 2.0);
                    if (d < bestDist) {
                        bestDist = d;
                        best = QRect(x, y, w, h);
                    }
                }
            return best;
        };
        int lo = 0, hi = nx;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (find(mid, false).isValid())
                lo = mid;
            else
                hi = mid - 1;
        }
        r = lo > 0 ? find(lo, true) : QRect();
    }
    if (r.width() < 3 || r.height() < 3)
        return QRectF(0, 0, 1, 1);
    // Cells were tested at their centres: stay half a cell inside.
    return QRectF((r.x() + 0.5) / nx, (r.y() + 0.5) / ny, (r.width() - 1.0) / nx, (r.height() - 1.0) / ny);
}

} // namespace

namespace {

// Whether the rectangle (normalized canvas coordinates) holds only valid image data,
// checked along its edges.
bool rectValid(const Geometry& g, const QRectF& r)
{
    const int n = 48;
    for (int i = 0; i <= n; ++i) {
        const double u = double(i) / n;
        const QPointF pts[4] = {{r.left() + r.width() * u, r.top()}, {r.left() + r.width() * u, r.bottom()},
                                {r.left(), r.top() + r.height() * u}, {r.right(), r.top() + r.height() * u}};
        for (const auto& p : pts)
            if (!g.canvasValid(p))
                return false;
    }
    return true;
}

// The rectangle scaled by `t` about its centre.
QRectF scaledAboutCentre(const QRectF& r, double t)
{
    QRectF s(0, 0, r.width() * t, r.height() * t);
    s.moveCenter(r.center());
    return s;
}

} // namespace

QRectF autoCropRect(const Geometry& g, double aspect)
{
    if (g.hasValidMask())
        return maskedCropRect(g, aspect);
    QSizeF cs = g.canvasSize();
    // Normalized size for scale t: width t*W0, height t*H0 fitting the aspect in the canvas.
    double wN = 1, hN = 1;
    double canvasAspect = cs.width() / cs.height();
    if (aspect > canvasAspect)
        hN = canvasAspect / aspect;
    else
        wN = aspect / canvasAspect;
    const QRectF full(0.5 - wN / 2, 0.5 - hN / 2, wN, hN);
    double lo = 0.05, hi = 1.0;
    if (rectValid(g, full))
        lo = 1.0;
    else
        for (int it = 0; it < 30; ++it) {
            double mid = 0.5 * (lo + hi);
            (rectValid(g, scaledAboutCentre(full, mid)) ? lo : hi) = mid;
        }
    return scaledAboutCentre(full, lo);
}

EmptyCrop emptyCropBorders(const Geometry& g, const QRectF& crop)
{
    EmptyCrop r;
    const int n = 100;
    int empty = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            empty += !g.canvasValid({crop.left() + crop.width() * (x + 0.5) / n, crop.top() + crop.height() * (y + 0.5) / n});
    r.fraction = double(empty) / (n * n);
    // How far in from the edges the empty parts reach, in full-resolution pixels: the
    // smallest inset (from the outermost pixels' centres, half a pixel in) that is all image.
    const QSizeF cs = g.canvasSize();
    if (cs.isEmpty())
        return r;
    auto inset = [&](double px) {
        const double ix = px / cs.width(), iy = px / cs.height();
        return crop.adjusted(ix, iy, -ix, -iy);
    };
    if (rectValid(g, inset(0.5)))
        return r;
    double lo = 0.5, hi = std::min(cs.width() * crop.width(), cs.height() * crop.height()) / 2;
    if (!rectValid(g, inset(hi)))
        lo = hi;
    else
        for (int it = 0; it < 24 && hi - lo > 0.25; ++it) {
            const double mid = 0.5 * (lo + hi);
            (rectValid(g, inset(mid)) ? hi : lo) = mid;
        }
    r.depthPx = hi - 0.5;
    return r;
}

bool cropConstraintApplies(const DevelopSettings& before, const DevelopSettings& after)
{
    if (!after.constrainCrop || after.crop != before.crop || after.quarterTurns != before.quarterTurns)
        return false;
    const bool geometryChanged = after.perspVertical != before.perspVertical || after.perspHorizontal != before.perspHorizontal
                                 || after.rotate != before.rotate || after.scale != before.scale
                                 || after.lensDistortion != before.lensDistortion;
    return geometryChanged || !before.constrainCrop;
}

DevelopSettings constrainedCrop(const DevelopEngine& engine, const DevelopSettings& before, DevelopSettings after)
{
    if (!cropConstraintApplies(before, after))
        return after;
    const Geometry gBefore = engine.geometry(before, true), gAfter = engine.geometry(after, true);
    const QSizeF cs = gAfter.canvasSize();
    const QRectF crop = before.crop;
    const double aspect = crop.width() * cs.width() / (crop.height() * cs.height());
    auto near = [](const QRectF& a, const QRectF& b) {
        return std::abs(a.left() - b.left()) < 1e-3 && std::abs(a.top() - b.top()) < 1e-3 && std::abs(a.right() - b.right()) < 1e-3
               && std::abs(a.bottom() - b.bottom()) < 1e-3;
    };
    if (crop == QRectF(0, 0, 1, 1) || near(crop, autoCropRect(gBefore, aspect))) {
        after.crop = autoCropRect(gAfter, aspect);
    } else if (!rectValid(gAfter, crop)) {
        double lo = 0, hi = 1;
        for (int it = 0; it < 30; ++it) {
            double mid = 0.5 * (lo + hi);
            (rectValid(gAfter, scaledAboutCentre(crop, mid)) ? lo : hi) = mid;
        }
        // The centre itself is outside the image: start over from the automatic crop.
        after.crop = lo > 0.05 ? scaledAboutCentre(crop, lo) : autoCropRect(gAfter, aspect);
    }
    return after;
}

} // namespace silma
