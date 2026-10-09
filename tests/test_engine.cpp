#include "Color.h"
#include "Engine.h"
#include "Export.h"
#include "AiDenoise.h"
#include "Heal.h"
#include "RawDecoder.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

using namespace silma;

static QString sample(const char* sub)
{
    QDir d(QString(SILMA_SAMPLES) + "/" + sub);
    auto l = d.entryList(QDir::Files, QDir::Name);
    return l.isEmpty() ? QString() : d.filePath(l.first());
}

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
            QVERIFY2(k == "wb" || k == "curve" || k == "crop" || findParam(k), p.key);
        }
    }

    void whiteBalancePicker()
    {
        const QString f = sample("2026-08-08");
        if (f.isEmpty())
            QSKIP("samples not available");
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

    void bayerMosaic()
    {
        // The mosaic matches LibRaw's demosaiced image per colour (without white balance).
        for (const char* dir : {"2026-08-08", "2026-05-17"}) {
            const QString f = sample(dir);
            if (f.isEmpty())
                QSKIP("samples not available");
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
        const QString f = sample("2026-05-17");
        if (f.isEmpty())
            QSKIP("samples not available");
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
        check(aiDenoiseRaw("rawnind", f, raw, 1.0, [&](int, int) { ++steps; }));
        QVERIFY(steps > 1);
        // Strength 0 is the input unchanged.
        const ImageF none = aiDenoiseRaw("rawnind", f, raw, 0.0);
        QVERIFY(none.data == raw.image.data);
        // The variant for other sensors (on the demosaiced image).
        qputenv("SILMA_AI_FORCE_LINEAR", "1");
        check(aiDenoiseRaw("rawnind", f, raw, 1.0));
        qunsetenv("SILMA_AI_FORCE_LINEAR");
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
    }

    void renderSamples()
    {
        for (const char* sub : {"2026-07-05", "2026-05-17"}) {
            QString f = sample(sub);
            if (f.isEmpty())
                QSKIP("samples not available");
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

    void textureAndDehaze()
    {
        QString f = sample("2026-05-17"); // drone landscape with some haze
        if (f.isEmpty())
            QSKIP("samples not available");
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

    void exportJpeg()
    {
        QString f = sample("2026-08-08");
        if (f.isEmpty())
            QSKIP("samples not available");
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
    }
};

QTEST_GUILESS_MAIN(TestEngine)
#include "test_engine.moc"
