#include "Batch.h"
#include "AutoHidePanel.h"
#include "DevelopPanel.h"
#include "Filmstrip.h"
#include "Importer.h"
#include "MainWindow.h"
#include "Widgets.h"
#include "TaskbarProgress.h"
#include <QProgressDialog>
#include <QThread>
#include "HealTool.h"
#include "Dialogs.h"
#include "DenoiseDialog.h"
#include "PanoramaDialog.h"
#include <QLineEdit>
#include <QRadioButton>
#include <QScrollBar>
#include "CropTool.h"
#include "AiDenoise.h"
#include "Derived.h"

#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QSignalSpy>
#include <QSlider>
#include <QDockWidget>
#include <QCheckBox>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
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

    // A new batch with three sample photos, for tests that must not see other tests' edits.
    QString freshBatch(const QString& name, int count = 3)
    {
        QDir d(QString(SILMA_SAMPLES) + "/2026-08-08");
        auto files = d.entryList(QDir::Files, QDir::Name).mid(0, count);
        if (files.size() < count)
            return {};
        QStringList paths;
        for (const auto& f : files)
            paths << d.filePath(f);
        QDir(m_tmp.path()).mkpath(name);
        QString file = m_tmp.filePath(name + "/" + name + ".silma");
        Batch b;
        b.create(file);
        return importFiles(b, paths, {}).added.size() == count ? file : QString();
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        m_batchFile = freshBatch("gui");
        if (m_batchFile.isEmpty())
            QSKIP("samples not available");
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
        // The collapsed panels' edge strips get their own space: the pinned filmstrip
        // sits between them instead of under them.
        QDockWidget* leftStrip = w.autoHidePanel(Qt::LeftEdge)->triggerDock();
        QDockWidget* rightStrip = right->triggerDock();
        auto* filmDock = w.findChild<QDockWidget*>("bottom");
        QTRY_VERIFY(leftStrip->isVisible() && rightStrip->isVisible());
        QTRY_VERIFY(filmDock->geometry().left() > leftStrip->geometry().right());
        QVERIFY(filmDock->geometry().right() < rightStrip->geometry().left());
        QVERIFY(!w.mainPane()->geometry().translated(w.mainPane()->parentWidget()->mapTo(&w, QPoint())).intersects(leftStrip->geometry()));

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
        QVERIFY(!leftStrip->isVisible() && !rightStrip->isVisible());
        QVERIFY(!QSettings().value("ui/autoHide/" + QString::number(int(Qt::RightEdge))).toBool());
    }

    void sliderInteraction()
    {
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.openBatch(m_batchFile));
        ParamSlider* contrast = slider(w, "contrast");
        QVERIFY(contrast);
        QSlider* qs = contrast->findChild<QSlider*>();
        QSignalSpy edits(contrast, &ParamSlider::edited);
        // A click at 80% of the groove sets about +60 (range -100..100), not a page step.
        QTest::mouseClick(qs, Qt::LeftButton, {}, QPoint(int(qs->width() * 0.8), qs->height() / 2));
        QTRY_VERIFY(!edits.isEmpty());
        QVERIFY2(contrast->value() > 45 && contrast->value() < 75, qPrintable(QString::number(contrast->value())));

        // Focusing the number of a transform parameter makes it active (guide lines).
        QSignalSpy active(w.developPanel(), &DevelopPanel::sliderActive);
        ParamSlider* vertical = slider(w, "vertical");
        auto* spin = vertical->findChild<QDoubleSpinBox*>();
        spin->setFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(!active.isEmpty());
        QCOMPARE(active.last().at(0).toString(), QString("vertical"));
        QCOMPARE(active.last().at(1).toBool(), true);
        contrast->findChild<QDoubleSpinBox*>()->setFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(active.size() >= 2);
        bool verticalOff = false;
        for (const auto& args : active)
            if (args.at(0).toString() == "vertical" && !args.at(1).toBool())
                verticalOff = true;
        QVERIFY(verticalOff);
        w.testUndo();
    }

    void filmstripSelectionAndMenu()
    {
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.openBatch(m_batchFile));
        auto* strip = w.findChild<Filmstrip*>();
        QVERIFY(strip && strip->model()->rowCount() >= 3);
        auto* delegate = static_cast<FilmstripDelegate*>(strip->itemDelegate());
        QVERIFY(!delegate->multiSelection);
        strip->selectAll();
        QVERIFY(delegate->multiSelection);

        // Right-clicking a photo outside the selection selects just it; the menu offers the photo actions.
        strip->clearSelection();
        strip->setCurrentIndex(strip->model()->index(0, 0));
        QRect r1 = strip->visualRect(strip->model()->index(1, 0));
        QStringList texts;
        auto grabMenu = [&] {
            QTimer::singleShot(200, [&] {
                if (auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                    for (QAction* a : m->actions())
                        if (!a->isSeparator())
                            texts << a->text();
                    m->close();
                }
            });
        };
        grabMenu();
        QTest::mouseClick(strip->viewport(), Qt::RightButton, {}, r1.center());
        emit strip->customContextMenuRequested(r1.center());
        QCOMPARE(strip->selectionModel()->selectedIndexes().size(), 1);
        QVERIFY(texts.contains(QStringLiteral("&Reject")));
        QVERIFY(texts.contains(QStringLiteral("Show in &File Manager")));
        QVERIFY(!texts.contains(QStringLiteral("Merge to &HDR...")));

        // With several selected, the menu acts on all of them.
        strip->selectAll();
        texts.clear();
        grabMenu();
        emit strip->customContextMenuRequested(r1.center());
        QVERIFY(texts.contains(QStringLiteral("Merge to &HDR...")));
        QVERIFY(texts.contains(QStringLiteral("&Export %1 Photos...").arg(strip->model()->rowCount())));
    }

    void filmstripEditedMarkerAndNoDragSelect()
    {
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.openBatch(freshBatch("filmstripEditedMarkerAndNoDragSelect")));
        auto* strip = w.findChild<Filmstrip*>();
        QVERIFY(strip && strip->model()->rowCount() >= 3);
        const QModelIndex last = strip->model()->index(2, 0);
        strip->setCurrentIndex(last);
        QTRY_COMPARE(w.currentPhoto()->id, last.data(PhotoListModel::IdRole).toLongLong());
        QVERIFY(!last.data(PhotoListModel::EditedRole).toBool());

        // The marker follows edits and undo right away.
        ParamSlider* exp = slider(w, "exposure");
        emit exp->edited(0.7, true);
        QVERIFY(last.data(PhotoListModel::EditedRole).toBool());
        w.testUndo();
        QVERIFY(!last.data(PhotoListModel::EditedRole).toBool());

        // Dragging across thumbnails does not extend the selection.
        QPoint a = strip->visualRect(strip->model()->index(0, 0)).center(), b = strip->visualRect(last).center();
        QTest::mousePress(strip->viewport(), Qt::LeftButton, {}, a);
        for (int i = 1; i <= 10; ++i)
            QTest::mouseMove(strip->viewport(), a + (b - a) * i / 10);
        QTest::mouseRelease(strip->viewport(), Qt::LeftButton, {}, b);
        QCOMPARE(strip->selectionModel()->selectedIndexes().size(), 1);
        // Shift+click still selects a range.
        QTest::mouseClick(strip->viewport(), Qt::LeftButton, Qt::ShiftModifier, b);
        QCOMPARE(strip->selectionModel()->selectedIndexes().size(), 3);
    }

    void filmstripDoesNotJumpOnClick()
    {
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.openBatch(freshBatch("filmstripDoesNotJumpOnClick", 10)));
        auto* strip = w.findChild<Filmstrip*>();
        auto* bar = strip->horizontalScrollBar();
        QTRY_VERIFY(bar->maximum() > 0);
        bar->setValue(bar->maximum() / 2);
        const int at = bar->value();
        // Click and Ctrl+click visible photos at the ends: no scrolling to centre them.
        auto visibleRows = [&] {
            QList<int> rows;
            for (int r = 0; r < strip->model()->rowCount(); ++r)
                if (strip->viewport()->rect().contains(strip->visualRect(strip->model()->index(r, 0))))
                    rows << r;
            return rows;
        };
        const QList<int> rows = visibleRows();
        QVERIFY(rows.size() >= 3);
        QVERIFY(!rows.isEmpty());
        QTest::mouseClick(strip->viewport(), Qt::LeftButton, {}, strip->visualRect(strip->model()->index(rows.first(), 0)).center());
        QTRY_COMPARE(w.currentPhoto()->id, strip->model()->index(rows.first(), 0).data(PhotoListModel::IdRole).toLongLong());
        QCOMPARE(bar->value(), at);
        QTest::mouseClick(strip->viewport(), Qt::LeftButton, Qt::ControlModifier, strip->visualRect(strip->model()->index(rows.last(), 0)).center());
        QCOMPARE(bar->value(), at);
    }

    void filmstripFollowsTurnedThumbnail()
    {
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.openBatch(freshBatch("filmstripFollowsTurnedThumbnail")));
        QTRY_VERIFY_WITH_TIMEOUT(w.mainPane()->hasRender(), 15000);
        auto* strip = w.findChild<Filmstrip*>();
        const int row = strip->currentIndex().row();
        QVERIFY(row + 1 < strip->model()->rowCount());
        const QModelIndex idx = strip->model()->index(row, 0), next = strip->model()->index(row + 1, 0);
        auto thumb = [&] { return idx.data(Qt::DecorationRole).value<QPixmap>().size(); };
        // The photo after the turned one follows right after its slot, whatever its width.
        auto gap = [&] { return strip->visualRect(next).left() - strip->visualRect(idx).right() - 1; };
        QTRY_VERIFY(thumb().width() > thumb().height());
        const int gap0 = gap();
        QVERIFY(strip->visualRect(idx).width() > strip->visualRect(idx).height());
        // Turned to portrait: the slot narrows and the strip closes up right away (not only
        // after the next database poll happens to lay it out).
        w.testTurn(1);
        QTRY_VERIFY_WITH_TIMEOUT(thumb().width() < thumb().height(), 15000);
        QTest::qWait(100);
        QVERIFY(strip->visualRect(idx).width() < strip->visualRect(idx).height());
        QCOMPARE(gap(), gap0);
        w.testTurn(-1);
        QTRY_VERIFY_WITH_TIMEOUT(thumb().width() > thumb().height(), 15000);
        QTest::qWait(100);
        QCOMPARE(gap(), gap0);
    }

    void deleteAndExcludePhotos()
    {
        const QString file = freshBatch("deleteAndExclude", 4);
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(w.openBatch(file));
        QTRY_VERIFY_WITH_TIMEOUT(w.mainPane()->hasRender(), 15000);
        auto* strip = w.findChild<Filmstrip*>();
        QList<PhotoInfo> photos;
        for (int r = 0; r < strip->model()->rowCount(); ++r)
            photos << static_cast<PhotoListModel*>(strip->model())->photoAt(r);
        QCOMPARE(photos.size(), 4);
        // Answers confirmations in turn with the buttons labelled `texts` (empty: Cancel);
        // stops when the list is used up or the removal returns.
        auto answering = std::make_shared<QTimer>();
        auto answer = [answering](QStringList texts) {
            answering->disconnect();
            answering->setInterval(20);
            QObject::connect(answering.get(), &QTimer::timeout, [answering, texts]() mutable {
                auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                if (!box || texts.isEmpty())
                    return;
                const QString text = texts.takeFirst();
                for (QAbstractButton* b : box->buttons())
                    if (b->text() == text || (text.isEmpty() && box->buttonRole(b) == QMessageBox::RejectRole)) {
                        b->click();
                        return;
                    }
                box->reject();
            });
            answering->start();
        };
        auto ids = [&] {
            QList<qint64> l;
            for (int r = 0; r < strip->model()->rowCount(); ++r)
                l << static_cast<PhotoListModel*>(strip->model())->photoAt(r).id;
            return l;
        };
        // Only that photo selected (whatever modifier state earlier tests left).
        auto selectOnly = [&](qint64 id) {
            w.testSelectPhoto(id);
            const int r = static_cast<PhotoListModel*>(strip->model())->rowOf(id);
            strip->selectionModel()->select(strip->model()->index(r, 0), QItemSelectionModel::ClearAndSelect);
            QCOMPARE(w.testSelectedIds(), QList<qint64>{id});
        };
        selectOnly(photos[1].id);
        // Cancel keeps everything.
        answer({QString()});
        w.testRemovePhotos(false);
        QCOMPARE(ids().size(), 4);
        // Exclude: gone from the batch, file kept, the next photo becomes current.
        answer({tr("Exclude from Batch")});
        w.testRemovePhotos(false);
        QCOMPARE(ids(), (QList<qint64>{photos[0].id, photos[2].id, photos[3].id}));
        QVERIFY(QFile::exists(photos[1].path));
        QCOMPARE(w.currentPhoto()->id, photos[2].id);
        // Delete the last photo: its file goes to the trash (or, where there is none, is
        // deleted after a second question), the one before becomes current.
        selectOnly(photos[3].id);
        answer({tr("Move to Trash"), tr("Delete Permanently")});
        w.testRemovePhotos(false);
        answering->stop();
        QCOMPARE(ids(), (QList<qint64>{photos[0].id, photos[2].id}));
        QVERIFY(!QFile::exists(photos[3].path));
        QCOMPARE(w.currentPhoto()->id, photos[2].id);
        QTRY_VERIFY_WITH_TIMEOUT(w.mainPane()->hasRender(), 15000);
        // Don't leave test files in the trash.
        for (const QString& t : w.trashedFiles()) {
            const QFileInfo fi(t);
            QVERIFY(QFile::remove(t));
            QFile::remove(fi.dir().filePath("../info/" + fi.fileName() + ".trashinfo"));
        }
        Batch b;
        b.open(file);
        QCOMPARE(b.photos().size(), 2);
    }

    void denoisePreview()
    {
        if (!aiModelInstalled("rawnind"))
            QSKIP("RawNIND model not installed (set SILMA_MODEL_DIR)");
        Batch b;
        b.open(m_batchFile);
        const PhotoInfo p = b.photos().first();
        const DevelopSettings s = DevelopSettings::fromJsonString(b.settings(p.id));
        DenoiseDialog dlg("rawnind", p.path, s, {}, 2, 1);
        dlg.show();
        // Pixel-level variation of an image (difference to the right neighbour).
        auto detail = [](const QImage& img) {
            const QImage g = img.convertToFormat(QImage::Format_Grayscale8);
            double d = 0;
            for (int y = 0; y < g.height(); ++y) {
                const uchar* l = g.constScanLine(y);
                for (int x = 0; x + 1 < g.width(); ++x)
                    d += std::abs(int(l[x + 1]) - int(l[x]));
            }
            return d / (double(g.width()) * g.height());
        };
        dlg.setStrength(100);
        QTRY_VERIFY_WITH_TIMEOUT(dlg.previewReady(), 60000);
        const QImage before = dlg.originalImage(), full = dlg.previewImage();
        QVERIFY(!before.isNull());
        QCOMPARE(full.size(), before.size());
        QVERIFY2(detail(full) < 0.8 * detail(before), qPrintable(QStringLiteral("%1 vs %2").arg(detail(full)).arg(detail(before))));
        // Weaker: in between (no new model run, only blending).
        dlg.setStrength(30);
        QTRY_VERIFY_WITH_TIMEOUT(dlg.previewReady(), 20000);
        const double mid = detail(dlg.previewImage());
        QVERIFY(mid > detail(full) && mid < detail(before) * 1.05);
        // Another region: a new original and denoised image there.
        const QPointF c = dlg.regionCenter();
        dlg.setRegionCenter(QPointF(0.15, 0.85));
        QVERIFY(dlg.regionCenter() != c);
        QVERIFY(!dlg.previewReady());
        QTRY_VERIFY_WITH_TIMEOUT(dlg.previewReady(), 60000);
        QVERIFY(dlg.originalImage() != before);
        QVERIFY(detail(dlg.previewImage()) < detail(dlg.originalImage()));
        QCOMPARE(dlg.strength(), 30);
        // Dragging the preview to the left looks further right; a click without moving
        // keeps the region.
        auto* view = dlg.findChild<QWidget*>("preview");
        QVERIFY(view);
        const QPointF c2 = dlg.regionCenter();
        QTest::mousePress(view, Qt::LeftButton, {}, QPoint(300, 200));
        QTest::mouseMove(view, QPoint(200, 200));
        QTest::mouseRelease(view, Qt::LeftButton, {}, QPoint(200, 200));
        QVERIFY(dlg.regionCenter().x() > c2.x());
        QCOMPARE(dlg.regionCenter().y(), c2.y());
        QTRY_VERIFY_WITH_TIMEOUT(dlg.previewReady(), 60000);
        const QPointF c3 = dlg.regionCenter();
        QTest::mouseClick(view, Qt::LeftButton, {}, QPoint(100, 100));
        QCOMPARE(dlg.regionCenter(), c3);
        QVERIFY(dlg.previewReady());
    }

    void panoramaPreview()
    {
        // DSC_0010..0012 of 2026-08-08 are a handheld sequence.
        QDir d(QString(SILMA_SAMPLES) + "/2026-08-08");
        if (!d.exists("DSC_0012.NEF"))
            QSKIP("samples not available");
        QDir(m_tmp.path()).mkpath("pano");
        Batch b;
        b.create(m_tmp.filePath("pano/pano.silma"));
        const QList<PanoramaSource> photos = panoramaSources(b, importFiles(b, {d.filePath("DSC_0010.NEF"), d.filePath("DSC_0011.NEF"), d.filePath("DSC_0012.NEF")}, {}).added);
        QCOMPARE(photos.size(), 3);
        PanoramaDialog dlg(photos);
        dlg.show();
        QVERIFY(!dlg.previewDone());
        QTRY_VERIFY_WITH_TIMEOUT(dlg.previewDone(), 60000);
        QVERIFY2(dlg.previewError().isEmpty(), qPrintable(dlg.previewError()));
        const QImage toned = dlg.previewImage();
        QVERIFY(toned.width() > toned.height() && toned.width() >= 1000);
        const QRectF crop = dlg.previewCrop();
        QVERIFY(crop.width() > 0.7 && crop.width() < 1 && crop.height() > 0.6);
        // Options: auto tone switches the preview's rendering; both go to the merge.
        auto* tone = dlg.findChildren<QCheckBox*>().last();
        QCOMPARE(tone->text(), tr("Auto tone"));
        tone->setChecked(false);
        QVERIFY(dlg.previewImage() != toned);
        QVERIFY(!dlg.options().autoTone && dlg.options().autoCrop);
    }

    void progressStaysOpenAndShowsInTaskbar()
    {
        MainWindow w;
        w.resize(1000, 700);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.openBatch(freshBatch("progressStaysOpen")));
        // A job with a step that completes, then a busy step: the dialog stays up throughout.
        std::atomic<int> stage = 0;
        w.testRunDerivedJob("Test job", [&stage](Batch&, const auto& progress, const std::atomic<bool>*) {
            progress("Step one", 4, 4);
            stage = 1;
            QThread::msleep(600);
            progress("Writing", 0, 0);
            QThread::msleep(600);
            stage = 2;
            return QList<qint64>{};
        });
        QPointer<QProgressDialog> pd;
        QTRY_VERIFY((pd = w.findChild<QProgressDialog*>()) && stage == 1);
        QTRY_VERIFY(pd && pd->value() == 4);
        // The taskbar entry follows the dialog (polled).
        QTRY_VERIFY(std::abs(TaskbarProgress::instance().shownProgress() - 1.0) < 1e-6);
        QTest::qWait(300);
        QVERIFY(pd && pd->isVisible());
        QTRY_COMPARE(stage.load(), 2);
        QTRY_VERIFY(!w.findChild<QProgressDialog*>());
        QTRY_VERIFY(TaskbarProgress::instance().shownProgress() < 0);

        // Background jobs as a count on the entry.
        TaskbarProgress::instance().setCount(2);
        QCOMPARE(TaskbarProgress::instance().shownCount(), 2);
        TaskbarProgress::instance().setCount(0);
        QCOMPARE(TaskbarProgress::instance().shownCount(), 0);
    }

    void jpegPhotoDefaults()
    {
        // A JPEG gets its own defaults (no sharpening): shown, and what double-click and Reset give.
        const QString file = freshBatch("jpegPhotoDefaults");
        QImage img(300, 200, QImage::Format_RGB888);
        img.fill(QColor(90, 140, 200));
        const QString jpg = QFileInfo(file).absolutePath() + "/phone.jpg";
        QVERIFY(img.save(jpg, "JPEG", 90));
        {
            Batch b;
            b.open(file);
            QCOMPARE(importFiles(b, {jpg}, {.importName = b.imports().first().name}).added.size(), 1);
        }
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.openBatch(file));
        auto* strip = w.findChild<Filmstrip*>();
        int row = -1;
        for (int r = 0; r < strip->model()->rowCount(); ++r)
            if (strip->model()->index(r, 0).data().toString() == "phone.jpg")
                row = r;
        QVERIFY(row >= 0);
        strip->setCurrentIndex(strip->model()->index(row, 0));
        QTRY_COMPARE(w.currentPhoto()->fileName, QStringLiteral("phone.jpg"));
        ParamSlider* sharpen = slider(w, "sharpen_amount");
        QVERIFY(sharpen);
        QCOMPARE(sharpen->value(), 0.0);
        emit sharpen->edited(60, true);
        QCOMPARE(w.currentSettings().sharpenAmount, 60.0);
        QTest::mouseDClick(sharpen->findChild<QSlider*>(), Qt::LeftButton);
        QCOMPARE(w.currentSettings().sharpenAmount, 0.0);
        emit slider(w, "exposure")->edited(0.5, true);
        w.testResetSettings();
        QCOMPARE(w.currentSettings(), DevelopSettings::defaults(true));
    }

    void derivedPhotoKeepsSelection()
    {
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        const QString file = freshBatch("derivedPhotoKeepsSelection");
        QVERIFY(w.openBatch(file));
        auto* strip = w.findChild<Filmstrip*>();
        const QModelIndex second = strip->model()->index(1, 0);
        QTest::mouseClick(strip->viewport(), Qt::LeftButton, {}, strip->visualRect(second).center());
        const qint64 picked = second.data(PhotoListModel::IdRole).toLongLong();
        QTRY_COMPARE(w.currentPhoto()->id, picked);
        // A derived photo job (its own connection) adds a photo right after the picked one; the GUI polls.
        { Batch b; b.open(file); createLinearCopy(b, picked); }
        w.testPoll();
        QCOMPARE(strip->model()->rowCount(), 4);
        // The derived photo is marked; the tooltip names its source.
        QCOMPARE(strip->model()->index(1, 0).data(PhotoListModel::IdRole).toLongLong(), picked);
        const QModelIndex derived = strip->model()->index(2, 0);
        QCOMPARE(derived.data(PhotoListModel::DerivedRole).toString(), QStringLiteral("DNG"));
        QVERIFY(strip->model()->index(1, 0).data(PhotoListModel::DerivedRole).toString().isEmpty());
        QVERIFY(derived.data(Qt::ToolTipRole).toString().endsWith(strip->model()->index(1, 0).data().toString()));
        if (qEnvironmentVariableIsSet("SILMA_SHOT"))
            strip->grab().save(qEnvironmentVariable("SILMA_SHOT"));
        // Focus coming back to the strip (e.g. a dialog closes) must not move to another photo.
        w.developPanel()->setFocus();
        strip->setFocus(Qt::ActiveWindowFocusReason);
        QCOMPARE(strip->currentIndex().data(PhotoListModel::IdRole).toLongLong(), picked);
        QCOMPARE(w.currentPhoto()->id, picked);
        QCOMPARE(w.testSelectedIds(), QList<qint64>{picked});
    }

    void pasteSettingsDialog()
    {
        QSettings().remove("paste/excluded");
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.openBatch(freshBatch("pasteSettingsDialog")));
        auto* strip = w.findChild<Filmstrip*>();
        QVERIFY(strip && strip->model()->rowCount() >= 3);
        const QModelIndex src = strip->model()->index(1, 0), dst = strip->model()->index(2, 0);
        strip->setCurrentIndex(src);
        QTRY_COMPARE(w.currentPhoto()->id, src.data(PhotoListModel::IdRole).toLongLong());
        emit slider(w, "exposure")->edited(1.25, true);
        emit slider(w, "contrast")->edited(40, true);
        w.testCopySettings();

        // Paste to the other photo with only Exposure checked.
        strip->setCurrentIndex(dst);
        QTRY_COMPARE(w.currentPhoto()->id, dst.data(PhotoListModel::IdRole).toLongLong());
        bool seen = false;
        QTimer::singleShot(100, [&] {
            auto* dlg = qobject_cast<PasteSettingsDialog*>(QApplication::activeModalWidget());
            if (!dlg)
                return;
            seen = true;
            QVERIFY(!dlg->parts().contains("crop")); // off by default
            QVERIFY(dlg->parts().contains("contrast"));
            dlg->setParts({"exposure"});
            dlg->accept();
        });
        w.testPasteSettings();
        QVERIFY(seen);
        QCOMPARE(w.currentSettings().exposure, 1.25);
        QCOMPARE(w.currentSettings().contrast, 0.0);
        // Paste (Previous Choice) reuses it without asking.
        const QModelIndex third = strip->model()->index(0, 0);
        strip->setCurrentIndex(third);
        QTRY_COMPARE(w.currentPhoto()->id, third.data(PhotoListModel::IdRole).toLongLong());
        const double contrastBefore = w.currentSettings().contrast;
        bool asked = false;
        QTimer guard;
        guard.setSingleShot(true);
        QObject::connect(&guard, &QTimer::timeout, [&] {
            if (auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                asked = true;
                d->reject();
            }
        });
        guard.start(300);
        w.testPasteSettings(false);
        QVERIFY(!asked);
        QCOMPARE(w.currentSettings().exposure, 1.25);
        QCOMPARE(w.currentSettings().contrast, contrastBefore);
        // The choice is remembered.
        PasteSettingsDialog again(1);
        QCOMPARE(again.parts(), QSet<QString>{"exposure"});
        // Check All / Check None, and a section's checkbox sets all its settings.
        auto button = [&](const QString& text) {
            for (auto* b : again.findChildren<QPushButton*>())
                if (b->text() == text)
                    return b;
            return static_cast<QPushButton*>(nullptr);
        };
        button("Check &All")->click();
        QCOMPARE(again.parts().size(), qsizetype(settingsParts().size()));
        button("Check &None")->click();
        QVERIFY(again.parts().isEmpty());
        auto* tree = again.findChild<QTreeWidget*>();
        QTreeWidgetItem* tone = tree->findItems("Tone", Qt::MatchExactly).value(0);
        QVERIFY(tone && tone->childCount() == 6);
        tone->setCheckState(0, Qt::Checked);
        QCOMPARE(again.parts(), (QSet<QString>{"exposure", "contrast", "highlights", "shadows", "whites", "blacks"}));
        QSettings().remove("paste/excluded");
    }

    void loupeCursorAndDistortionGuides()
    {
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QVERIFY(w.openBatch(m_batchFile));
        ImagePane* pane = w.mainPane();
        QTRY_VERIFY_WITH_TIMEOUT(pane->engine() != nullptr, 15000);

        // A loupe: "+" while fitted, "-" when zoomed in (different pixmaps).
        QCOMPARE(pane->cursor().shape(), Qt::BitmapCursor);
        const QImage zoomIn = pane->cursor().pixmap().toImage();
        pane->toggleZoom(pane->rect().center());
        QCOMPARE(pane->cursor().shape(), Qt::BitmapCursor);
        QVERIFY(pane->cursor().pixmap().toImage() != zoomIn);
        pane->toggleZoom(pane->rect().center());
        QCOMPARE(pane->cursor().pixmap().toImage(), zoomIn);
        // Tools own the cursor.
        w.testCropTool(true);
        QVERIFY(pane->cursor().shape() != Qt::BitmapCursor);
        w.testCropTool(false);
        QCOMPARE(pane->cursor().shape(), Qt::BitmapCursor);

        // Adjusting Distortion shows the straight-line guides, like Transform.
        QVERIFY(!pane->hasOverlay());
        emit slider(w, "lens_distortion")->activeChanged(true);
        QVERIFY(pane->hasOverlay());
        emit slider(w, "lens_distortion")->activeChanged(false);
        QVERIFY(!pane->hasOverlay());
    }

    void whiteBalancePicker()
    {
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        const QString batchFile = freshBatch("whiteBalancePicker");
        QVERIFY(w.openBatch(batchFile));
        QTRY_VERIFY_WITH_TIMEOUT(w.mainPane()->engine() != nullptr, 15000);
        const qint64 id = w.currentPhoto()->id;

        // Esc cancels the picker.
        w.testWbPicker(true);
        QVERIFY(w.wbPickerActive());
        QVERIFY(w.mainPane()->tool() != nullptr);
        QTest::keyClick(&w, Qt::Key_Escape);
        QVERIFY(!w.wbPickerActive());
        QVERIFY(w.mainPane()->tool() == nullptr);

        // A click on a usable point sets a custom white balance and ends the tool.
        w.testWbPicker(true);
        auto expected = w.mainPane()->engine()->wbFromPoint(w.currentSettings(), {0.5, 0.5});
        QVERIFY(expected);
        QPointF at = w.mainPane()->imageToWidget({0.5, 0.5});
        QTest::mouseClick(w.mainPane(), Qt::LeftButton, {}, at.toPoint());
        QVERIFY(!w.wbPickerActive());
        QCOMPARE(w.currentSettings().wbMode, WbMode::Custom);
        QVERIFY(std::abs(w.currentSettings().temperature - expected->temperature) <= 25);
        Batch b;
        b.open(batchFile);
        QCOMPARE(b.history(id).last().label, QStringLiteral("White Balance: Picked"));
    }

    void importDialogLateName()
    {
        ImportInfo existing;
        existing.name = QStringLiteral("2026-08-08");
        {
            // The capture dates arrive after the dialog opened: an existing import with that name is preselected.
            ImportDialog dlg({QStringLiteral("a.nef")}, {existing}, QString());
            QVERIFY(dlg.options().importName.isEmpty()); // automatic (capture date) until then
            dlg.setDefaultName(QStringLiteral("2026-08-08"));
            QCOMPARE(dlg.options().importName, QStringLiteral("2026-08-08"));
            QVERIFY(dlg.findChildren<QRadioButton*>().at(1)->isChecked());
        }
        {
            // A name the user typed meanwhile is kept.
            ImportDialog dlg({QStringLiteral("a.nef")}, {existing}, QString());
            auto* name = dlg.findChild<QLineEdit*>();
            name->setFocus();
            QTest::keyClicks(name, "Trip");
            dlg.setDefaultName(QStringLiteral("2026-08-08"));
            QCOMPARE(dlg.options().importName, QStringLiteral("Trip"));
        }
    }

    void editUndoRejectRestore()
    {
        qint64 firstId = 0;
        {
            MainWindow w;
            w.resize(1400, 900);
            w.show();
            QVERIFY(w.openBatch(m_batchFile));
            // Start from the first photo, whichever an earlier test viewed last.
            auto* strip = w.findChild<Filmstrip*>();
            strip->setCurrentIndex(strip->model()->index(0, 0));
            QVERIFY(w.currentPhoto());
            firstId = w.currentPhoto()->id;
            QCOMPARE(firstId, strip->model()->index(0, 0).data(PhotoListModel::IdRole).toLongLong());
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

    void turnAndResetTransform()
    {
        const QString file = freshBatch("turn");
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QVERIFY(w.openBatch(file));
        QTRY_VERIFY_WITH_TIMEOUT(w.mainPane()->hasRender(), 15000);
        const qint64 id = w.currentPhoto()->id;
        DevelopSettings s = w.currentSettings();
        s.perspVertical = 20;
        s.rotate = 1.5;
        s.crop = QRectF(0.1, 0.1, 0.5, 0.5);
        w.testCommit(s, "Edit");
        const QSizeF canvas = w.mainPane()->engine()->geometry(w.currentSettings(), true).canvasSize();
        w.testTurn(1);
        QCOMPARE(w.currentSettings().quarterTurns, 1);
        QCOMPARE(w.currentSettings().perspHorizontal, -20.0);
        QCOMPARE(w.mainPane()->engine()->geometry(w.currentSettings(), true).canvasSize(), canvas.transposed());
        // Reset Transform clears the corrections and the crop, but keeps the turn.
        w.testResetTransform();
        QCOMPARE(w.currentSettings().perspVertical, 0.0);
        QCOMPARE(w.currentSettings().perspHorizontal, 0.0);
        QCOMPARE(w.currentSettings().rotate, 0.0);
        QCOMPARE(w.currentSettings().crop, QRectF(0, 0, 1, 1));
        QCOMPARE(w.currentSettings().quarterTurns, 1);
        // So does Reset Settings; Rotate Left turns back. The batch holds the result.
        w.testResetSettings();
        QCOMPARE(w.currentSettings().quarterTurns, 1);
        w.testTurn(-1);
        Batch b;
        b.open(file);
        QCOMPARE(DevelopSettings::fromJsonString(b.settings(id)).quarterTurns, 0);
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

        // A stroke: stored with its path; the patch covers just the stroke (plus feather).
        w.testHealStroke({QPointF(0.3, 0.6), QPointF(0.35, 0.62), QPointF(0.4, 0.6)}, 0.01);
        QTRY_COMPARE_WITH_TIMEOUT(w.pendingHealJobs(), 0, 60000);
        s = DevelopSettings::fromJsonString(b.settings(id));
        QCOMPARE(s.heals.last().path.size(), 2);
        QRect r;
        QVERIFY(b.patch(s.heals.last().patchId, &r, nullptr));
        const int sensorLong = std::max(w.mainPane()->engine()->raw().fullWidth, w.mainPane()->engine()->raw().fullHeight);
        // About 10% + 2.5% brush and feather of the long edge wide, far below the 3x context.
        QVERIFY2(r.width() < sensorLong * 0.16 && r.height() < sensorLong * 0.08, qPrintable(QStringLiteral("%1x%2").arg(r.width()).arg(r.height())));
    }

    void healToolStroke()
    {
        // Drawing with the tool (no model needed: the request is checked, not generated).
        HealTool tool;
        Geometry g(GeometrySource{6000, 4000, 1, 0, nullptr, nullptr}, DevelopSettings());
        tool.setSpots({}, g, 6000);
        tool.setBrushSize(0.01);
        QList<QPointF> got;
        double gotRadius = 0;
        connect(&tool, &HealTool::healRequested, [&](const QList<QPointF>& s, double r) {
            got = s;
            gotRadius = r;
        });
        ImagePane pane;
        QMouseEvent ev(QEvent::MouseButtonPress, QPointF(), QPointF(), Qt::LeftButton, Qt::LeftButton, {});
        // A click: a round spot.
        tool.press(pane, {0.5, 0.5}, &ev);
        tool.release(pane, {0.5, 0.5}, &ev);
        QCOMPARE(got.size(), 1);
        QCOMPARE(gotRadius, 0.01);
        // A drag: a stroke.
        tool.press(pane, {0.2, 0.5}, &ev);
        for (int i = 1; i <= 20; ++i)
            tool.move(pane, {0.2 + i * 0.005, 0.5}, &ev);
        tool.release(pane, {0.3, 0.5}, &ev);
        QVERIFY(got.size() > 5);
        QCOMPARE(got.last(), QPointF(0.3, 0.5));
        // A long drag stops at the size cap (20% of the long edge, brush included).
        tool.press(pane, {0.1, 0.5}, &ev);
        for (int i = 1; i <= 80; ++i)
            tool.move(pane, {0.1 + i * 0.01, 0.5}, &ev);
        tool.release(pane, {0.9, 0.5}, &ev);
        const double extent = got.last().x() - got.first().x() + 2 * 0.01;
        QVERIFY2(extent <= 0.2 + 1e-9 && extent > 0.18, qPrintable(QString::number(extent)));
    }
};

QTEST_MAIN(TestGui)
#include "test_gui.moc"
