#include "Color.h"
#include "Engine.h"
#include "Export.h"
#include "AiDenoise.h"
#include "Heal.h"
#include "LensCorrection.h"
#include "Metadata.h"
#include "RawDecoder.h"
#include "Upright.h"
#include "Geometry.h"
#include "TestRaws.h"

#include <QDir>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtConcurrent>
#include <QtTest>
#include <numeric>

using namespace silma;

class TestEngine : public QObject {
    Q_OBJECT

private slots:
    void tempTintRoundTrip()
    {
        for (double t : {2500.0, 4000.0, 5500.0, 6500.0, 9000.0})
            for (double tint : {-40.0, 0.0, 30.0}) {
                double t2, tint2;
                xyToTempTint(tempTintToXy(t, tint), t2, tint2);
                QVERIFY2(std::abs(t2 - t) / t < 0.002, qPrintable(QString("%1 -> %2").arg(t).arg(t2)));
                QVERIFY(std::abs(tint2 - tint) < 0.5);
            }
    }

    void pasteParts()
    {
        DevelopSettings src, dst;
        src.exposure = 1.5;
        src.contrast = 20;
        src.wbMode = WbMode::Custom;
        src.temperature = 4000;
        src.crop = QRectF(0.1, 0.1, 0.5, 0.5);
        src.sharpenAmount = 90;
        src.heals = {HealSpot{0.5, 0.5, 0.02, 7}};
        dst.contrast = -30;
        dst.heals = {HealSpot{0.2, 0.2, 0.02, 3}};
        DevelopSettings r = copySettingsParts(dst, src, {"exposure", "wb", "sharpen_amount"});
        QCOMPARE(r.exposure, 1.5);
        QCOMPARE(r.contrast, -30.0); // not chosen: the target's value stays
        QCOMPARE(r.wbMode, WbMode::Custom);
        QCOMPARE(r.temperature, 4000.0);
        QCOMPARE(r.crop, QRectF(0, 0, 1, 1));
        QCOMPARE(r.sharpenAmount, 90.0);
        QCOMPARE(r.heals, dst.heals); // heal spots are never pasted
        // Every part key is handled (scalar keys must name a param).
        for (const SettingsPart& p : settingsParts()) {
            const QString k = QString::fromLatin1(p.key);
            QVERIFY2(k == "wb" || k == "profile" || k == "curve" || k == "crop" || findParam(k), p.key);
        }
    }

