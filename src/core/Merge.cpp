#include "Merge.h"

#include "Engine.h"
#include "Metadata.h"

#include <QFileInfo>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <opencv2/core.hpp>
#include <opencv2/features.hpp>
#include <opencv2/geometry.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>
#include <opencv2/stitching/detail/exposure_compensate.hpp>
#include <opencv2/stitching/detail/matchers.hpp>
#include <opencv2/stitching/detail/motion_estimators.hpp>
#include <opencv2/stitching/detail/seam_finders.hpp>
#include <opencv2/stitching/detail/warpers.hpp>
#include <opencv2/video/tracking.hpp>
#include <stdexcept>

namespace silma {

namespace {

void checkCancel(const std::atomic<bool>* c)
{
    if (c && *c)
        throw std::runtime_error("Cancelled");
}

void report(const StepProgress& p, const QString& step, int d = 0, int t = 0)
{
    if (p)
        p(step, d, t);
}

double exposureValue(const PhotoInfo& p)
{
    double t = p.exposureTime > 0 ? p.exposureTime : 1.0;
    double iso = p.iso > 0 ? p.iso : 100.0;
    double n = p.fNumber > 0 ? p.fNumber : 1.0;
    return t * iso / (n * n);
}

cv::Mat toMat(ImageF& img) { return cv::Mat(img.height, img.width, CV_32FC3, img.data.data()); }

ImageF fromMat(const cv::Mat& m)
{
    ImageF out(m.cols, m.rows);
    cv::Mat dst(m.rows, m.cols, CV_32FC3, out.data.data());
    m.copyTo(dst);
    return out;
}

// 8-bit image from encoded floats (0..1).
cv::Mat to8bit(const ImageF& enc)
{
    cv::Mat m(enc.height, enc.width, CV_8UC3);
    for (int y = 0; y < enc.height; ++y) {
        const float* s = enc.row(y);
        uchar* d = m.ptr<uchar>(y);
        for (int x = 0; x < enc.width * 3; ++x)
            d[x] = uchar(std::clamp(int(s[x] * 255.0f + 0.5f), 0, 255));
    }
    return m;
}

DevelopSettings settingsWithAutoTone(const QString& path, bool autoTone)
{
    DevelopSettings s;
    if (!autoTone)
        return s;
    try {
        DevelopEngine e(path, {.halfSize = true});
        s = e.autoTone(s);
    } catch (...) {
    }
    return s;
}

} // namespace

QList<MergeSource> mergeSources(Batch& b, const QList<qint64>& ids)
{
    QList<MergeSource> out;
    for (qint64 id : ids)
        if (auto p = b.photo(id))
            out << MergeSource{*p, DevelopSettings::fromJsonString(b.settings(id)).quarterTurns};
    return out;
}

// ---- HDR --------------------------------------------------------------------------

namespace {

// A merged HDR image, before it becomes a DNG.
struct HdrMerged {
    ImageF out; // camera-native linear, normalized by info.whiteScale
    LinearDngInfo info;
    QList<MergeSource> sources; // by exposure
    int ref = 0;
};

// Merges bracketed exposures at full resolution, or (`preview`) at about 1000 pixels from
// fast half-size decodes (the ghost masks then work at about the same scale as the merge's).
HdrMerged mergeHdr(QList<MergeSource> sources, const HdrOptions& o, bool preview, const StepProgress& progress, const std::atomic<bool>* cancel)
{
    if (sources.size() < 2)
        throw std::runtime_error("HDR merge needs at least two photos");
    std::sort(sources.begin(), sources.end(), [](const MergeSource& a, const MergeSource& c) { return exposureValue(a.photo) < exposureValue(c.photo); });
    QList<PhotoInfo> photos;
    for (const auto& src : sources)
        photos << src.photo;
    const int n = int(photos.size());
    const int ref = n / 2;

    std::vector<RawImage> raws(n);
    for (int i = 0; i < n; ++i) {
        report(progress, QStringLiteral("Decoding"), i, n);
        checkCancel(cancel);
        raws[i] = decodeRaw(photos[i].path, {.halfSize = preview});
        if (preview) {
            const int f = std::max(1, int(std::lround(std::max(raws[i].image.width, raws[i].image.height) / 1000.0)));
            raws[i].image = downscale(raws[i].image, f);
        }
        if (raws[i].image.width != raws[0].image.width || raws[i].image.height != raws[0].image.height)
            throw std::runtime_error("HDR merge needs photos of the same size");
    }
    const int W = raws[0].image.width, H = raws[0].image.height;

    // Camera-native data and relative exposure (scale to the reference).
    std::vector<double> k(n);
    for (int i = 0; i < n; ++i)
        k[i] = exposureValue(photos[ref]) / exposureValue(photos[i]);
    // Refine with the data: median ratio of the green channel where both are well exposed.
    auto native = [&](int i, int x, int y, int c) { return raws[i].image.px(x, y)[c] / float(raws[i].color.appliedMul[c]); };
    for (int i = 0; i < n; ++i) {
        if (i == ref)
            continue;
        std::vector<float> ratios;
        for (int y = 0; y < H; y += 8)
            for (int x = 0; x < W; x += 8) {
                float a = raws[i].image.px(x, y)[1], r = raws[ref].image.px(x, y)[1];
                if (a > 0.03f && a < 0.85f && r > 0.03f && r < 0.85f)
                    ratios.push_back(native(ref, x, y, 1) / native(i, x, y, 1));
            }
        if (ratios.size() > 1000) {
            std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
            double r = ratios[ratios.size() / 2];
            if (r > k[i] / 2 && r < k[i] * 2)
                k[i] = r;
        }
    }

    // Alignment: SIFT + RANSAC homography on exposure-normalized luminance previews (handles
    // handheld shifts and rotation); ECC (Euclidean) as a fallback.
    std::vector<cv::Mat> warps(n); // 3x3 homographies mapping reference coords -> image coords
    if (o.align) {
        const int f = std::max(1, std::max(W, H) / 1600);
        auto lumMat = [&](int i) {
            cv::Mat m(H / f, W / f, CV_8U);
            for (int y = 0; y < m.rows; ++y)
                for (int x = 0; x < m.cols; ++x) {
                    double v = native(i, x * f, y * f, 1) * k[i];
                    double l = std::log2(std::max(v, 1e-5));
                    m.at<uchar>(y, x) = uchar(std::clamp((l + 12.0) / 14.0 * 255.0, 0.0, 255.0));
                }
            return m;
        };
        cv::Mat refM = lumMat(ref);
        cv::Ptr<cv::SIFT> sift = cv::SIFT::create(4000);
        std::vector<cv::KeyPoint> refKp;
        cv::Mat refDesc;
        sift->detectAndCompute(refM, cv::noArray(), refKp, refDesc);
        cv::BFMatcher bf(cv::NORM_L2);
        for (int i = 0; i < n; ++i) {
            if (i == ref)
                continue;
            report(progress, QStringLiteral("Aligning"), i, n);
            checkCancel(cancel);
            cv::Mat m = lumMat(i);
            std::vector<cv::KeyPoint> kp;
            cv::Mat desc;
            sift->detectAndCompute(m, cv::noArray(), kp, desc);
            cv::Mat Hm;
            if (!desc.empty() && !refDesc.empty()) {
                std::vector<std::vector<cv::DMatch>> knn;
                bf.knnMatch(refDesc, desc, knn, 2);
                std::vector<cv::Point2f> a, c;
                for (const auto& mm : knn)
                    if (mm.size() == 2 && mm[0].distance < 0.75f * mm[1].distance) {
                        a.push_back(refKp[mm[0].queryIdx].pt);
                        c.push_back(kp[mm[0].trainIdx].pt);
                    }
                if (a.size() >= 12) {
                    std::vector<uchar> inl;
                    Hm = cv::findHomography(a, c, cv::RANSAC, 2.0, inl);
                    int nin = int(std::count(inl.begin(), inl.end(), 1));
                    if (nin < 12)
                        Hm = cv::Mat();
                }
            }
            if (Hm.empty()) {
                cv::Mat w = cv::Mat::eye(2, 3, CV_32F);
                try {
                    cv::findTransformECC(refM, m, w, cv::MOTION_EUCLIDEAN, cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 80, 1e-5), cv::noArray(), 5);
                } catch (const cv::Exception&) {
                }
                Hm = cv::Mat::eye(3, 3, CV_64F);
                cv::Mat(w).convertTo(Hm(cv::Rect(0, 0, 3, 2)), CV_64F);
            }
            // Scale from preview to full-resolution coordinates.
            cv::Mat S(cv::Matx33d(f, 0, 0, 0, f, 0, 0, 0, 1)), Si = S.inv();
            warps[i] = S * Hm * Si;
        }
    }
    // Apply alignment to the white-balanced data (used for weights and values).
    for (int i = 0; i < n; ++i) {
        if (warps[i].empty())
            continue;
        cv::Mat src = toMat(raws[i].image);
        cv::Mat dst;
        cv::warpPerspective(src, dst, warps[i], src.size(), cv::INTER_LINEAR | cv::WARP_INVERSE_MAP, cv::BORDER_REPLICATE);
        raws[i].image = fromMat(dst);
    }

