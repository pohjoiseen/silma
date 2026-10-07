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
