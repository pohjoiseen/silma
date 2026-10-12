#include "Batch.h"
#include "AiDenoise.h"
#include "Derived.h"
#include "Develop.h"
#include "Importer.h"
#include "RawDecoder.h"
#include "TestRaws.h"

#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <exiv2/exiv2.hpp>

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

using namespace silma;

class TestBatch : public QObject {
    Q_OBJECT

private slots:
    void createAndReopen()
    {
        QTemporaryDir tmp;
        QString f = tmp.filePath("trip.silma");
        {
            Batch b;
            b.create(f);
            auto im = b.addImport("2026-07-05");
            QVERIFY(QDir(im.path).exists());
            QCOMPARE(im.path, tmp.filePath("2026-07-05"));
        }
        Batch b;
        b.open(f);
        QCOMPARE(b.imports().size(), 1);
        QCOMPARE(b.db().userVersion(), Batch::kSchemaVersion);
        QVERIFY_THROWS_EXCEPTION(DbError, Batch().create(f));
    }

    void rejectsNewerSchema()
    {
        QTemporaryDir tmp;
        QString f = tmp.filePath("x.silma");
        {
            Batch b;
            b.create(f);
            b.db().setUserVersion(Batch::kSchemaVersion + 1);
        }
        QVERIFY_THROWS_EXCEPTION(DbError, Batch().open(f));
    }

    void migratesFromV1()
    {
        QTemporaryDir tmp;
        QString f = tmp.filePath("old.silma");
        {
            Batch b;
            b.create(f);
            b.addImport("a");
            // Simulate a batch written by schema version 1.
            b.db().exec("DROP TABLE patches");
            b.db().setUserVersion(1);
        }
        Batch b;
        b.open(f);
        QCOMPARE(b.db().userVersion(), Batch::kSchemaVersion);
        QCOMPARE(b.imports().size(), 1);
        auto im = b.imports().first();
        PhotoInfo p;
        p.importId = im.id;
        p.fileName = "x.nef";
        p.path = im.path + "/x.nef";
        qint64 id = b.addPhoto(p);
        qint64 patch = b.addPatch(id, QRect(1, 2, 3, 4), QByteArray("data"));
        QRect r;
        QVERIFY(b.patch(patch, &r, nullptr));
        QCOMPARE(r, QRect(1, 2, 3, 4));
    }

    void historyUndoRedo()
    {
        QTemporaryDir tmp;
        Batch b;
        b.create(tmp.filePath("h.silma"));
        auto im = b.addImport("a");
        PhotoInfo p;
        p.importId = im.id;
        p.fileName = "x.nef";
        p.path = im.path + "/x.nef";
        qint64 id = b.addPhoto(p);
        QCOMPARE(b.settings(id), QString("{}"));
        b.pushHistory(id, "Exposure", R"({"exposure":1})");
        b.pushHistory(id, "Contrast", R"({"exposure":1,"contrast":10})");
        QCOMPARE(b.history(id).size(), 3);
        QCOMPARE(b.setHistoryPos(id, 1), QString(R"({"exposure":1})"));
        QCOMPARE(b.settings(id), QString(R"({"exposure":1})"));
        // New edit after undo drops the redo tail.
        b.pushHistory(id, "Shadows", R"({"exposure":1,"shadows":5})");
        auto h = b.history(id);
        QCOMPARE(h.size(), 3);
        QCOMPARE(h.last().label, QString("Shadows"));
        QCOMPARE(b.historyPos(id), 2);
        b.removePhoto(id);
        QCOMPARE(b.history(id).size(), 0);
    }

    void importSamples()
    {
        // Taken on different days.
        const QStringList files = {testRaw("other-day/DSC_0001.NEF"), testRaw("drone/DJI_0488.DNG")};
        if (files.contains(QString()))
            QSKIP("test raws not available");
        QTemporaryDir tmp;
        Batch b;
        b.create(tmp.filePath("t.silma"));
        auto r = importFiles(b, files, {.splitByDate = true});
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join('\n')));
        QCOMPARE(r.added.size(), 2);
        QCOMPARE(b.imports().size(), 2);
        auto ps = b.photos();
        for (const auto& p : ps) {
            QVERIFY(QFile::exists(p.path));
            QVERIFY(!b.thumbnail(p.id).isEmpty());
            QVERIFY(p.captureTime.isValid());
        }
        // Duplicate import is skipped.
        auto r2 = importFiles(b, files, {});
        QCOMPARE(r2.added.size(), 0);
        QCOMPARE(r2.skipped.size(), 2);
    }

    void importJpegAndHeic()
    {
        const QString nef = testRaw("batch/DSC_0001.NEF");
        if (nef.isEmpty())
            QSKIP("test raws not available");
        QTemporaryDir tmp;
        QDir src(tmp.filePath("card"));
        QVERIFY(src.mkpath("."));
        const QString raw = src.filePath("DSC_0001.NEF");
        QVERIFY(QFile::copy(nef, raw));
        QImage img(300, 200, QImage::Format_RGB888);
        img.fill(QColor(200, 120, 60));
        QVERIFY(img.save(src.filePath("DSC_0001.JPG"), "JPEG", 90)); // the raw's sidecar
        QVERIFY(img.save(src.filePath("phone.jpg"), "JPEG", 90));
        // A portrait phone photo stored landscape with an EXIF orientation.
        QVERIFY(img.save(src.filePath("rotated.jpg"), "JPEG", 90));
        {
            auto e = Exiv2::ImageFactory::open(src.filePath("rotated.jpg").toStdString());
            e->readMetadata();
            e->exifData()["Exif.Image.Orientation"] = uint16_t(6);
            e->writeMetadata();
        }
        const QString heifEnc = QStandardPaths::findExecutable("heif-enc");
        if (!heifEnc.isEmpty())
            QCOMPARE(QProcess::execute(heifEnc, {"-q", "80", "-o", src.filePath("phone.heic"), src.filePath("phone.jpg")}), 0);

        Batch b;
        b.create(tmp.filePath("t.silma"));
        auto r = importFiles(b, collectPhotoFiles({src.path()}), {});
        QVERIFY2(r.errors.isEmpty(), qPrintable(r.errors.join('\n')));
        QCOMPARE(r.sidecars, QStringList{src.filePath("DSC_0001.JPG")});
        QCOMPARE(r.added.size(), heifEnc.isEmpty() ? 3 : 4);
        for (const auto& p : b.photos()) {
            QVERIFY(!b.thumbnail(p.id).isEmpty());
            const bool rendered = p.fileName != "DSC_0001.NEF";
            QCOMPARE(p.kind, rendered ? PhotoKind::Rendered : PhotoKind::Raw);
            // JPEG/HEIC start without sharpening, colour noise reduction and lens corrections.
            QCOMPARE(DevelopSettings::fromJsonString(b.settings(p.id)), DevelopSettings::defaults(rendered));
            if (!rendered)
                continue;
            const RawImage r = decodeRaw(p.path);
            QVERIFY(r.rendered);
            QCOMPARE(QSize(r.image.width, r.image.height), p.fileName == "rotated.jpg" ? QSize(200, 300) : QSize(300, 200));
            QCOMPARE(r.orientation, 1);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, createAiDenoised(b, p.id, defaultAiDenoiseModel(), 1.0));
        }
    }
};

QTEST_GUILESS_MAIN(TestBatch)
#include "test_batch.moc"