    // Ghost masks: regions where an image disagrees with the reference (moving people,
    // leaves) get no weight. Computed at low resolution, dilated and softened.
    std::vector<cv::Mat> ghost(n); // CV_32F, 1 = usable
    const int gf = std::max(1, std::max(W, H) / 800);
    if (o.deghost) {
        auto logLum = [&](int i) {
            cv::Mat m(H / gf, W / gf, CV_32F), v(H / gf, W / gf, CV_8U);
            for (int y = 0; y < m.rows; ++y)
                for (int x = 0; x < m.cols; ++x) {
                    const float* p = raws[i].image.px(x * gf, y * gf);
                    float t = std::max({p[0], p[1], p[2]});
                    v.at<uchar>(y, x) = t > 0.003f && t < 0.93f ? 255 : 0;
                    m.at<float>(y, x) = std::log2(std::max(native(i, x * gf, y * gf, 1) * float(k[i]), 1e-6f));
                }
            cv::GaussianBlur(m, m, cv::Size(), 1.5);
            return std::make_pair(m, v);
        };
        auto [refL, refV] = logLum(ref);
        for (int i = 0; i < n; ++i) {
            if (i == ref)
                continue;
            auto [L, V] = logLum(i);
            cv::Mat diff = cv::abs(L - refL);
            cv::Mat bad = (diff > 0.2f) & V & refV;
            cv::dilate(bad, bad, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(11, 11)));
            cv::Mat g;
            bad.convertTo(g, CV_32F, 1.0 / 255);
            cv::GaussianBlur(g, g, cv::Size(), 3);
            ghost[i] = 1.0f - cv::min(g * 1.5f, 1.0f);

        }
    }

