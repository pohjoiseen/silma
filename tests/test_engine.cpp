#include "Color.h"
#include "Engine.h"
#include "Export.h"

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
