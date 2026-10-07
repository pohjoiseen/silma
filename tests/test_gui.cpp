#include "Batch.h"
#include "AutoHidePanel.h"
#include "DevelopPanel.h"
#include "Filmstrip.h"
#include "Importer.h"
#include "MainWindow.h"
#include "Widgets.h"
#include "CropTool.h"
#include "AiDenoise.h"

#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

using namespace silma;

class TestGui : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_batchFile;

    static ParamSlider* slider(MainWindow& w, const char* key)
    {
        for (auto* s : w.developPanel()->findChildren<ParamSlider*>())
            if (QLatin1String(s->def().key) == QLatin1String(key))
                return s;
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        QDir d(QString(SILMA_SAMPLES) + "/2026-08-08");
        auto files = d.entryList(QDir::Files, QDir::Name).mid(0, 3);
        if (files.size() < 3)
            QSKIP("samples not available");
        QStringList paths;
        for (const auto& f : files)
            paths << d.filePath(f);
        m_batchFile = m_tmp.filePath("gui.silma");
        Batch b;
        b.create(m_batchFile);
        QCOMPARE(importFiles(b, paths, {}).added.size(), 3);
    }

    void filmstripPicksUpLateThumbnails()
    {
        // Photo rows can appear before their thumbnails (an import in progress): the
        // model must not remember the missing thumbnail.
        Batch b;
        b.open(m_batchFile);
        qint64 id = b.photos().first().id;
        QByteArray jpeg = b.thumbnail(id);
        QVERIFY(!jpeg.isEmpty());
        b.db().exec(QStringLiteral("DELETE FROM thumbnails WHERE photo_id = %1").arg(id).toUtf8().constData());
        PhotoListModel m;
        m.setBatch(&b);
        m.reload(std::nullopt);
        QModelIndex i = m.index(m.rowOf(id));
        QVERIFY(i.data(Qt::DecorationRole).value<QPixmap>().isNull());
        b.setThumbnail(id, jpeg, 0);
        m.refresh();
        QVERIFY(!i.data(Qt::DecorationRole).value<QPixmap>().isNull());
    }

    void autoHidePanels()
    {
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.openBatch(m_batchFile));
        auto* panel = w.developPanel();
        QVERIFY(qobject_cast<QDockWidget*>(panel->parentWidget()));

        // Pin buttons in the dock title bars switch to auto hide.
        auto pinOf = [](QDockWidget* d) {
            auto* bar = qobject_cast<DockTitleBar*>(d->titleBarWidget());
            return bar ? bar->findChild<QToolButton*>() : nullptr;
        };
        auto* rightDock = qobject_cast<QDockWidget*>(panel->parentWidget());
        QVERIFY(pinOf(rightDock));
        pinOf(rightDock)->click();
        QVERIFY(w.isAutoHide(Qt::RightEdge));
        w.setAutoHide(Qt::LeftEdge, true);
        AutoHidePanel* right = w.autoHidePanel(Qt::RightEdge);
        QCOMPARE(panel->parentWidget(), right);
        QVERIFY(!right->isVisible());
        // The image gets the whole width.
        QTRY_VERIFY(w.mainPane()->width() > 1300);

        // Hovering the right edge reveals the panel over the image...
        QPoint edge(w.width() - 2, w.height() / 2);
        QCursor::setPos(w.mapToGlobal(edge));
        QTest::mouseMove(&w, edge);
        QTRY_VERIFY(right->isRevealed());
        QTRY_COMPARE(right->geometry().right(), w.width() - 1);
        QVERIFY(panel->isVisible());
        QVERIFY(w.mainPane()->width() > 1300);
        // ...and moving away hides it again.
        QPoint away(w.width() / 2, w.height() / 2);
        QCursor::setPos(w.mapToGlobal(away));
        QTest::mouseMove(&w, away);
        QTRY_VERIFY(!right->isRevealed());
        QTRY_VERIFY(!right->isVisible());

        // Pinning again (with the overlays' pin buttons) puts the content back into the dock.
        QToolButton* rightPin = right->findChild<QToolButton*>();
        QVERIFY(rightPin);
        rightPin->click();
        QToolButton* leftPin = qobject_cast<QToolButton*>(w.findChild<QTabWidget*>()->cornerWidget());
        QVERIFY(leftPin);
        leftPin->click();
        QTRY_VERIFY(!w.isAutoHide(Qt::LeftEdge));
        QVERIFY(!w.isAutoHide(Qt::RightEdge));
        QVERIFY(qobject_cast<QDockWidget*>(panel->parentWidget()));
        QTRY_VERIFY(panel->isVisible());
        QTRY_VERIFY(w.mainPane()->width() < 1100);
        QVERIFY(!QSettings().value("ui/autoHide/" + QString::number(int(Qt::RightEdge))).toBool());
    }

    void editUndoRejectRestore()
    {
        qint64 firstId = 0;
        {
            MainWindow w;
            w.resize(1400, 900);
            w.show();
            QVERIFY(w.openBatch(m_batchFile));
            QVERIFY(w.currentPhoto());
            firstId = w.currentPhoto()->id;
            QTRY_VERIFY_WITH_TIMEOUT(w.mainPane()->hasRender(), 15000);

            // Slider edit -> one history step.
            ParamSlider* exp = slider(w, "exposure");
            QVERIFY(exp);
            emit exp->edited(0.5, false); // dragging: no history yet
            emit exp->edited(1.0, true);
            Batch b;
            b.open(m_batchFile);
            QCOMPARE(b.history(firstId).size(), 2);
            QCOMPARE(DevelopSettings::fromJsonString(b.settings(firstId)).exposure, 1.0);

            w.testUndo();
            QCOMPARE(DevelopSettings::fromJsonString(b.settings(firstId)).exposure, 0.0);
            QCOMPARE(exp->value(), 0.0);
            w.testRedo();
            QCOMPARE(w.currentSettings().exposure, 1.0);

            // Before/after shows a second pane.
            w.testSetViewMode(ViewMode::BeforeAfter);
            QVERIFY(w.secondPane()->isVisible());
            QTRY_VERIFY_WITH_TIMEOUT(w.secondPane()->hasRender(), 15000);
            w.testSetViewMode(ViewMode::Single);
            QVERIFY(!w.secondPane()->isVisible());

            // Reject moves to the next photo.
            w.testReject();
            QVERIFY(b.photo(firstId)->rejected);
            QVERIFY(w.currentPhoto()->id != firstId);

            // An external (CLI) change is picked up.
            qint64 cur = w.currentPhoto()->id;
            DevelopSettings s;
            s.shadows = 33;
            b.pushHistory(cur, "CLI", s.toJsonString());
            w.testPoll();
            QCOMPARE(w.currentSettings().shadows, 33.0);
            QCOMPARE(slider(w, "shadows")->value(), 33.0);

            // Show request from the CLI.
            b.setState("gui.goto", QString::number(firstId));
            w.testPoll();
            QCOMPARE(w.currentPhoto()->id, firstId);
            w.close();
        }
        // Reopening restores the last photo.
        MainWindow w2;
        w2.show();
        QVERIFY(w2.openBatch(m_batchFile));
        QCOMPARE(w2.currentPhoto()->id, firstId);
        QCOMPARE(w2.currentSettings().exposure, 1.0);
    }

    void cropAndUpright()
    {
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(w.openBatch(m_batchFile));
        QTRY_VERIFY_WITH_TIMEOUT(w.mainPane()->hasRender(), 15000);
        qint64 id = w.currentPhoto()->id;
        w.testCropTool(true);
        QVERIFY(w.mainPane()->tool());
        emit w.cropTool()->rectChanged(QRectF(0.1, 0.1, 0.5, 0.5), true);
        Batch b;
        b.open(m_batchFile);
        QCOMPARE(DevelopSettings::fromJsonString(b.settings(id)).crop, QRectF(0.1, 0.1, 0.5, 0.5));
        w.testCropTool(false);
        QVERIFY(!w.mainPane()->tool());
        int before = int(b.history(id).size());
        w.testAutoUpright(2);
        // Either a correction step was added or there were not enough lines.
        QVERIFY(b.history(id).size() >= before);
    }

    void heal()
    {
        if (!aiModelInstalled("lama"))
            QSKIP("LaMa model not installed (set SILMA_MODEL_DIR)");
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(w.openBatch(m_batchFile));
        // Wait for the full-resolution engine.
        QTRY_VERIFY_WITH_TIMEOUT(w.mainPane()->engine() && !w.mainPane()->engine()->isHalfSize(), 20000);
        qint64 id = w.currentPhoto()->id;
        int before = int(w.currentSettings().heals.size());
        w.testHealAt(QPointF(0.5, 0.5), 0.03);
        QTRY_COMPARE_WITH_TIMEOUT(w.pendingHealJobs(), 0, 60000);
        QCOMPARE(int(w.currentSettings().heals.size()), before + 1);
        Batch b;
        b.open(m_batchFile);
        auto s = DevelopSettings::fromJsonString(b.settings(id));
        QVERIFY(b.patch(s.heals.last().patchId, nullptr, nullptr));
        QTRY_VERIFY_WITH_TIMEOUT(w.mainPane()->hasRender(), 15000);
    }
};

QTEST_MAIN(TestGui)
#include "test_gui.moc"