    report(progress, QStringLiteral("Merging"));
    checkCancel(cancel);
    ImageF out(W, H);
    const RawColorInfo& rc = raws[ref].color;
#pragma omp parallel for schedule(dynamic, 16)
    for (int y = 0; y < H; ++y) {
        std::vector<float> wts(n);
        for (int x = 0; x < W; ++x) {
            bool refValid = false;
            {
                const float* p = raws[ref].image.px(x, y);
                float t = std::max({p[0], p[1], p[2]});
                // Dark reference pixels are noisy but still show where things are; only clipped
                // ones cannot be trusted for deghosting.
                refValid = t > 0.002f && t < 0.95f;
            }
            double wsum = 0;
            for (int i = 0; i < n; ++i) {
                const float* p = raws[i].image.px(x, y);
                float t = std::max({p[0], p[1], p[2]});
                // Hat weight: avoid noisy shadows and clipped highlights; favour long exposures.
                float w = std::clamp((t - 0.002f) / 0.05f, 0.0f, 1.0f) * std::clamp((0.96f - t) / 0.12f, 0.0f, 1.0f);
                w *= float(1.0 / k[i]); // exposure time relative to the reference
                if (!ghost[i].empty() && w > 0) {
                    // Bilinear sample of the low-resolution ghost mask.
                    float gx = std::min((x + 0.5f) / gf - 0.5f, float(ghost[i].cols - 1)), gy = std::min((y + 0.5f) / gf - 0.5f, float(ghost[i].rows - 1));
                    gx = std::max(gx, 0.0f);
                    gy = std::max(gy, 0.0f);
                    int x0 = int(gx), y0 = int(gy), x1 = std::min(x0 + 1, ghost[i].cols - 1), y1 = std::min(y0 + 1, ghost[i].rows - 1);
                    float tx = gx - x0, ty = gy - y0;
                    const cv::Mat& G = ghost[i];
                    float g = (G.at<float>(y0, x0) * (1 - tx) + G.at<float>(y0, x1) * tx) * (1 - ty) + (G.at<float>(y1, x0) * (1 - tx) + G.at<float>(y1, x1) * tx) * ty;
                    // Where the reference itself is unusable keep the other exposures.
                    w *= refValid ? g : 1.0f;
                }
                wts[i] = w;
                wsum += w;
            }
            float* d = out.px(x, y);
            if (wsum <= 1e-9) {
                // Everything clipped (use the darkest) or everything black (use the brightest).
                const float* p0 = raws[0].image.px(x, y);
                int pick = std::max({p0[0], p0[1], p0[2]}) > 0.5f ? 0 : n - 1;
                for (int c = 0; c < 3; ++c)
                    d[c] = native(pick, x, y, c) * float(k[pick]);
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                double acc = 0;
                for (int i = 0; i < n; ++i)
                    if (wts[i] > 0)
                        acc += wts[i] * native(i, x, y, c) * k[i];
                d[c] = float(acc / wsum);
            }
        }
    }
    (void)rc;
    // Normalize so the brightest value is 1.0 and compensate with the baseline exposure.
    float mx = 1e-6f;
    for (float v : out.data)
        mx = std::max(mx, v);
    float scale = 1.0f;
    {
        // Native clip level for white (the channel with the largest multiplier clips first).
        double nativeClip = 1.0 / std::max({rc.appliedMul[0], rc.appliedMul[1], rc.appliedMul[2]});
        scale = float(std::max(1.0, mx / nativeClip));
    }
    HdrMerged m;
    m.info = dngInfoFromRaw(raws[ref], photos[ref].path, photos[ref].lens);
    m.info.whiteScale = scale;
    m.info.baselineExposure += std::log2(scale);
    m.out = std::move(out);
    m.sources = sources;
    m.ref = ref;
    return m;
}

} // namespace

