#include "Upright.h"

#include "Engine.h"
#include "Filters.h"

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
    double k = 1200.0 / std::max(frame.width(), frame.height());
    int w = int(frame.width() * k), h = int(frame.height() * k);
    ImageF img = engine.renderEncoded(s, {.width = w, .height = h, .ignoreCrop = true});
    PlaneF gray(w, h);
    for (size_t i = 0; i < gray.data.size(); ++i) {
        const float* p = img.data.data() + i * 3;
        gray.data[i] = 0.3f * p[0] + 0.55f * p[1] + 0.15f * p[2];
    }
    auto segs = detectLineSegments(gray, std::hypot(w, h) * 0.035);

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
    const double capRad = 10.0 * M_PI / 180;

    auto cost = [&](double pitch, double yaw, double roll, int* used) {
        Mat3 H = perspectiveMatrix(pitch, yaw, roll, f);
        double c = 0, wsum = 0;
        int n = 0;
        for (const auto& sg : vs) {
            double ax, ay, bx, by;
            applyH(H, sg.ax, sg.ay, ax, ay);
            applyH(H, sg.bx, sg.by, bx, by);
            double a = std::atan2(by - ay, bx - ax); // -pi..pi
            // Deviation from vertical / horizontal.
            double dv = std::abs(std::remainder(a - M_PI / 2, M_PI));
            double dh = std::abs(std::remainder(a, M_PI));
            double d;
            if (useV && dv < capRad * 2.5 && (!useH || dv <= dh))
                d = dv;
            else if (useH && dh < capRad * 2.5)
                d = dh;
            else
                continue;
            ++n;
            // Robust (truncated quadratic) loss.
            c += sg.w * std::min(d * d, capRad * capRad);
            wsum += sg.w;
        }
        if (used)
            *used = n;
        if (wsum <= 0)
            return 1e9;
        // Lines that are not near-vertical/horizontal count as "capped" so the search
        // doesn't win by pushing everything out of the windows; small regularization too.
        double totalW = 0;
        for (const auto& sg : vs)
            totalW += sg.w;
        c += (totalW - wsum) * capRad * capRad;
        return c / totalW + 1e-5 * (pitch * pitch + yaw * yaw + roll * roll);
    };

    double best = 1e18, bp = 0, by = 0, br = 0;
    for (double p = useV ? -kMax : 0; p <= (useV ? kMax : 0) + 1e-9; p += 2.5)
        for (double y = useH ? -kMax : 0; y <= (useH ? kMax : 0) + 1e-9; y += 2.5)
            for (double r = -10; r <= 10 + 1e-9; r += 1.0) {
                double c = cost(p, y, r, nullptr);
                if (c < best) {
                    best = c;
                    bp = p;
                    by = y;
                    br = r;
                }
            }
    // Coordinate descent refinement.
    for (double step : {1.0, 0.5, 0.25, 0.1, 0.05}) {
        for (int it = 0; it < 20; ++it) {
            bool improved = false;
            for (int axis = 0; axis < 3; ++axis) {
                if ((axis == 0 && !useV) || (axis == 1 && !useH))
                    continue;
                for (double dir : {-1.0, 1.0}) {
                    double p = bp + (axis == 0) * dir * step, y = by + (axis == 1) * dir * step, r = br + (axis == 2) * dir * step;
                    if (std::abs(p) > kMax || std::abs(y) > kMax || std::abs(r) > 15)
                        continue;
                    double c = cost(p, y, r, nullptr);
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
    cost(bp, by, br, &res.segmentsUsed);
    // Geometry maps slider v -> pitch = -v/100*kMax and h -> yaw = h/100*kMax.
    res.vertical = std::round(-bp / kMax * 100);
    res.horizontal = std::round(by / kMax * 100);
    res.rotate = std::round(br * 10) / 10;
    res.ok = res.segmentsUsed >= 2;
    return res;
}

QRectF autoCropRect(const Geometry& g, double aspect)
{
    QSizeF cs = g.canvasSize();
    // Normalized size for scale t: width t*W0, height t*H0 fitting the aspect in the canvas.
    double wN = 1, hN = 1;
    double canvasAspect = cs.width() / cs.height();
    if (aspect > canvasAspect)
        hN = canvasAspect / aspect;
    else
        wN = aspect / canvasAspect;
    auto valid = [&](double t) {
        double hw = wN * t / 2, hh = hN * t / 2;
        const int n = 48;
        for (int i = 0; i <= n; ++i) {
            double u = double(i) / n;
            const QPointF pts[4] = {{0.5 - hw + 2 * hw * u, 0.5 - hh}, {0.5 - hw + 2 * hw * u, 0.5 + hh}, {0.5 - hw, 0.5 - hh + 2 * hh * u}, {0.5 + hw, 0.5 - hh + 2 * hh * u}};
            for (const auto& p : pts)
                if (!g.canvasValid(p))
                    return false;
        }
        return true;
    };
    double lo = 0.05, hi = 1.0;
    if (valid(1.0))
        lo = 1.0;
    else
        for (int it = 0; it < 30; ++it) {
            double mid = 0.5 * (lo + hi);
            (valid(mid) ? lo : hi) = mid;
        }
    double hw = wN * lo / 2, hh = hN * lo / 2;
    return QRectF(0.5 - hw, 0.5 - hh, 2 * hw, 2 * hh);
}

} // namespace silma
