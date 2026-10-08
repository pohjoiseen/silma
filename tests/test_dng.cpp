#include "DngWriter.h"
#include "Importer.h"
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

    void jpegXlDng()
    {
        // Lightroom panorama: DNG 1.7, JPEG XL compressed linear raw with a transparency mask.
        const QString f = QString(SILMA_SAMPLES) + "/DSC_0785-Pano.dng";
        if (!QFileInfo::exists(f))
            QSKIP("DSC_0785-Pano.dng sample not present");
        RawImage r = decodeRaw(f);
        QCOMPARE(r.image.width, 12147);
        QCOMPARE(r.image.height, 4453);
        double mean = 0;
        for (float v : r.image.data)
            mean += v;
        mean /= r.image.data.size();
        // Decoded as linear (not with the codestream's sRGB curve, which gave ~0.59).
        QVERIFY2(mean > 0.2 && mean < 0.42, qPrintable(QString::number(mean)));
        // Outside the stitched area (top left corner) is masked to black; the middle is not.
        QCOMPARE(r.image.px(5, 5)[1], 0.0f);
        QVERIFY(r.image.px(6000, 2200)[1] > 0.01f);
        RawImage half = decodeRaw(f, {.halfSize = true});
        QCOMPARE(half.image.width, 12147 / 2);
        QCOMPARE(half.fullWidth, 12147);
        // The thumbnail comes from the 1024 px JPEG XL preview, not the 256 px JPEG one.
        QImage thumb = QImage::fromData(thumbnailFromEmbeddedPreview(f, 1));
        QCOMPARE(thumb.width(), 512);
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