qint64 createHdrMerge(Batch& b, const QList<qint64>& ids, const HdrOptions& o, const StepProgress& progress, const std::atomic<bool>* cancel)
{
    HdrMerged m = mergeHdr(mergeSources(b, ids), o, false, progress, cancel);
    report(progress, QStringLiteral("Writing DNG"));
    // Write first, then compute auto settings from the result.
    QList<qint64> sourceIds;
    for (const auto& src : m.sources)
        sourceIds << src.photo.id;
    qint64 id = addDerivedPhoto(b, m.sources[m.ref].photo, sourceIds, "HDR", m.out, m.info, QString(), DevelopSettings().toJsonString(), true);
    auto np = b.photo(id);
    DevelopSettings s = settingsWithAutoTone(np->path, o.autoTone);
    // The merge keeps the sensor's orientation: a 90 degree turn of the photos carries over.
    s.quarterTurns = m.sources[m.ref].quarterTurns;
    b.pushHistory(id, QStringLiteral("HDR Merge (%1 photos)").arg(m.sources.size()), s.toJsonString());
    return id;
}

struct HdrPreview::Impl {
    QTemporaryDir dir;
    std::unique_ptr<DevelopEngine> engine;
    int quarterTurns = 0;
};

HdrPreview::HdrPreview(const QList<MergeSource>& sources, const HdrOptions& o, const StepProgress& progress, const std::atomic<bool>* cancel)
    : d(std::make_unique<Impl>())
{
    HdrMerged m = mergeHdr(sources, o, true, progress, cancel);
    if (!d->dir.isValid())
        throw std::runtime_error("No temporary directory for the preview");
    // Through a DNG like the real result, so the preview renders the same way.
    const QString path = d->dir.filePath("preview.dng");
    writeLinearDng(path, m.out, m.info, true);
    d->engine = std::make_unique<DevelopEngine>(path);
    d->quarterTurns = m.sources[m.ref].quarterTurns;
}

HdrPreview::~HdrPreview() = default;

QImage HdrPreview::render(bool autoTone, int longEdge) const
{
    DevelopSettings s;
    s.quarterTurns = d->quarterTurns;
    if (autoTone)
        s = d->engine->autoTone(s);
    const QSize out = d->engine->outputSize(s);
    const double k = std::min(1.0, double(longEdge) / std::max(out.width(), out.height()));
    return d->engine->render(s, {.width = std::max(1, int(out.width() * k)), .height = std::max(1, int(out.height() * k))}).image;
}

// ---- Panorama -----------------------------------------------------------------------

QRect largestRectangle(const std::vector<uint8_t>& mask, int w, int h)
{
    std::vector<int> heights(w, 0);
    QRect best;
    long bestArea = 0;
    std::vector<int> stack;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x)
            heights[x] = mask[size_t(y) * w + x] ? heights[x] + 1 : 0;
        stack.clear();
        for (int x = 0; x <= w; ++x) {
            int hh = x < w ? heights[x] : 0;
            while (!stack.empty() && heights[stack.back()] >= hh) {
                int top = stack.back();
                stack.pop_back();
                int height = heights[top];
                int left = stack.empty() ? 0 : stack.back() + 1;
                long area = long(height) * (x - left);
                if (area > bestArea) {
                    bestArea = area;
                    best = QRect(left, y - height + 1, x - left, height);
                }
            }
            stack.push_back(x);
        }
    }
    return best;
}

