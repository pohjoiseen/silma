#include "Batch.h"
#include "Importer.h"

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
        QDir samples(SILMA_SAMPLES);
        QStringList files;
        for (const char* sub : {"2026-07-05", "2026-05-17"}) {
            QDir d(samples.filePath(sub));
            auto list = d.entryList(QDir::Files, QDir::Name);
            if (!list.isEmpty())
                files << d.filePath(list.first());
        }
        if (files.size() < 2)
            QSKIP("sample raws not available");
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
};

QTEST_GUILESS_MAIN(TestBatch)
#include "test_batch.moc"
