#include "DngWriter.h"
#include "RawDecoder.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace silma;

class TestDng : public QObject {
    Q_OBJECT
private slots:
    void levelsSurviveRoundTrip()
    {
        QTemporaryDir tmp;
        ImageF img(600, 520);
        for (size_t i = 0; i < img.data.size(); ++i)
            img.data[i] = 0.25f;
        img.px(10, 10)[0] = 0.5f; // max well below 1
        LinearDngInfo info;
        info.make = "Test";
        info.model = "Cam";
        info.color.xyzToCam = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
        QString f = tmp.filePath("t.dng");
        writeLinearDng(f, img, info);
        RawImage r;
        try {
            r = decodeRaw(f);
        } catch (const std::exception& e) {
            QFAIL(e.what());
        }
        QCOMPARE(r.image.width, 600);
        double mean = 0;
        for (float v : r.image.data)
            mean += v;
        mean /= r.image.data.size();
        qInfo() << "decoded mean" << mean << "applied" << r.color.appliedMul[0];
        // Camera-native 0.25 white-balanced with multipliers 1 stays 0.25.
        QVERIFY2(std::abs(mean - 0.25) < 0.01, qPrintable(QString::number(mean)));
    }

    void hdrValuesAboveOne()
    {
        QTemporaryDir tmp;
        ImageF img(600, 520);
        for (size_t i = 0; i < img.data.size(); ++i)
            img.data[i] = 0.25f;
        img.px(10, 10)[0] = 4.0f;
        LinearDngInfo info;
        info.make = "Test";
        info.model = "Cam";
        info.color.xyzToCam = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
        QString f = tmp.filePath("h.dng");
        writeLinearDng(f, img, info);
        RawImage r = decodeRaw(f);
        double mean = 0, mx = 0;
        for (float v : r.image.data) {
            mean += v;
            mx = std::max(mx, double(v));
        }
        mean /= r.image.data.size();
        qInfo() << "hdr decoded mean" << mean << "max" << mx;
    }
};

QTEST_GUILESS_MAIN(TestDng)
#include "test_dng.moc"