namespace {

// A stitched panorama, before it becomes a DNG.
struct Stitched {
    ImageF pano; // camera-native linear (the reference photo's camera), not white-balanced
    cv::Mat valid; // coverage (8-bit, 255 = covered)
    QRectF crop{0, 0, 1, 1}; // largest covered rectangle (normalized), when asked for
    RawImage refRaw;
    QList<PhotoInfo> photos; // in capture order
    int ref = 0;
    double nativeClip = 1;
};

// Registers the photos at a small working size and stitches them, at full resolution or
// (`preview`) at the working size from fast half-size decodes.
Stitched stitchPanorama(QList<MergeSource> sources, bool preview, bool autoCrop, const StepProgress& progress, const std::atomic<bool>* cancel)
{
    namespace cd = cv::detail;
    if (sources.size() < 2)
        throw std::runtime_error("A panorama needs at least two photos");
    std::sort(sources.begin(), sources.end(), [](const MergeSource& a, const MergeSource& c) { return a.photo.captureTime < c.photo.captureTime; });
    QList<PhotoInfo> photos;
    for (const auto& src : sources)
        photos << src.photo;
    const int n = int(photos.size());
    const int ref = n / 2;

    // Lens-corrected full images (camera-native linear) and small previews for registration.
    std::vector<ImageF> full(n);
    std::vector<cv::Mat> work(n), seamImgs(n);
    std::vector<RawColorInfo> colors(n);
    RawImage refRaw;
    DevelopSettings plain; // lens corrections only
    plain.sharpenAmount = 0;
    double workScale = 1, seamScale = 1, outScale = 1;
    for (int i = 0; i < n; ++i) {
        report(progress, QStringLiteral("Preparing photos"), i, n);
        checkCancel(cancel);
        DevelopEngine e(photos[i].path, {.halfSize = preview});
        plain.quarterTurns = sources[i].quarterTurns; // e.g. turned by hand: wrong orientation tag
        QSizeF cs = e.geometry(plain, true).canvasSize();
        int W = int(std::round(cs.width())), H = int(std::round(cs.height()));
        workScale = std::min(1.0, std::sqrt(0.6e6 / (double(W) * H)));
        seamScale = std::min(1.0, std::sqrt(0.1e6 / (double(W) * H)));
        outScale = preview ? workScale : 1.0;
        ImageF cam = e.warpedCamera(plain, std::max(1, int(W * outScale)), std::max(1, int(H * outScale)), true);
        colors[i] = e.raw().color;
        for (size_t p = 0; p < cam.pixelCount(); ++p)
            for (int c = 0; c < 3; ++c)
                cam.data[p * 3 + c] /= float(colors[i].appliedMul[c]);
        full[i] = std::move(cam);
        // Registration previews use the normal rendering (default look).
        work[i] = to8bit(e.renderEncoded(plain, {.width = int(W * workScale), .height = int(H * workScale), .ignoreCrop = true}));
        cv::resize(work[i], seamImgs[i], cv::Size(), seamScale / workScale, seamScale / workScale, cv::INTER_AREA);
        if (i == ref)
            refRaw = e.raw();
    }

    report(progress, QStringLiteral("Finding features"));
    checkCancel(cancel);
    std::vector<cd::ImageFeatures> features(n);
    cv::Ptr<cv::Feature2D> sift = cv::SIFT::create(3000);
    for (int i = 0; i < n; ++i) {
        cd::computeImageFeatures(sift, work[i], features[i]);
        features[i].img_idx = i;
    }
    std::vector<cd::MatchesInfo> pairwise;
    cd::BestOf2NearestMatcher matcher(false, 0.3f);
    matcher(features, pairwise);
    matcher.collectGarbage();
    if (qEnvironmentVariableIsSet("SILMA_DEBUG_PANO")) {
        for (int i = 0; i < n; ++i)
            fprintf(stderr, "image %d: %zu keypoints, work %dx%d\n", i, features[i].keypoints.size(), work[i].cols, work[i].rows);
        for (const auto& m : pairwise)
            if (m.src_img_idx >= 0 && m.src_img_idx < m.dst_img_idx)
                fprintf(stderr, "pair %d-%d: %zu matches, %d inliers, confidence %.2f\n", m.src_img_idx, m.dst_img_idx, m.matches.size(), m.num_inliers, m.confidence);
    }
    std::vector<int> keep = cd::leaveBiggestComponent(features, pairwise, 1.0f);
    if (int(keep.size()) < n)
        throw std::runtime_error(QStringLiteral("Could only connect %1 of %2 photos; do they overlap?").arg(keep.size()).arg(n).toStdString());

    report(progress, QStringLiteral("Estimating camera positions"));
    std::vector<cd::CameraParams> cameras;
    cd::HomographyBasedEstimator estimator;
    if (!estimator(features, pairwise, cameras))
        throw std::runtime_error("Camera estimation failed");
    for (auto& c : cameras) {
        cv::Mat R;
        c.R.convertTo(R, CV_32F);
        c.R = R;
    }
    cd::BundleAdjusterRay adjuster;
    adjuster.setConfThresh(1.0);
    if (!adjuster(features, pairwise, cameras))
        throw std::runtime_error("Bundle adjustment failed");
    std::vector<double> focals;
    for (const auto& c : cameras)
        focals.push_back(c.focal);
    std::sort(focals.begin(), focals.end());
    double warpedScale = focals[focals.size() / 2];
    std::vector<cv::Mat> rmats;
    for (const auto& c : cameras)
        rmats.push_back(c.R.clone());
    cd::waveCorrect(rmats, cd::WAVE_CORRECT_HORIZ);
    for (int i = 0; i < n; ++i)
        cameras[i].R = rmats[i];

    // Seams at low resolution.
    report(progress, QStringLiteral("Finding seams"));
    checkCancel(cancel);
    double seamAspect = seamScale / workScale;
    std::vector<cv::Point> seamCorners(n);
    std::vector<cv::UMat> seamWarped(n), seamMasks(n);
    {
        cd::CylindricalWarper warper(float(warpedScale * seamAspect));
        for (int i = 0; i < n; ++i) {
            cv::Mat K;
            cameras[i].K().convertTo(K, CV_32F);
            K.at<float>(0, 0) *= float(seamAspect);
            K.at<float>(0, 2) *= float(seamAspect);
            K.at<float>(1, 1) *= float(seamAspect);
            K.at<float>(1, 2) *= float(seamAspect);
            cv::Mat warped, mask(seamImgs[i].size(), CV_8U, cv::Scalar(255)), mwarped;
            seamCorners[i] = warper.warp(seamImgs[i], K, cameras[i].R, cv::INTER_LINEAR, cv::BORDER_REFLECT, warped);
            warper.warp(mask, K, cameras[i].R, cv::INTER_NEAREST, cv::BORDER_CONSTANT, mwarped);
            warped.convertTo(seamWarped[i], CV_32F);
            mwarped.copyTo(seamMasks[i]);
        }
        cd::GraphCutSeamFinder seams(cd::GraphCutSeamFinderBase::COST_COLOR);
        seams.find(seamWarped, seamCorners, seamMasks);
    }

    // Full-resolution (or preview-sized) warp.
    double fullAspect = outScale / workScale;
    cd::CylindricalWarper warper(float(warpedScale * fullAspect));
    std::vector<cv::Mat> warped(n), masks(n);
    std::vector<cv::Point> corners(n);
    for (int i = 0; i < n; ++i) {
        report(progress, QStringLiteral("Warping"), i, n);
        checkCancel(cancel);
        cv::Mat K;
        cameras[i].K().convertTo(K, CV_32F);
        K.at<float>(0, 0) *= float(fullAspect);
        K.at<float>(0, 2) *= float(fullAspect);
        K.at<float>(1, 1) *= float(fullAspect);
        K.at<float>(1, 2) *= float(fullAspect);
        cv::Mat src = toMat(full[i]);
        corners[i] = warper.warp(src, K, cameras[i].R, cv::INTER_LINEAR, cv::BORDER_CONSTANT, warped[i]);
        cv::Mat mask(src.size(), CV_8U, cv::Scalar(255));
        warper.warp(mask, K, cameras[i].R, cv::INTER_NEAREST, cv::BORDER_CONSTANT, masks[i]);
        full[i] = ImageF(); // free memory
    }

    // Gain compensation from overlaps (least squares, as in Brown & Lowe), in linear light.
    report(progress, QStringLiteral("Matching exposure"));
    std::vector<double> gains(n, 1.0);
    {
        cv::Mat A = cv::Mat::zeros(n, n, CV_64F), bb = cv::Mat::zeros(n, 1, CV_64F);
        const double sigmaN = 10.0 / 255, sigmaG = 0.1;
        std::vector<cv::Rect> rects(n);
        for (int i = 0; i < n; ++i)
            rects[i] = cv::Rect(corners[i], warped[i].size());
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                if (i == j)
                    continue;
                cv::Rect ov = rects[i] & rects[j];
                double si = 0, sj = 0;
                long cnt = 0;
                for (int y = ov.y; y < ov.y + ov.height; y += 4)
                    for (int x = ov.x; x < ov.x + ov.width; x += 4) {
                        int xi = x - corners[i].x, yi = y - corners[i].y, xj = x - corners[j].x, yj = y - corners[j].y;
                        if (!masks[i].at<uchar>(yi, xi) || !masks[j].at<uchar>(yj, xj))
                            continue;
                        // Encoded (sqrt) luminance keeps the problem well conditioned.
                        si += std::sqrt(std::max(0.0f, warped[i].at<cv::Vec3f>(yi, xi)[1]));
                        sj += std::sqrt(std::max(0.0f, warped[j].at<cv::Vec3f>(yj, xj)[1]));
                        ++cnt;
                    }
                if (cnt < 50)
                    continue;
                double Ii = si / cnt, Ij = sj / cnt;
                A.at<double>(i, i) += cnt * (2 * Ii * Ii / (sigmaN * sigmaN) + 1 / (sigmaG * sigmaG));
                A.at<double>(i, j) -= cnt * 2 * Ii * Ij / (sigmaN * sigmaN);
                bb.at<double>(i) += cnt / (sigmaG * sigmaG);
            }
        cv::Mat g;
        if (cv::solve(A, bb, g, cv::DECOMP_SVD))
            for (int i = 0; i < n; ++i)
                if (g.at<double>(i) > 0.2 && g.at<double>(i) < 5)
                    gains[i] = g.at<double>(i) * g.at<double>(i); // sqrt domain -> linear
        double gref = gains[ref];
        for (auto& g2 : gains)
            g2 /= gref;
    }