    void whiteBalancePicker()
    {
        const QString f = testRaw("batch/DSC_0001.NEF");
        if (f.isEmpty())
            QSKIP("test raws not available");
        DevelopEngine e(f, {.halfSize = true});
        DevelopSettings s;
        const QSize out(1800, 1200); // a 3x3 block covers about the picker's sample patch
        int checked = 0;
        for (double v = 0.2; v < 0.85; v += 0.15)
            for (double u = 0.2; u < 0.85; u += 0.15) {
                auto wb = e.wbFromPoint(s, {u, v});
                if (!wb)
                    continue;
                // The result does not depend on the white balance currently set.
                DevelopSettings warm = s;
                warm.wbMode = WbMode::Custom;
                warm.temperature = 3000;
                auto wb2 = e.wbFromPoint(warm, {u, v});
                QVERIFY(wb2 && std::abs(wb2->temperature - wb->temperature) < 1 && std::abs(wb2->tint - wb->tint) < 0.1);
                if (wb->temperature <= 2000 || wb->temperature >= 50000 || std::abs(wb->tint) >= 150)
                    continue; // clamped: cannot be fully neutral
                // Rendered with the picked white balance, the point comes out grey.
                DevelopSettings picked = s;
                picked.wbMode = WbMode::Custom;
                picked.temperature = wb->temperature;
                picked.tint = wb->tint;
                // Picking the now-grey point again changes nothing.
                auto again = e.wbFromPoint(picked, {u, v});
                QVERIFY(again && std::abs(again->temperature - wb->temperature) < 1 && std::abs(again->tint - wb->tint) < 0.1);
                // Rendered with the picked white balance, the point comes out grey (judged on
                // flat areas only: on texture the sampled patch and output pixels differ).
                QImage img = e.render(picked, {.width = out.width(), .height = out.height()}).image;
                double avg[3] = {0, 0, 0}, lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        QColor q = img.pixelColor(int(u * img.width()) + dx, int(v * img.height()) + dy);
                        const int ch[3] = {q.red(), q.green(), q.blue()};
                        for (int c = 0; c < 3; ++c) {
                            avg[c] += ch[c] / 9.0;
                            lo[c] = std::min(lo[c], double(ch[c]));
                            hi[c] = std::max(hi[c], double(ch[c]));
                        }
                    }
                const double texture = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
                if (texture > 12 || avg[1] < 40 || avg[1] > 230)
                    continue; // textured, or where the tone curve compresses colour differences
                const double spread = std::max({avg[0], avg[1], avg[2]}) - std::min({avg[0], avg[1], avg[2]});
                QVERIFY2(spread <= 6, qPrintable(QStringLiteral("at %1,%2: %3 %4 %5").arg(u).arg(v).arg(avg[0]).arg(avg[1]).arg(avg[2])));
                ++checked;
            }
        QVERIFY2(checked >= 3, qPrintable(QString::number(checked)));
    }

    void healStrokes()
    {
        // Stroke heals survive the settings JSON; round spots stay in the old form.
        DevelopSettings s;
        s.heals = {HealSpot{0.1, 0.2, 0.01, 5, {}}, HealSpot{0.3, 0.3, 0.02, 6, {QPointF(0.35, 0.3), QPointF(0.4, 0.32)}}};
        QCOMPARE(DevelopSettings::fromJsonString(s.toJsonString()).heals, s.heals);
        QCOMPARE(s.toJson()["heal"].toArray()[0].toArray().size(), 4);

        // Pasting follows the stroke: fully replaced on it, untouched away from it.
        const int W = 400, H = 200;
        ImageF img(W, H); // zeros
        HealSpot st{0.25, 0.5, 0.02, 1, {QPointF(0.75, 0.5)}}; // horizontal, radius 8 px
        HealPatch patch;
        patch.rect = QRect(0, 0, W, H);
        patch.data = ImageF(W, H);
        std::fill(patch.data.data.begin(), patch.data.data.end(), 0.5f);
        pasteHealPatch(img, 1.0, W, H, st, patch);
        QCOMPARE(img.px(200, 100)[1], 0.5f); // on the line
        QCOMPARE(img.px(100, 104)[1], 0.5f); // within the brush
        QCOMPARE(img.px(200, 130)[1], 0.0f); // beyond brush and feather
        QCOMPARE(img.px(60, 100)[1], 0.0f);  // past the stroke's end (x = 100 - 8 - feather)
        QVERIFY(img.px(200, 109)[1] > 0 && img.px(200, 109)[1] < 0.5f); // feathered edge

        // Extent includes the brush; the cap is a fifth of the long edge.
        QVERIFY(std::abs(healExtent(st, W, H) - (200 + 16) / 400.0) < 1e-9);
        QVERIFY(healExtent(HealSpot{0.5, 0.5, 0.02, 0, {}}, W, H) < kMaxHealExtent);
    }

    void renderedRoundTrip()
    {
        // A JPEG developed with its default settings comes out as it went in.
        QTemporaryDir dir;
        QImage src(360, 240, QImage::Format_RGB888);
        for (int y = 0; y < src.height(); ++y)
            for (int x = 0; x < src.width(); ++x) {
                const int band = y / 60; // grey ramp, hue sweeps, saturated patches
                QColor c = band == 0 ? QColor(x * 255 / 359, x * 255 / 359, x * 255 / 359)
                         : band == 1 ? QColor::fromHsv(x, 120, 200)
                         : band == 2 ? QColor::fromHsv(x, 255, 40 + x / 2)
                                     : QColor::fromHsv((x / 40) * 40, 230, 230);
                src.setPixelColor(x, y, c);
            }
        const QString path = dir.filePath("test.jpg");
        QVERIFY(src.save(path, "JPEG", 100));
        const QImage ref = QImage(path).convertToFormat(QImage::Format_RGB888);
        DevelopEngine e(path);
        const ImageF out = e.renderEncoded(DevelopSettings::defaults(true), {.width = src.width(), .height = src.height(), .highQuality = true});
        double sum = 0, worst = 0;
        for (int y = 0; y < ref.height(); ++y)
            for (int x = 0; x < ref.width(); ++x)
                for (int c = 0; c < 3; ++c) {
                    const double d = std::abs(out.px(x, y)[c] * 255.0 - ref.constScanLine(y)[x * 3 + c]);
                    sum += d;
                    worst = std::max(worst, d);
                }
        const double mean = sum / (3.0 * ref.width() * ref.height());
        qDebug() << "mean" << mean << "worst" << worst;
        QVERIFY2(mean < 0.6 && worst < 4, qPrintable(QStringLiteral("mean %1, worst %2").arg(mean).arg(worst)));
        // Controls still work: more exposure is brighter.
        DevelopSettings s = DevelopSettings::defaults(true);
        s.exposure = 1;
        const ImageF brighter = e.renderEncoded(s, {.width = 36, .height = 24});
        const ImageF normal = e.renderEncoded(DevelopSettings::defaults(true), {.width = 36, .height = 24});
        QVERIFY(brighter.px(18, 3)[1] > normal.px(18, 3)[1] + 0.05f);
    }

    void hotPixels()
    {
        // Synthetic RGGB mosaic with noise-free gradients: a hot red and a stuck green sample go,
        // a bright 3x3 spot (real detail reaches the neighbours) stays.
        const int W = 40, H = 30;
        std::vector<quint16> d(size_t(W) * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                d[size_t(y) * W + x] = quint16(600 + 10 * x + 5 * y);
        const int color[2][2] = {{0, 1}, {1, 2}};
        const float black[2][2] = {{512, 512}, {512, 512}};
        const std::array<double, 3> mul{2.0, 1.0, 1.5};
        const quint16 before = d[10 * W + 10];
        d[10 * W + 10] = 4000;          // red (even row, even column)
        d[20 * W + 21] = 16383;         // stuck green
        for (int y = 14; y <= 16; ++y)  // spot
            for (int x = 30; x <= 32; ++x)
                d[size_t(y) * W + x] = 9000;
        QCOMPARE(removeHotPixels(d.data(), W, H, W, color, black, 16383, mul), 2);
        QVERIFY(std::abs(int(d[10 * W + 10]) - int(before)) < 40);
        QVERIFY(d[20 * W + 21] < 2000);
        QCOMPARE(d[15 * W + 31], quint16(9000));

        // The stuck red pixel of the denoise test photo (if available); a daylight photo has
        // almost none.
        const QString nef = testRaw("denoise/DSC_0036.NEF");
        if (!nef.isEmpty()) {
            const BayerMosaic m = decodeBayerMosaic(nef);
            QVERIFY(m.data[size_t(3936) * m.width + 2412] < 0.2f); // was at white level
            QVERIFY(decodeRaw(nef).hotPixels > 20);
        }
        const QString day = testRaw("drone/DJI_0488.DNG");
        if (!day.isEmpty())
            QVERIFY(decodeRaw(day).hotPixels <= 2);
    }

    void bayerMosaic()
    {
        // The mosaic matches LibRaw's demosaiced image per colour (without white balance).
        for (const char* name : {"batch/DSC_0001.NEF", "drone/DJI_0488.DNG"}) {
            const QString f = testRaw(name);
            if (f.isEmpty())
                QSKIP("test raws not available");
            const BayerMosaic m = decodeBayerMosaic(f);
            const RawImage raw = decodeRaw(f);
            QCOMPARE(m.width, raw.image.width);
            QCOMPARE(m.height, raw.image.height);
            double sum[3] = {}, ref[3] = {};
            long n[3] = {};
            for (int y = 0; y < m.height; y += 3)
                for (int x = 0; x < m.width; x += 5) {
                    const int c = m.color[y % 2][x % 2];
                    const float v = raw.image.px(x, y)[c];
                    if (v > 0.9f)
                        continue; // clipped by LibRaw
                    sum[c] += m.data[size_t(y) * m.width + x];
                    ref[c] += v / raw.color.appliedMul[c];
                    ++n[c];
                }
            for (int c = 0; c < 3; ++c) {
                QVERIFY(n[c] > 1000);
                QVERIFY(std::abs(sum[c] / ref[c] - 1) < 0.02);
            }
        }
    }

    void rawAiDenoise()
    {
        // Needs the downloaded model (run with SILMA_MODEL_DIR pointing at installed models).
        if (!aiModelInstalled("rawnind"))
            QSKIP("RawNIND model not installed");
        const QString f = testRaw("drone/DJI_0488.DNG");
        if (f.isEmpty())
            QSKIP("test raws not available");
        const RawImage raw = decodeRaw(f);
        // Fine-scale variation (difference to a 3x3 box mean) and mean of a channel.
        auto stats = [&](const ImageF& img, int c, double& mean, double& detail) {
            double s = 0, d = 0;
            long n = 0;
            for (int y = 1; y < img.height - 1; y += 4)
                for (int x = 1; x < img.width - 1; x += 4) {
                    double box = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            box += img.px(x + dx, y + dy)[c];
                    const double v = img.px(x, y)[c];
                    s += v;
                    d += std::abs(v - box / 9);
                    ++n;
                }
            mean = s / n;
            detail = d / n;
        };
        auto check = [&](const ImageF& den) {
            QCOMPARE(den.width, raw.image.width);
            QCOMPARE(den.height, raw.image.height);
            for (int c = 0; c < 3; ++c) {
                double m0, d0, m1, d1;
                stats(raw.image, c, m0, d0);
                stats(den, c, m1, d1);
                // Same brightness and colour, much less pixel-level noise.
                QVERIFY2(std::abs(m1 / m0 - 1) < 0.03, qPrintable(QStringLiteral("channel %1 mean %2 -> %3").arg(c).arg(m0).arg(m1)));
                QVERIFY2(d1 < 0.7 * d0, qPrintable(QStringLiteral("channel %1 detail %2 -> %3").arg(c).arg(d0).arg(d1)));
            }
        };
        int steps = 0;
        const ImageF full = aiDenoiseRaw("rawnind", f, raw, 1.0, [&](int, int) { ++steps; });
        check(full);
        QVERIFY(steps > 1);
        // A region (the preview) comes out like the same area of the whole photo, apart from
        // its edges; odd offsets on purpose (the mosaic pattern).
        {
            AiDenoiser denoiser("rawnind", f, raw);
            const QRect r(1001, 777, 501, 403);
            const ImageF part = denoiser.denoise(r);
            QCOMPARE(part.width, r.width());
            QCOMPARE(part.height, r.height());
            double diff = 0, sum = 0;
            for (int y = 48; y < r.height() - 48; ++y)
                for (int x = 48; x < r.width() - 48; ++x)
                    for (int c = 0; c < 3; ++c) {
                        diff += std::abs(part.px(x, y)[c] - full.px(r.x() + x, r.y() + y)[c]);
                        sum += full.px(r.x() + x, r.y() + y)[c];
                    }
            QVERIFY2(diff / sum < 0.01, qPrintable(QStringLiteral("relative difference %1").arg(diff / sum)));
            ImageF blended = part;
            blendDenoised(blended, raw.image, r, 0.0);
            QCOMPARE(blended.px(10, 10)[1], raw.image.px(r.x() + 10, r.y() + 10)[1]);
        }
        // Strength 0 is the input unchanged.
        const ImageF none = aiDenoiseRaw("rawnind", f, raw, 0.0);
        QVERIFY(none.data == raw.image.data);
        // The variant for other sensors (on the demosaiced image).
        qputenv("SILMA_AI_FORCE_LINEAR", "1");
        check(aiDenoiseRaw("rawnind", f, raw, 1.0));
        qunsetenv("SILMA_AI_FORCE_LINEAR");
    }

    void lanczosAndOutputSharpening()
    {
        // A flat image stays flat; a step edge stays sharp (area averaging blurs it more).
        ImageF flat(300, 200);
        std::fill(flat.data.begin(), flat.data.end(), 0.4f);
        const ImageF small = resizeLanczos(flat, 100, 67);
        QCOMPARE(small.width, 100);
        QCOMPARE(small.height, 67);
        for (float v : small.data)
            QVERIFY(std::abs(v - 0.4f) < 1e-4f);
        ImageF step(300, 30);
        for (int y = 0; y < step.height; ++y)
            for (int x = 0; x < step.width; ++x)
                std::fill_n(step.px(x, y), 3, x < 151 ? 0.2f : 0.8f);
        auto slope = [](const ImageF& img) { // steepest neighbour difference along the middle row
            float m = 0;
            for (int x = 1; x < img.width; ++x)
                m = std::max(m, img.px(x, img.height / 2)[0] - img.px(x - 1, img.height / 2)[0]);
            return m;
        };
        QVERIFY(slope(resizeLanczos(step, 100, 10)) > slope(resize(step, 100, 10)) + 0.02f);

        // Output sharpening: none changes nothing; more sharpening raises local contrast in steps.
        ImageF tex(64, 64);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                std::fill_n(tex.px(x, y), 3, 0.5f + 0.1f * float(((x / 2) + (y / 3)) % 2));
        auto contrast = [](const ImageF& img) {
            double s = 0;
            for (int y = 8; y < 56; ++y)
                for (int x = 9; x < 56; ++x)
                    s += std::abs(img.px(x, y)[1] - img.px(x - 1, y)[1]);
            return s;
        };
        double last = contrast(tex);
        ImageF none = tex;
        outputSharpen(none, OutputSharpening::None);
        QVERIFY(none.data == tex.data);
        for (OutputSharpening l : {OutputSharpening::Low, OutputSharpening::Standard, OutputSharpening::High}) {
            ImageF t = tex;
            outputSharpen(t, l);
            const double c = contrast(t);
            QVERIFY(c > last * 1.05);
            last = c;
        }
    }

    void aiModels()
    {
        // RawNIND is the denoise model (NAFNet and SCUNet were dropped: they wiped out fine texture).
        QCOMPARE(defaultAiDenoiseModel(), QStringLiteral("rawnind"));
        QCOMPARE(aiDenoiseModels().size(), 1);
        const AiModelInfo* m = findAiModel("rawnind");
        QVERIFY(m && !m->archive.isEmpty() && !m->linearOnnxFile.isEmpty());
        QVERIFY(!findAiModel("nafnet-sidd-w64") && !findAiModel("scunet-real-psnr"));
        QVERIFY(findAiModel(aiInpaintModel().id));
    }

    void settingsJsonRoundTrip()
    {
        DevelopSettings s;
        s.exposure = 1.25;
        s.wbMode = WbMode::Custom;
        s.temperature = 4300;
        s.curveRgb.points = {{0, 0}, {0.5, 0.6}, {1, 1}};
        s.crop = QRectF(0.1, 0.1, 0.8, 0.7);
        DevelopSettings r = DevelopSettings::fromJsonString(s.toJsonString());
        QVERIFY(r == s);
        // Defaults are not stored.
        QCOMPARE(DevelopSettings().toJson().size(), 1);
        QVERIFY(!applyAssignment(r, "exposure=9").isEmpty());
        QVERIFY(applyAssignment(r, "shadows=40").isEmpty());
        QCOMPARE(r.shadows, 40.0);
        QVERIFY(applyAssignment(r, "dehaze=35").isEmpty());
        QVERIFY(applyAssignment(r, "texture=-20").isEmpty());
        DevelopSettings r2 = DevelopSettings::fromJsonString(r.toJsonString());
        QCOMPARE(r2.dehaze, 35.0);
        QCOMPARE(r2.texture, -20.0);
        // Relative changes, clamped to the range.
        QVERIFY(applyAssignment(r, "shadows+=15").isEmpty());
        QCOMPARE(r.shadows, 55.0);
        QVERIFY(applyAssignment(r, "texture -= 90").isEmpty());
        QCOMPARE(r.texture, -100.0);
        QVERIFY(applyAssignment(r, "exposure+=0.3").isEmpty());
        QVERIFY(applyAssignment(r, "temperature-=300").isEmpty());
        QCOMPARE(r.temperature, 4000.0);
        QVERIFY(r.wbMode == WbMode::Custom);
        QVERIFY(!applyAssignment(r, "crop+=0.1").isEmpty());
        QVERIFY(!applyAssignment(r, "shadows+=x").isEmpty());
    }

    void renderSamples()
    {
        for (const char* name : {"other-day/DSC_0001.NEF", "drone/DJI_0488.DNG"}) {
            QString f = testRaw(name);
            if (f.isEmpty())
                QSKIP("test raws not available");
            DevelopEngine e(f, {.halfSize = true});
            WbValues as = e.asShotWb();
            // Multipliers -> temperature -> multipliers is consistent.
            WbValues back = e.wbFromTempTint(as.temperature, as.tint);
            for (int c = 0; c < 3; ++c)
                QVERIFY(std::abs(back.mul[c] - as.mul[c]) / as.mul[c] < 0.01);
            QVERIFY(as.temperature > 3000 && as.temperature < 9000);
            DevelopSettings s;
            auto r = e.render(s, {.width = 300, .height = 200});
            QCOMPARE(r.image.width(), 300);
            quint64 total = 0, sum = 0;
            for (int v = 0; v < 256; ++v) {
                total += r.histogram[1][v];
                sum += quint64(v) * r.histogram[1][v];
            }
            QCOMPARE(total, quint64(300 * 200));
            double mean = double(sum) / total;
            QVERIFY2(mean > 40 && mean < 200, qPrintable(QString::number(mean)));
            // Exposure +1 brightens.
            s.exposure = 1;
            auto r2 = e.render(s, {.width = 300, .height = 200});
            quint64 sum2 = 0;
            for (int v = 0; v < 256; ++v)
                sum2 += quint64(v) * r2.histogram[1][v];
            QVERIFY(sum2 > sum);
            // ROI render of the full crop equals full render dimensions-wise.
            auto r3 = e.render(s, {.width = 100, .height = 100, .roi = QRectF(0.4, 0.4, 0.1, 0.15)});
            QCOMPARE(r3.image.height(), 100);
            QVERIFY(!e.lensDescription().isEmpty());
        }
    }

    void quarterTurns()
    {
        // Orientation composition: four turns are the identity, and a turn of an unrotated
        // photo is EXIF 6 (90 CW).
        QCOMPARE(turnedOrientation(1, 1), 6);
        QCOMPARE(turnedOrientation(1, -1), 8);
        QCOMPARE(turnedOrientation(1, 2), 3);
        for (int o = 1; o <= 8; ++o) {
            QCOMPARE(turnedOrientation(o, 4), o);
            QCOMPARE(turnedOrientation(turnedOrientation(o, 1), -1), o);
        }
        // A turned photo maps each point of its crop to the same sensor position as the
        // unturned crop at the turned-back point: crop and perspective follow the turn.
        GeometrySource src;
        src.sensorWidth = 6000;
        src.sensorHeight = 4000;
        src.focalLength35 = 28;
        DevelopSettings s;
        s.crop = QRectF(0.1, 0.2, 0.5, 0.6);
        s.rotate = 2;
        for (int o : {1, 6, 8, 3})
            for (auto [v, h] : {std::pair{30.0, 0.0}, std::pair{0.0, -20.0}}) {
                src.orientation = o;
                s.perspVertical = v;
                s.perspHorizontal = h;
                const Geometry g0(src, s);
                for (int turns : {1, -1, 2}) {
                    const DevelopSettings t = turnedSettings(s, turns);
                    const Geometry g1(src, t);
                    QCOMPARE(turns == 2 ? g1.canvasSize() : g1.canvasSize().transposed(), g0.canvasSize());
                    for (double u : {0.1, 0.5, 0.9})
                        for (double w : {0.2, 0.7}) {
                            // (u, w) in the turned crop -> the point before turning.
                            const QPointF p = turns == 1 ? QPointF(w, 1 - u) : turns == -1 ? QPointF(1 - w, u) : QPointF(1 - u, 1 - w);
                            float a[6], b[6], ga[3], gb[3];
                            g1.mapCrop(u, w, a, ga);
                            g0.mapCrop(p.x(), p.y(), b, gb);
                            QVERIFY2(std::abs(a[2] - b[2]) < 1e-5 && std::abs(a[3] - b[3]) < 1e-5,
                                     qPrintable(QStringLiteral("o %1 v %2 h %3 turns %4: %5,%6 vs %7,%8").arg(o).arg(v).arg(h).arg(turns).arg(a[2]).arg(a[3]).arg(b[2]).arg(b[3])));
                        }
                }
            }
        // Settings round trip and the CLI assignment.
        s.quarterTurns = 3;
        QCOMPARE(DevelopSettings::fromJsonString(s.toJsonString()).quarterTurns, 3);
        QVERIFY(applyAssignment(s, "quarter_turns=-1").isEmpty());
        QCOMPARE(s.quarterTurns, 3);
    }

    void uprightVertical()
    {
        // Walls with many verticals: the hand-tuned correction is vertical 14, rotate -0.9
        // (a fixed penalty on the correction used to stop at 10).
        if (testRaws("upright").size() < 3)
            QSKIP("test raws not available");
        auto upright = [&](const char* name) {
            DevelopEngine e(testRaw(QStringLiteral("upright/") + name), {.halfSize = true});
            return autoUpright(e, DevelopSettings{}, UprightMode::Vertical);
        };
        UprightResult r = upright("DSC_0160.NEF");
        QVERIFY(r.ok);
        QVERIFY2(r.vertical >= 13 && r.vertical <= 16, qPrintable(QString::number(r.vertical)));
        QVERIFY2(std::abs(r.rotate + 0.85) <= 0.15, qPrintable(QString::number(r.rotate)));
        // Harbour with few building lines and a mast: a level horizon, not a 3 degree roll.
        r = upright("DSC_0164.NEF");
        QVERIFY(r.ok);
        QVERIFY2(std::abs(r.rotate) < 1.5, qPrintable(QString::number(r.rotate)));
        // Lighthouse shot looking up: a strong, well-supported correction.
        r = upright("DSC_0154.NEF");
        QVERIFY(r.ok);
        QVERIFY2(r.vertical <= -45, qPrintable(QString::number(r.vertical)));
    }

    void constrainedCrop()
    {
        const QString f = testRaw("upright/DSC_0160.NEF");
        if (f.isEmpty())
            QSKIP("test raws not available");
        DevelopEngine e(f, {.halfSize = true});
        const QRectF full(0, 0, 1, 1);
        auto noBorders = [&](const DevelopSettings& s) {
            const Geometry g = e.geometry(s, true);
            for (QPointF p : {s.crop.topLeft(), s.crop.topRight(), s.crop.bottomLeft(), s.crop.bottomRight()})
                if (!g.canvasValid(p))
                    return false;
            return true;
        };
        // The whole frame: cropped to the largest valid area, and back when undone.
        DevelopSettings a;
        DevelopSettings b = a;
        b.rotate = 5;
        b = silma::constrainedCrop(e, a, b);
        QVERIFY(b.crop != full && b.crop.width() > 0.85 && noBorders(b));
        QVERIFY(std::abs(b.crop.width() - b.crop.height()) < 1e-6); // same aspect as the frame
        DevelopSettings c = b;
        c.perspVertical = 20; // an automatic crop follows further changes
        c = silma::constrainedCrop(e, b, c);
        QVERIFY(c.crop != b.crop && noBorders(c));
        DevelopSettings d = c;
        d.rotate = 0;
        d.perspVertical = 0;
        QCOMPARE(silma::constrainedCrop(e, c, d).crop, full);
        // A crop set by hand stays while it is valid, and shrinks about its centre otherwise.
        a.crop = QRectF(0.1, 0.1, 0.5, 0.5);
        b = a;
        b.rotate = 2;
        QCOMPARE(silma::constrainedCrop(e, a, b).crop, a.crop);
        b.rotate = -30;
        b = silma::constrainedCrop(e, a, b);
        QVERIFY(b.crop.width() < 0.5 && noBorders(b));
        QVERIFY(std::abs(b.crop.center().x() - 0.35) < 1e-9 && std::abs(b.crop.center().y() - 0.35) < 1e-9);
        // Off: nothing changes; turning it on applies it.
        a = DevelopSettings{};
        a.constrainCrop = false;
        b = a;
        b.rotate = 5;
        QCOMPARE(silma::constrainedCrop(e, a, b).crop, full);
        c = b;
        c.constrainCrop = true;
        QVERIFY(silma::constrainedCrop(e, b, c).crop != full);
        // Edits of the crop itself are left alone.
        c = a;
        c.constrainCrop = true;
        d = c;
        d.rotate = 5;
        d.crop = QRectF(0, 0, 0.5, 0.5);
        QCOMPARE(silma::constrainedCrop(e, c, d).crop, d.crop);
    }

    void healMatchesItsSurroundings()
    {
        if (!aiModelInstalled("lama"))
            QSKIP("LaMa model not installed (set SILMA_MODEL_DIR)");
        const QString f = testRaw("heal/DSC_0320.NEF");
        if (f.isEmpty())
            QSKIP("test raws not available");
        // A heal over plain cloudy sky (next to a bird, the photo's real heal) should give
        // back about the same sky; it used to come out as a cyan-green disc.
        DevelopEngine e(f);
        HealSpot spot;
        spot.x = 0.50;
        spot.y = 0.10;
        spot.radius = 0.012;
        spot.patchId = 1;
        e.addHealPatch(1, generateHealPatch(e.raw(), spot));
        DevelopSettings healed;
        healed.heals << spot;
        const QRectF roi(spot.x - 0.02, spot.y - 0.03, 0.04, 0.06);
        const QSize full = e.outputSize(healed);
        const RenderRequest req{.width = int(full.width() * roi.width()), .height = int(full.height() * roi.height()), .roi = roi};
        const QImage before = e.render(DevelopSettings(), req).image, after = e.render(healed, req).image;
        // Mean colour difference over the inner part of the spot, in 8-bit levels.
        const double r = spot.radius * full.width() * 0.8;
        double diff[3] = {};
        int n = 0;
        for (int y = 0; y < after.height(); ++y)
            for (int x = 0; x < after.width(); ++x)
                if (std::hypot(x - after.width() / 2.0, y - after.height() / 2.0) < r) {
                    const QRgb a = after.pixel(x, y), b = before.pixel(x, y);
                    diff[0] += qRed(a) - qRed(b);
                    diff[1] += qGreen(a) - qGreen(b);
                    diff[2] += qBlue(a) - qBlue(b);
                    ++n;
                }
        QVERIFY(n > 1000);
        for (int k = 0; k < 3; ++k)
            QVERIFY2(std::abs(diff[k] / n) < 3, qPrintable(QStringLiteral("channel %1 off by %2 levels").arg(k).arg(diff[k] / n)));
    }

    void autoToneSparesBlownSkies()
    {
        const QString blownSky = testRaw("auto-tone/DSC_0251.NEF"), brightSky = testRaw("auto-tone/DSC_0160.NEF");
        if (blownSky.isEmpty() || brightSky.isEmpty())
            QSKIP("test raws not available");
        // An overcast sky, about 11% blown: a moderate pull (the slider keeps blown areas near
        // white, so it no longer turns the sky into a flat grey or blue patch). Was -91, -55, -8.
        DevelopEngine blown(blownSky, {.halfSize = true});
        const double hi = blown.autoTone({}).highlights;
        QVERIFY2(hi >= -45 && hi <= -20, qPrintable(QString::number(hi)));
        // Bright but nothing clipped: a moderate pull (-36), and some contrast.
        DevelopEngine bright(brightSky, {.halfSize = true});
        const DevelopSettings b = bright.autoTone({});
        QVERIFY2(b.highlights >= -45 && b.highlights <= -28, qPrintable(QString::number(b.highlights)));
        QVERIFY2(b.contrast >= 10, qPrintable(QString::number(b.contrast)));
    }

    void textureAndDehaze()
    {
        QString f = testRaw("drone/DJI_0488.DNG"); // drone landscape with some haze
        if (f.isEmpty())
            QSKIP("test raws not available");
        DevelopEngine e(f, {.halfSize = true});
        const RenderRequest req{.width = 600, .height = 450};
        // Luminance statistics of a render: mean, standard deviation, mean chroma and
        // fine detail energy (differences between neighbours).
        struct Stats { double mean, sd, chroma, detail; };
        auto stats = [&](const DevelopSettings& s) {
            QImage img = e.render(s, req).image;
            double sum = 0, sum2 = 0, chroma = 0, detail = 0;
            const int n = img.width() * img.height();
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    QRgb p = img.pixel(x, y);
                    double l = 0.2126 * qRed(p) + 0.7152 * qGreen(p) + 0.0722 * qBlue(p);
                    sum += l;
                    sum2 += l * l;
                    chroma += std::max({qRed(p), qGreen(p), qBlue(p)}) - std::min({qRed(p), qGreen(p), qBlue(p)});
                    if (x > 0)
                        detail += std::abs(qGray(p) - qGray(img.pixel(x - 1, y)));
                }
            double mean = sum / n;
            return Stats{mean, std::sqrt(sum2 / n - mean * mean), chroma / n, detail / n};
        };
        DevelopSettings s;
        Stats base = stats(s);
        s.dehaze = 50;
        Stats dh = stats(s);
        QVERIFY2(dh.detail > base.detail * 1.05, qPrintable(QStringLiteral("%1 vs %2").arg(dh.detail).arg(base.detail)));
        QVERIFY(dh.chroma > base.chroma * 1.05);
        QVERIFY(dh.mean < base.mean); // removing haze darkens
        s.dehaze = -50;
        Stats haze = stats(s);
        QVERIFY(haze.sd < base.sd * 0.9);
        QVERIFY(haze.mean > base.mean); // a light veil
        s.dehaze = 0;
        s.texture = 100;
        Stats tx = stats(s);
        QVERIFY2(tx.detail > base.detail * 1.1, qPrintable(QStringLiteral("%1 vs %2").arg(tx.detail).arg(base.detail)));
        QVERIFY(std::abs(tx.mean - base.mean) < 3); // detail only, not brightness
        s.texture = -100;
        QVERIFY(stats(s).detail < base.detail * 0.9);
    }

    void nefDefaultCrop()
    {
        // NEFs carry no default crop: 8 pixels are left out on each side, like Lightroom
        // and the camera's JPEGs (D5600: 6016x4016 -> 6000x4000).
        QString f = testRaw("batch/DSC_0001.NEF");
        if (f.isEmpty())
            QSKIP("test raws not available");
        DevelopEngine e(f, {.halfSize = true});
        QCOMPARE(e.raw().fullWidth, 6016);
        QCOMPARE(QRect(e.raw().cropX, e.raw().cropY, e.raw().cropW, e.raw().cropH), QRect(8, 8, 6000, 4000));
        QCOMPARE(e.outputSize(DevelopSettings()), QSize(6000, 4000));
        DevelopSettings turned;
        turned.quarterTurns = 1;
        QCOMPARE(e.outputSize(turned), QSize(4000, 6000));
        // The frame's corners are 8 pixels in from the sensor's (and the lens auto-zoom).
        float src[6], gain[3];
        DevelopSettings noLens;
        noLens.lensDistortion = 0;
        const Geometry g = e.geometry(noLens);
        QVERIFY(g.mapCrop(0, 0, src, gain));
        const double x = (0.5 + (8.0 / 6016 - 0.5) / g.lensZoom()) * 6016, y = (0.5 + (8.0 / 4016 - 0.5) / g.lensZoom()) * 4016;
        QVERIFY2(std::abs(src[2] * 6016 - x) < 0.5 && std::abs(src[3] * 4016 - y) < 0.5,
                 qPrintable(QStringLiteral("%1 %2 / %3 %4").arg(src[2] * 6016).arg(src[3] * 4016).arg(x).arg(y)));
    }

    void exportJpeg()
    {
        QString f = testRaw("batch/DSC_0001.NEF");
        if (f.isEmpty())
            QSKIP("test raws not available");
        QTemporaryDir tmp;
        PhotoInfo p;
        p.path = f;
        p.fileName = QFileInfo(f).fileName();
        p.captureTime = QDateTime(QDate(2026, 8, 8), QTime(12, 0));
        ExportOptions o;
        o.folder = tmp.path();
        o.sizeMode = ExportSizeMode::LongEdge;
        o.size = 800;
        QString out = exportPhoto(p, DevelopSettings(), "x", 1, o);
        QCOMPARE(QFileInfo(out).fileName(), QString("20260808-%1.jpg").arg(QFileInfo(f).completeBaseName()));
        QImage img(out);
        QCOMPARE(std::max(img.width(), img.height()), 800);
        // Second export does not overwrite.
        QString out2 = exportPhoto(p, DevelopSettings(), "x", 1, o);
        QVERIFY(out2 != out);

        // Empty borders in the crop are reported.
        QStringList warnings;
        exportPhoto(p, DevelopSettings(), "x", 1, o, {}, &warnings);
        QVERIFY(warnings.isEmpty());
        DevelopSettings rotated;
        rotated.rotate = 8; // not constrained (settings as stored, e.g. from an old edit)
        exportPhoto(p, rotated, "x", 1, o, {}, &warnings);
        QCOMPARE(warnings.size(), 1);
        QVERIFY2(warnings[0].startsWith(p.fileName + ": empty (black) borders") && warnings[0].contains("% of the image"), qPrintable(warnings[0]));
        // The largest valid crop is fine.
        warnings.clear();
        DevelopEngine e(f, {.halfSize = true});
        const Geometry g = e.geometry(rotated, true);
        rotated.crop = autoCropRect(g, g.canvasSize().width() / g.canvasSize().height());
        exportPhoto(p, rotated, "x", 1, o, {}, &warnings);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        // A crop a pixel past the image (all round) leaves at most a pixel at the corners: not
        // reported; 10 px is.
        warnings.clear();
        const QSizeF cs = g.canvasSize();
        auto grown = [&](double px) {
            const double dx = px / cs.width(), dy = px / cs.height();
            return rotated.crop.adjusted(-dx, -dy, dx, dy);
        };
        QVERIFY(emptyCropBorders(g, grown(1)).depthPx > 0); // past the image, but not by 2 px
        DevelopSettings past = rotated;
        past.crop = grown(1);
        exportPhoto(p, past, "x", 1, o, {}, &warnings);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        past.crop = grown(10);
        exportPhoto(p, past, "x", 1, o, {}, &warnings);
        QCOMPARE(warnings.size(), 1);
        QVERIFY2(warnings[0].contains("px wide along the edge"), qPrintable(warnings[0]));
        warnings.clear();
        // So is a camera profile that is not installed.
        DevelopSettings withProfile;
        withProfile.profile = "No Such Profile";
        exportPhoto(p, withProfile, "x", 1, o, {}, &warnings);
        QCOMPARE(warnings.size(), 1);
        QVERIFY2(warnings[0].startsWith(p.fileName + ": camera profile \"No Such Profile\" is not installed"), qPrintable(warnings[0]));
    }

    void lensCorrectionInParallel()
    {
        // lensfun is not thread-safe: engines made at the same time (parallel exports) used to
        // find no lens now and then, and rendered without lens corrections.
        const QString f = testRaw("batch/DSC_0001.NEF");
        if (f.isEmpty())
            QSKIP("test raws not available");
        const QString expected = DevelopEngine(f, {.halfSize = true}).lensDescription();
        QVERIFY(expected.startsWith("lensfun"));
        QList<int> runs(64);
        std::iota(runs.begin(), runs.end(), 0);
        QThreadPool pool;
        pool.setMaxThreadCount(16);
        const QStringList got = QtConcurrent::blockingMapped<QStringList>(&pool, runs, [&](int) {
            RawImage raw = decodeRaw(f, {.halfSize = true});
            LensQuery q;
            q.cameraMake = raw.make;
            q.cameraModel = raw.model;
            const MetadataSummary md = readMetadataSummary(f);
            q.lens = md.lens;
            q.focalLength = md.focalLength;
            q.aperture = md.fNumber;
            return LensCorrection::build(raw, q)->description();
        });
        for (const QString& d : got)
            QCOMPARE(d, expected);
    }

    void decodeKeepsUnclippedChannels()
    {
        const QString f = testRaw("auto-tone/DSC_0251.NEF"); // overcast sky, about 11% blown
        if (f.isEmpty())
            QSKIP("test raws not available");
        const RawImage raw = decodeRaw(f, {.halfSize = true});
        // Channel c clips at appliedMul[c] (green at 1); red and blue keep their values above
        // 1 where green is clipped (they were cut at 1 before 2026-10).
        float mx[3] = {0, 0, 0};
        for (size_t i = 0; i < raw.image.pixelCount(); ++i)
            for (int c = 0; c < 3; ++c)
                mx[c] = std::max(mx[c], raw.image.data[i * 3 + c]);
        for (int c = 0; c < 3; ++c)
            QVERIFY2(mx[c] <= raw.color.appliedMul[c] * 1.001, qPrintable(QString("%1: %2 > %3").arg(c).arg(mx[c]).arg(raw.color.appliedMul[c])));
        QVERIFY(std::abs(mx[1] - 1.0f) < 0.01f);
        QVERIFY2(std::max(mx[0], mx[2]) > 1.2f, qPrintable(QString("%1 %2").arg(mx[0]).arg(mx[2])));
    }

    void exportAsksAboutExistingFiles()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        auto f = [&](const QString& n) { return tmp.filePath(n); };
        for (const char* n : {"a.jpg", "b.jpg"}) {
            QFile file(f(n));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        // a and b exist; c does not; the second a is the same name again in this export.
        const QStringList planned = {f("a.jpg"), f("b.jpg"), f("c.jpg"), f("a.jpg")};
        QList<int> asked;
        auto always = [&](ExistingFileChoice c) {
            return [&, c](const QString&, int left) {
                asked << left;
                return ExistingFileAnswer{c, true};
            };
        };
        auto r = resolveExportPaths(planned, always(ExistingFileChoice::Rename));
        QCOMPARE(*r, QStringList({f("a-2.jpg"), f("b-2.jpg"), f("c.jpg"), f("a-3.jpg")}));
        QCOMPARE(asked, QList<int>({3})); // asked once (3 existing), then applied to all
        asked.clear();
        r = resolveExportPaths(planned, always(ExistingFileChoice::Overwrite));
        QCOMPARE(*r, QStringList({f("a.jpg"), f("b.jpg"), f("c.jpg"), f("a-2.jpg")})); // never twice into one file
        r = resolveExportPaths(planned, always(ExistingFileChoice::Skip));
        QCOMPARE(*r, QStringList({QString(), QString(), f("c.jpg"), QString()}));
        QVERIFY(!resolveExportPaths(planned, always(ExistingFileChoice::Cancel)));
        // Without "apply to all" every existing file is asked about.
        asked.clear();
        int k = 0;
        r = resolveExportPaths(planned, [&](const QString&, int left) {
            asked << left;
            return ExistingFileAnswer{k++ == 0 ? ExistingFileChoice::Skip : ExistingFileChoice::Overwrite, false};
        });
        QCOMPARE(asked, QList<int>({3, 2, 1}));
        QCOMPARE(*r, QStringList({QString(), f("b.jpg"), f("c.jpg"), f("a.jpg")}));
        // Nothing exists: no questions, names unchanged.
        asked.clear();
        r = resolveExportPaths({f("x.jpg"), f("y.jpg")}, always(ExistingFileChoice::Cancel));
        QCOMPARE(*r, QStringList({f("x.jpg"), f("y.jpg")}));
        QVERIFY(asked.isEmpty());
    }

    void highlightsKeepBlownAreasWhite()
    {
        const QString f = testRaw("auto-tone/DSC_0251.NEF"); // overcast sky, about 11% blown
        if (f.isEmpty())
            QSKIP("test raws not available");
        DevelopEngine e(f, {.halfSize = true});
        const RenderRequest req{.width = 600, .height = 400};
        const ImageF base = e.renderEncoded(DevelopSettings(), req);
        // Mean of the pixels that are white / in the midtones at default settings.
        auto means = [&](const DevelopSettings& s) {
            const ImageF img = e.renderEncoded(s, req);
            double white = 0, mid = 0;
            int nw = 0, nm = 0;
            for (size_t i = 0; i < img.pixelCount(); ++i) {
                const float* b = base.data.data() + i * 3;
                const float* p = img.data.data() + i * 3;
                const float v = (p[0] + p[1] + p[2]) / 3, bv = (b[0] + b[1] + b[2]) / 3;
                if (std::min({b[0], b[1], b[2]}) > 0.98f) {
                    white += v;
                    ++nw;
                } else if (bv > 0.2f && bv < 0.45f) {
                    mid += v;
                    ++nm;
                }
            }
            return std::pair{white / std::max(nw, 1), mid / std::max(nm, 1)};
        };
        const auto [white0, mid0] = means(DevelopSettings());
        DevelopSettings s;
        s.highlights = -100;
        const auto [white2, mid2] = means(s);
        const QString info = QString("white %1 / %2, mid %3 / %4").arg(white0).arg(white2).arg(mid0).arg(mid2);
        // The blown sky stays nearly white (the earlier curve turned it grey: 0.65).
        QVERIFY2(white2 > 0.85, qPrintable(info));
        // Midtones hardly change.
        QVERIFY2(std::abs(mid2 - mid0) < 0.04, qPrintable(info));
    }
};

QTEST_GUILESS_MAIN(TestEngine)
#include "test_engine.moc"
