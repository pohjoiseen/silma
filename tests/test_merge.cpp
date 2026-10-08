#include "Batch.h"
#include "Engine.h"
#include "Importer.h"
#include "Merge.h"
#include "Upright.h"

#include <QDir>
#include <QTemporaryDir>

#include <QtTest>

using namespace silma;

class TestMerge : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    Batch m_batch;
    QList<qint64> m_ids;

private slots:
    void initTestCase()
    {
        // DSC_0010..0012 of 2026-08-08 are a handheld sequence.
        QDir d(QString(SILMA_SAMPLES) + "/2026-08-08");
        QStringList files;
        for (const char* f : {"DSC_0010.NEF", "DSC_0011.NEF", "DSC_0012.NEF"})
            if (d.exists(f))
                files << d.filePath(f);
        if (files.size() < 3)
            QSKIP("samples not available");
        m_batch.create(m_tmp.filePath("m.silma"));
        m_ids = importFiles(m_batch, files, {}).added;
        QCOMPARE(m_ids.size(), 3);
    }

    void largestRectangle_()
    {
        std::vector<uint8_t> m(10 * 6, 0);
        for (int y = 1; y < 5; ++y)
            for (int x = 2; x < 9; ++x)
                m[y * 10 + x] = 1;
        QCOMPARE(largestRectangle(m, 10, 6), QRect(2, 1, 7, 4));
    }

    void panorama()
    {
        qint64 id = createPanorama(m_batch, m_ids, {});
        auto p = m_batch.photo(id);
        QVERIFY(p && p->kind == PhotoKind::DerivedDng);
        QVERIFY(p->width >= 6016);
        DevelopEngine e(p->path, {.halfSize = true});
        const DevelopSettings s = DevelopSettings::fromJsonString(m_batch.settings(id));
        auto r = e.render(s, {.width = 400, .height = 300});
        QVERIFY(!r.image.isNull());
        // The DNG carries its coverage: auto crop later finds about the merge's own crop.
        Geometry g = e.geometry(s, true);
        QVERIFY(g.hasValidMask());
        QRectF c = autoCropRect(g, 0);
        for (QPointF p : {c.topLeft(), c.topRight(), c.bottomLeft(), c.bottomRight()})
            QVERIFY(g.canvasValid(p));
        const double area = c.width() * c.height(), mergeArea = s.crop.width() * s.crop.height();
        QVERIFY2(std::abs(area - mergeArea) < 0.1 * mergeArea, qPrintable(QStringLiteral("%1 vs %2").arg(area).arg(mergeArea)));
    }

    void hdr()
    {
        qint64 id = createHdrMerge(m_batch, m_ids.mid(0, 2), {});
        auto p = m_batch.photo(id);
        QVERIFY(p && QFile::exists(p->path));
        QCOMPARE(p->width, 6016);
        QVERIFY(m_batch.history(id).size() == 2);
    }
};

QTEST_GUILESS_MAIN(TestMerge)
#include "test_merge.moc"