    // Feathered blend along the seams.
    report(progress, QStringLiteral("Blending"));
    checkCancel(cancel);
    cv::Rect dst = cv::Rect(corners[0], warped[0].size());
    for (int i = 1; i < n; ++i)
        dst |= cv::Rect(corners[i], warped[i].size());
    ImageF pano(dst.width, dst.height);
    cv::Mat panoM = toMat(pano);
    cv::Mat wsum = cv::Mat::zeros(dst.size(), CV_32F);
    const double featherSigma = std::max(4.0, 0.006 * std::max(dst.width, dst.height));
    for (int i = 0; i < n; ++i) {
        checkCancel(cancel);
        cv::Mat seam, seamUp;
        seamMasks[i].copyTo(seam);
        // Upscale the seam mask to full resolution, restricted to the valid area.
        cv::resize(seam, seamUp, warped[i].size(), 0, 0, cv::INTER_LINEAR);
        cv::Mat w;
        seamUp.convertTo(w, CV_32F, 1.0 / 255);
        cv::GaussianBlur(w, w, cv::Size(), featherSigma);
        cv::Mat valid;
        masks[i].convertTo(valid, CV_32F, 1.0 / 255);
        // Fade out towards the image's own border too.
        cv::Mat dist;
        cv::distanceTransform(masks[i], dist, cv::DIST_L2, 3);
        cv::Mat edge = cv::min(dist / float(featherSigma), 1.0);
        w = w.mul(valid).mul(edge) + 1e-6f * valid;
        cv::Rect r(corners[i] - dst.tl(), warped[i].size());
        cv::Mat img = warped[i] * gains[i];
        std::vector<cv::Mat> ch(3);
        cv::split(img, ch);
        for (auto& c : ch)
            c = c.mul(w);
        cv::Mat weighted;
        cv::merge(ch, weighted);
        panoM(r) += weighted;
        wsum(r) += w;
        warped[i].release();
    }
    cv::Mat valid = wsum > 1e-5;
    for (int y = 0; y < pano.height; ++y)
        for (int x = 0; x < pano.width; ++x) {
            float ws = wsum.at<float>(y, x);
            float* p = pano.px(x, y);
            if (ws > 1e-5f)
                for (int c = 0; c < 3; ++c)
                    p[c] /= ws;
        }

