#include "DngWriter.h"
#include "Engine.h"
#include "Importer.h"
#include "RawDecoder.h"
#include "Upright.h"
#include "TestRaws.h"

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

    void defaultCropRoundTrip()
    {
        // DNGs made from a raw keep its image area, so its crop applies to them unchanged.
        QTemporaryDir tmp;
        ImageF img(616, 416);
        LinearDngInfo info;
        info.make = "Test";
        info.model = "Cam";
        info.color.xyzToCam = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
        info.defaultCrop = QRect(8, 8, 600, 400);
        QString f = tmp.filePath("c.dng");
        writeLinearDng(f, img, info);
        RawImage r = decodeRaw(f);
        QCOMPARE(QRect(r.cropX, r.cropY, r.cropW, r.cropH), info.defaultCrop);
        QCOMPARE(DevelopEngine(f).outputSize(DevelopSettings()), QSize(600, 400));
        // Without one: all of it (no 8 pixel inset for DNGs).
        info.defaultCrop = {};
        writeLinearDng(f, img, info);
        r = decodeRaw(f);
        QCOMPARE(QRect(r.cropX, r.cropY, r.cropW, r.cropH), QRect(0, 0, 616, 416));
    }

    void transparencyMaskRoundTrip()
    {
        // Our own panoramas store their coverage as a DNG transparency mask.
        QTemporaryDir tmp;
        ImageF img(700, 400);
        LinearDngInfo info;
        info.make = "Test";
        info.model = "Cam";
        info.color.xyzToCam = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
        info.transparencyMask.assign(img.pixelCount(), 0);
        for (int y = 0; y < img.height; ++y)
            for (int x = 0; x < img.width; ++x)
                if (x < 420) { // left 60% covered
                    info.transparencyMask[size_t(y) * img.width + x] = 255;
                    for (int c = 0; c < 3; ++c)
                        img.px(x, y)[c] = 0.25f;
                }
        QString f = tmp.filePath("m.dng");
        writeLinearDng(f, img, info, true);
        RawImage r = decodeRaw(f);
        // LibRaw still decodes the main image, not the mask.
        QCOMPARE(r.image.width, 700);
        QVERIFY(std::abs(r.image.px(100, 100)[1] - 0.25f) < 0.01f);
        QVERIFY(r.valid);
        QVERIFY(r.valid->at(0.3, 0.5));
        QVERIFY(!r.valid->at(0.7, 0.5));
        DevelopEngine e(f);
        QRectF c = autoCropRect(e.geometry(DevelopSettings(), true), 0);
        QVERIFY2(c.right() <= 0.6 && c.width() > 0.55 && c.height() > 0.95,
                 qPrintable(QStringLiteral("%1 %2 %3 %4").arg(c.x()).arg(c.y()).arg(c.width()).arg(c.height())));
    }

    void jpegXlDng()
    {
        // Lightroom panorama: DNG 1.7, JPEG XL compressed linear raw with a transparency mask.
        const QString f = testRaw("dng/DSC_0785-Pano.dng");
        if (f.isEmpty())
            QSKIP("test raw dng/DSC_0785-Pano.dng not present");
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

        // Auto crop keeps to the stitched area: any shape when free, or the given aspect.
        DevelopEngine e(f, {.halfSize = true});
        Geometry g = e.geometry(DevelopSettings(), true);
        QVERIFY(g.hasValidMask());
        const QSizeF cs = g.canvasSize();
        for (double aspect : {0.0, 16.0 / 9.0}) {
            QRectF c = autoCropRect(g, aspect);
            for (QPointF p : {c.topLeft(), c.topRight(), c.bottomLeft(), c.bottomRight(), c.center()})
                QVERIFY2(g.canvasValid(p), qPrintable(QStringLiteral("aspect %1: %2,%3").arg(aspect).arg(p.x()).arg(p.y())));
            const double area = c.width() * c.height();
            if (aspect == 0)
                QVERIFY2(area > 0.6, qPrintable(QString::number(area)));
            else
                QVERIFY(std::abs(c.width() * cs.width() / (c.height() * cs.height()) - aspect) < 0.02);
        }
    }

    void jpegXlDngMapPolynomial()
    {
        // Lightroom panorama whose data is stored scaled per channel (and with a curve), mapped
        // back by MapPolynomial opcodes in OpcodeList2; ignoring them made it magenta.
        const QString f = testRaw("dng/DJI_0487-Pano.dng");
        if (f.isEmpty())
            QSKIP("test raw dng/DJI_0487-Pano.dng not present");
        RawImage r = decodeRaw(f, {.halfSize = true});
        double sum[3] = {0, 0, 0};
        for (size_t i = 0; i < r.image.data.size(); ++i)
            sum[i % 3] += r.image.data[i];
        const double n = double(r.image.data.size() / 3);
        // A snowy town, white-balanced: about neutral and as bright as the frames it was made of.
        QVERIFY2(sum[1] / n > 0.1 && sum[1] / n < 0.35, qPrintable(QString::number(sum[1] / n)));
        for (int c : {0, 2})
            QVERIFY2(std::abs(sum[c] / sum[1] - 1) < 0.15, qPrintable(QStringLiteral("%1: %2").arg(c).arg(sum[c] / sum[1])));
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