    // Constrained crop (as a crop setting, the DNG keeps everything).
    Stitched out;
    if (autoCrop) {
        const int f = std::max(1, std::max(pano.width, pano.height) / 1500);
        cv::Mat small;
        cv::resize(valid, small, cv::Size(pano.width / f, pano.height / f), 0, 0, cv::INTER_AREA);
        std::vector<uint8_t> m(size_t(small.cols) * small.rows);
        for (int y = 0; y < small.rows; ++y)
            for (int x = 0; x < small.cols; ++x)
                m[size_t(y) * small.cols + x] = small.at<uchar>(y, x) > 250;
        QRect r = largestRectangle(m, small.cols, small.rows);
        if (r.width() > 10 && r.height() > 10)
            out.crop = QRectF(double(r.x() + 1) / small.cols, double(r.y() + 1) / small.rows, double(r.width() - 2) / small.cols, double(r.height() - 2) / small.rows);
    }
    out.nativeClip = 1.0 / std::max({colors[ref].appliedMul[0], colors[ref].appliedMul[1], colors[ref].appliedMul[2]});
    out.pano = std::move(pano);
    out.valid = valid;
    out.refRaw = std::move(refRaw);
    out.photos = photos;
    out.ref = ref;
    return out;
}

// DNG description of a stitched panorama.
LinearDngInfo panoramaDngInfo(const Stitched& st)
{
    float mx = 1e-6f;
    for (float v : st.pano.data)
        mx = std::max(mx, v);
    LinearDngInfo info = dngInfoFromRaw(st.refRaw, st.photos[st.ref].path, QString());
    info.orientation = 1;
    info.lensCorrected = true;
    info.whiteScale = float(std::max(1.0, mx / st.nativeClip));
    info.baselineExposure += std::log2(info.whiteScale);
    // Coverage, so auto crop can find the stitched area later (e.g. with --no-crop).
    info.transparencyMask.assign(st.valid.datastart, st.valid.dataend);
    return info;
}

} // namespace


qint64 createPanorama(Batch& b, const QList<qint64>& ids, const PanoOptions& o, const StepProgress& progress, const std::atomic<bool>* cancel)
{
    const Stitched st = stitchPanorama(mergeSources(b, ids), false, o.autoCrop, progress, cancel);
    report(progress, QStringLiteral("Writing DNG"));
    const LinearDngInfo info = panoramaDngInfo(st);
    QList<qint64> sourceIds;
    for (const auto& p : st.photos)
        sourceIds << p.id;
    PhotoInfo base = st.photos[st.ref];
    base.orientation = 1;
    qint64 id = addDerivedPhoto(b, base, sourceIds, "Pano", st.pano, info, QString(), DevelopSettings().toJsonString(), true);
    auto np = b.photo(id);
    DevelopSettings s = settingsWithAutoTone(np->path, o.autoTone);
    s.crop = st.crop;
    b.pushHistory(id, QStringLiteral("Panorama (%1 photos)").arg(st.photos.size()), s.toJsonString());
    return id;
}

struct PanoramaPreview::Impl {
    QTemporaryDir dir;
    std::unique_ptr<DevelopEngine> engine;
    QRectF crop;
    int photoCount = 0;
};

PanoramaPreview::PanoramaPreview(const QList<MergeSource>& sources, const StepProgress& progress, const std::atomic<bool>* cancel)
    : d(std::make_unique<Impl>())
{
    Stitched st = stitchPanorama(sources, true, true, progress, cancel);
    if (!d->dir.isValid())
        throw std::runtime_error("No temporary directory for the preview");
    // Through a DNG like the real result, so the preview renders the same way.
    const QString path = d->dir.filePath("preview.dng");
    writeLinearDng(path, st.pano, panoramaDngInfo(st), true);
    d->engine = std::make_unique<DevelopEngine>(path);
    d->crop = st.crop;
    d->photoCount = int(st.photos.size());
}

PanoramaPreview::~PanoramaPreview() = default;

QRectF PanoramaPreview::autoCrop() const { return d->crop; }

QSize PanoramaPreview::size() const
{
    const QSizeF s = d->engine->geometry(DevelopSettings(), true).canvasSize();
    return QSize(int(s.width()), int(s.height()));
}

QImage PanoramaPreview::render(bool autoTone, int longEdge) const
{
    DevelopSettings s;
    if (autoTone)
        s = d->engine->autoTone(s);
    const QSize full = size();
    const double k = std::min(1.0, double(longEdge) / std::max(full.width(), full.height()));
    return d->engine->render(s, {.width = std::max(1, int(full.width() * k)), .height = std::max(1, int(full.height() * k)), .ignoreCrop = true}).image;
}

} // namespace silma
