#include "MainWindow.h"

#include "DevelopPanel.h"
#include "Dialogs.h"
#include "Filmstrip.h"
#include "Importer.h"
#include "Metadata.h"
#include "RawDecoder.h"

#include <QActionGroup>
#include <QApplication>
#include <QBuffer>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QThreadPool>
#include <QToolBar>
#include <QtConcurrent>

namespace silma {

namespace {
constexpr int kThumbVersionRendered = 1;
constexpr const char* kAllImports = "all";

QString exposureText(double t)
{
    if (t <= 0)
        return {};
    if (t < 0.5)
        return QStringLiteral("1/%1 s").arg(qRound(1.0 / t));
    return QStringLiteral("%1 s").arg(t, 0, 'g', 3);
}
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Silmä"));
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

    auto* central = new QWidget(this);
    auto* cl = new QHBoxLayout(central);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(2);
    m_pane2 = new ImagePane(central);
    m_pane = new ImagePane(central);
    m_pane2->hide();
    cl->addWidget(m_pane2, 1);
    cl->addWidget(m_pane, 1);
    setCentralWidget(central);
    m_pane->setMessage(tr("Create or open a batch (File menu)"));

    m_model = new PhotoListModel(this);
    m_model->setBatch(&m_batch);
    createDocks();
    createActions();

    m_status = new QLabel(this);
    statusBar()->addPermanentWidget(m_status);

    connect(&m_engines, &EngineCache::engineReady, this, &MainWindow::onEngineReady);
    connect(&m_engines, &EngineCache::loadFailed, this, [this](const QString& path, const QString& err) {
        if (m_photo && m_photo->path == path)
            m_pane->setMessage(tr("Cannot decode: %1").arg(err));
    });
    connect(m_pane, &ImagePane::rendered, this, [this](const Histogram& h, const WbValues& wb) {
        m_panel->setHistogram(h);
        m_panel->setEffectiveWb(wb);
    });
    // Keep zoom/pan in sync between the two panes.
    connect(m_pane, &ImagePane::viewStateChanged, m_pane2, &ImagePane::setViewState);
    connect(m_pane2, &ImagePane::viewStateChanged, m_pane, &ImagePane::setViewState);

    m_thumbTimer.setSingleShot(true);
    m_thumbTimer.setInterval(1500);
    connect(&m_thumbTimer, &QTimer::timeout, this, [this] {
        if (m_photo)
            renderThumbnail(m_photo->id);
    });
    m_pollTimer.setInterval(1000);
    connect(&m_pollTimer, &QTimer::timeout, this, &MainWindow::pollDatabase);
    m_pollTimer.start();

    restoreWindowState();
}

MainWindow::~MainWindow() = default;

void MainWindow::createDocks()
{
    // Left: imports and history.
    m_leftDock = new QDockWidget(tr("Imports && History"), this);
    m_leftDock->setObjectName("left");
    auto* split = new QSplitter(Qt::Vertical, m_leftDock);
    auto* importsBox = new QWidget(split);
    auto* il = new QVBoxLayout(importsBox);
    il->setContentsMargins(4, 4, 4, 4);
    il->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(tr("Imports")), importsBox));
    m_importList = new QListWidget(importsBox);
    il->addWidget(m_importList);
    auto* historyBox = new QWidget(split);
    auto* hl = new QVBoxLayout(historyBox);
    hl->setContentsMargins(4, 4, 4, 4);
    hl->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(tr("History")), historyBox));
    m_historyList = new QListWidget(historyBox);
    hl->addWidget(m_historyList);
    split->addWidget(importsBox);
    split->addWidget(historyBox);
    m_leftDock->setWidget(split);
    addDockWidget(Qt::LeftDockWidgetArea, m_leftDock);
    connect(m_importList, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        QVariant v = it->data(Qt::UserRole);
        if (v.toString() == QLatin1String(kAllImports))
            selectImport(std::nullopt, true);
        else
            selectImport(v.toLongLong(), false);
    });
    connect(m_historyList, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { jumpToHistory(it->data(Qt::UserRole).toInt()); });

    // Right: develop tools.
    m_rightDock = new QDockWidget(tr("Develop"), this);
    m_rightDock->setObjectName("right");
    m_panel = new DevelopPanel(m_rightDock);
    m_rightDock->setWidget(m_panel);
    addDockWidget(Qt::RightDockWidgetArea, m_rightDock);
    m_panel->setEnabled(false);
    connect(m_panel, &DevelopPanel::edited, this, &MainWindow::onPanelEdited);
    connect(m_panel, &DevelopPanel::autoToneRequested, this, &MainWindow::autoTone);
    connect(m_panel, &DevelopPanel::autoUprightRequested, this, &MainWindow::autoUpright);
    connect(m_panel, &DevelopPanel::autoCropRequested, this, &MainWindow::autoCrop);
    connect(m_panel, &DevelopPanel::cropToolToggled, this, &MainWindow::setCropTool);
    connect(m_panel, &DevelopPanel::sliderActive, this, [this](const QString& key, bool active) {
        QString k = active ? key : QString();
        if (k != m_activeSlider) {
            m_activeSlider = k;
            updatePanes();
        }
    });

    // Bottom: filmstrip.
    m_bottomDock = new QDockWidget(tr("Filmstrip"), this);
    m_bottomDock->setObjectName("bottom");
    m_filmstrip = new Filmstrip(m_bottomDock);
    m_filmstrip->setModel(m_model);
    m_bottomDock->setWidget(m_filmstrip);
    addDockWidget(Qt::BottomDockWidgetArea, m_bottomDock);
    connect(m_filmstrip->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& cur) {
        if (cur.isValid() && !m_restoring)
            selectPhoto(cur.data(PhotoListModel::IdRole).toLongLong());
    });
}

void MainWindow::createActions()
{
    auto* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New Batch..."), QKeySequence::New, this, &MainWindow::newBatch);
    file->addAction(tr("&Open Batch..."), QKeySequence::Open, this, &MainWindow::openBatchDialog);
    m_recentMenu = file->addMenu(tr("Open &Recent"));
    file->addSeparator();
    file->addAction(tr("&Import Photos..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I), this, &MainWindow::importPhotos);
    file->addAction(tr("&Export..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), this, &MainWindow::exportPhotos);
    file->addSeparator();
    file->addAction(tr("&Delete Rejected Photos..."), this, &MainWindow::deleteRejected);
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    auto* edit = menuBar()->addMenu(tr("&Edit"));
    m_actUndo = edit->addAction(tr("&Undo"), QKeySequence::Undo, this, &MainWindow::undo);
    m_actRedo = edit->addAction(tr("&Redo"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), this, &MainWindow::redo);
    m_actRedo->setShortcuts({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), QKeySequence(Qt::CTRL | Qt::Key_Y)});
    edit->addSeparator();
    edit->addAction(tr("&Copy Settings"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this, &MainWindow::copySettings);
    edit->addAction(tr("&Paste Settings"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), this, &MainWindow::pasteSettings);
    edit->addAction(tr("R&eset Settings"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), this, &MainWindow::resetSettings);
    edit->addSeparator();
    edit->addAction(tr("Select &All"), QKeySequence::SelectAll, m_filmstrip, &QListView::selectAll);

    auto* photo = menuBar()->addMenu(tr("&Photo"));
    photo->addAction(tr("&Reject"), QKeySequence(Qt::Key_X), this, [this] { setRejected(true); });
    photo->addAction(tr("&Unreject"), QKeySequence(Qt::Key_U), this, [this] { setRejected(false); });
    photo->addSeparator();
    photo->addAction(tr("Auto &Tone"), QKeySequence(Qt::CTRL | Qt::Key_U), this, &MainWindow::autoTone);
    photo->addAction(tr("Auto &White Balance"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_U), this, [this] {
        DevelopSettings s = m_settings;
        s.wbMode = WbMode::Auto;
        commit(s, tr("Auto White Balance"));
    });
    photo->addAction(tr("&Crop Tool"), QKeySequence(Qt::Key_R), this, [this] { setCropTool(!m_cropTool); });
    photo->addSeparator();
    photo->addAction(tr("&Next Photo"), QKeySequence(Qt::Key_Right), this, [this] { navigate(1); });
    photo->addAction(tr("&Previous Photo"), QKeySequence(Qt::Key_Left), this, [this] { navigate(-1); });

    auto* view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("Toggle &Zoom (Fit / 1:1)"), QKeySequence(Qt::Key_Z), this, [this] { m_pane->toggleZoom(m_pane->rect().center()); });
    view->addAction(tr("Zoom &In"), QKeySequence::ZoomIn, this, [this] { m_pane->zoomTo(m_pane->currentZoom() * 1.5, m_pane->rect().center()); });
    view->addAction(tr("Zoom &Out"), QKeySequence::ZoomOut, this, [this] { m_pane->zoomTo(m_pane->currentZoom() / 1.5, m_pane->rect().center()); });
    view->addSeparator();
    m_actBeforeAfter = view->addAction(tr("&Before / After"), QKeySequence(Qt::Key_Backslash), this, [this](bool on) {
        setViewMode(on ? ViewMode::BeforeAfter : ViewMode::Single);
    });
    m_actBeforeAfter->setCheckable(true);
    m_actCompare = view->addAction(tr("&Compare With Another Photo"), QKeySequence(Qt::Key_C), this, [this](bool on) {
        setViewMode(on ? ViewMode::Compare : ViewMode::Single);
    });
    m_actCompare->setCheckable(true);
    view->addSeparator();
    view->addAction(tr("Toggle Side &Panels"), QKeySequence(Qt::Key_Tab), this, &MainWindow::toggleSidePanels);
    auto addToggle = [&](QDockWidget* d, const QString& text, const QKeySequence& key) {
        QAction* a = d->toggleViewAction();
        a->setText(text);
        a->setShortcut(key);
        view->addAction(a);
    };
    addToggle(m_leftDock, tr("&Left Panel"), QKeySequence(Qt::Key_F7));
    addToggle(m_rightDock, tr("&Right Panel"), QKeySequence(Qt::Key_F8));
    addToggle(m_bottomDock, tr("&Filmstrip"), QKeySequence(Qt::Key_F6));

    auto* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&Keyboard Shortcuts"), this, [this] {
        QMessageBox::information(this, tr("Keyboard Shortcuts"),
                                 tr("<table>"
                                    "<tr><td><b>Left / Right</b></td><td>Previous / next photo</td></tr>"
                                    "<tr><td><b>X / U</b></td><td>Reject / unreject</td></tr>"
                                    "<tr><td><b>Z</b>, click</td><td>Toggle fit / 1:1 zoom; wheel zooms</td></tr>"
                                    "<tr><td><b>\\</b></td><td>Before / after</td></tr>"
                                    "<tr><td><b>C</b></td><td>Compare with another photo (select it too, or the previous one is used)</td></tr>"
                                    "<tr><td><b>R</b></td><td>Crop tool</td></tr>"
                                    "<tr><td><b>Ctrl+U</b></td><td>Auto tone</td></tr>"
                                    "<tr><td><b>Ctrl+Z / Ctrl+Shift+Z</b></td><td>Undo / redo</td></tr>"
                                    "<tr><td><b>Ctrl+Shift+C / V</b></td><td>Copy / paste settings (to all selected)</td></tr>"
                                    "<tr><td><b>Tab, F6, F7, F8</b></td><td>Hide panels</td></tr>"
                                    "</table>"));
    });
    help->addAction(tr("&About Silmä"), this, [this] {
        QMessageBox::about(this, tr("About Silmä"), tr("<b>Silmä</b> %1<br>A streamlined raw photo developer.").arg(QStringLiteral(SILMA_VERSION)));
    });
}

void MainWindow::restoreWindowState()
{
    QSettings s;
    if (!restoreGeometry(s.value("window/geometry").toByteArray()))
        resize(1500, 950);
    if (!restoreState(s.value("window/state").toByteArray())) {
        resizeDocks({m_leftDock, m_rightDock}, {230, 330}, Qt::Horizontal);
        resizeDocks({m_bottomDock}, {130}, Qt::Vertical);
    }
    updateRecentMenu();
}

// ---- batch / navigation ---------------------------------------------------------

void MainWindow::newBatch()
{
    QString dir = QSettings().value("batch/lastDir", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
    QString f = QFileDialog::getSaveFileName(this, tr("New Batch"), dir + "/" + tr("Trip") + ".silma", tr("Silmä batches (*.silma)"));
    if (f.isEmpty())
        return;
    if (!f.endsWith(QLatin1String(".silma")))
        f += QLatin1String(".silma");
    if (QFileInfo::exists(f)) {
        QMessageBox::warning(this, tr("New Batch"), tr("%1 already exists. Open it instead.").arg(f));
        return;
    }
    try {
        Batch b;
        b.create(f);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("New Batch"), QString::fromUtf8(e.what()));
        return;
    }
    if (openBatch(f))
        importPhotos();
}

void MainWindow::openBatchDialog()
{
    QString dir = QSettings().value("batch/lastDir", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
    QString f = QFileDialog::getOpenFileName(this, tr("Open Batch"), dir, tr("Silmä batches (*.silma)"));
    if (!f.isEmpty())
        openBatch(f);
}

bool MainWindow::openBatch(const QString& path)
{
    closeBatch();
    try {
        m_batch.open(path);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, tr("Open Batch"), tr("Cannot open %1:\n%2").arg(path, QString::fromUtf8(e.what())));
        return false;
    }
    QSettings s;
    s.setValue("batch/last", m_batch.file());
    s.setValue("batch/lastDir", QFileInfo(m_batch.file()).absolutePath());
    QStringList recent = s.value("batch/recent").toStringList();
    recent.removeAll(m_batch.file());
    recent.prepend(m_batch.file());
    s.setValue("batch/recent", recent.mid(0, 10));
    updateRecentMenu();
    updateTitle();
    m_dataVersion = m_batch.dataVersion();
    m_pane->setMessage(QString());
    restoreSession();
    return true;
}

void MainWindow::closeBatch()
{
    if (!m_batch.isOpen())
        return;
    saveSession();
    m_thumbTimer.stop();
    m_photo.reset();
    m_comparePhoto.reset();
    m_engines.clear();
    m_pane->setEngine(nullptr);
    m_pane2->setEngine(nullptr);
    m_model->reload(std::nullopt);
    m_model->setBatch(nullptr);
    m_batch.close();
    m_model->setBatch(&m_batch);
    m_importList->clear();
    m_historyList->clear();
    m_panel->setEnabled(false);
}

void MainWindow::updateTitle()
{
    QString t = QStringLiteral("Silmä");
    if (m_batch.isOpen())
        t = m_batch.name() + QStringLiteral(" — Silmä");
    setWindowTitle(t);
}

void MainWindow::updateRecentMenu()
{
    m_recentMenu->clear();
    for (const QString& f : QSettings().value("batch/recent").toStringList()) {
        if (!QFileInfo::exists(f))
            continue;
        m_recentMenu->addAction(f, this, [this, f] { openBatch(f); });
    }
    m_recentMenu->setEnabled(!m_recentMenu->isEmpty());
}

void MainWindow::reloadImports()
{
    QVariant cur = m_importList->currentItem() ? m_importList->currentItem()->data(Qt::UserRole) : QVariant();
    m_importList->clear();
    if (!m_batch.isOpen())
        return;
    int total = 0;
    auto imports = m_batch.imports();
    for (const auto& im : imports)
        total += im.photoCount;
    auto* all = new QListWidgetItem(tr("All Photos (%1)").arg(total), m_importList);
    all->setData(Qt::UserRole, QString::fromLatin1(kAllImports));
    for (const auto& im : imports) {
        QString text = im.rejectedCount ? tr("%1 (%2, %3 rejected)").arg(im.name).arg(im.photoCount).arg(im.rejectedCount) : tr("%1 (%2)").arg(im.name).arg(im.photoCount);
        auto* it = new QListWidgetItem(text, m_importList);
        it->setData(Qt::UserRole, im.id);
        it->setToolTip(im.path);
    }
    for (int i = 0; i < m_importList->count(); ++i)
        if (m_importList->item(i)->data(Qt::UserRole) == cur)
            m_importList->setCurrentRow(i);
}

void MainWindow::selectImport(std::optional<qint64> importId, bool all)
{
    if (!m_batch.isOpen())
        return;
    if (!all && !importId) {
        auto ims = m_batch.imports();
        if (ims.isEmpty())
            all = true;
        else
            importId = ims.first().id;
    }
    QVariant key = all ? QVariant(QString::fromLatin1(kAllImports)) : QVariant(*importId);
    for (int i = 0; i < m_importList->count(); ++i)
        if (m_importList->item(i)->data(Qt::UserRole) == key)
            m_importList->setCurrentRow(i);
    m_model->reload(all ? std::nullopt : importId);
    m_batch.setState("ui.import", all ? QString::fromLatin1(kAllImports) : QString::number(*importId));
    // Keep the current photo if it is in this list, else select the first.
    qint64 want = m_photo ? m_photo->id : 0;
    if (m_model->rowOf(want) < 0)
        want = m_model->rowCount() ? m_model->photoAt(0).id : 0;
    if (want)
        selectPhoto(want);
    else {
        m_photo.reset();
        showCurrentPhoto();
    }
}

void MainWindow::selectPhoto(qint64 id)
{
    if (m_photo && m_photo->id == id && m_pane->engine())
        return;
    // Flush a pending thumbnail update for the photo we leave.
    if (m_thumbTimer.isActive() && m_photo) {
        m_thumbTimer.stop();
        renderThumbnail(m_photo->id);
    }
    auto p = m_batch.photo(id);
    if (!p)
        return;
    if (m_photo && m_photo->id != id)
        m_previousPhotoId = m_photo->id;
    m_photo = p;
    int row = m_model->rowOf(id);
    if (row >= 0 && m_filmstrip->currentIndex().row() != row) {
        m_restoring = true;
        m_filmstrip->setCurrentIndex(m_model->index(row));
        m_restoring = false;
    }
    if (row >= 0)
        m_filmstrip->scrollTo(m_model->index(row), QAbstractItemView::PositionAtCenter);
    m_batch.setState("ui.photo", QString::number(id));
    m_settings = m_committed = DevelopSettings::fromJsonString(m_batch.settings(id));
    if (m_viewMode == ViewMode::Compare) {
        // Keep comparing against the reference; if the reference itself was selected, leave compare.
        if (m_comparePhoto && m_comparePhoto->id == id)
            setViewMode(ViewMode::Single);
    }
    showCurrentPhoto();
    // Prefetch neighbours for quick browsing.
    for (int d : {1, -1}) {
        int r = row + d;
        if (r >= 0 && r < m_model->rowCount())
            m_engines.prefetch(m_model->photoAt(r).path);
    }
}

void MainWindow::showCurrentPhoto()
{
    if (!m_photo) {
        m_pane->setEngine(nullptr);
        m_pane->setPlaceholder({});
        m_pane->setMessage(m_batch.isOpen() && m_model->rowCount() == 0 ? tr("No photos — use File › Import Photos") : QString());
        m_panel->setEnabled(false);
        m_historyList->clear();
        m_status->clear();
        return;
    }
    const PhotoInfo& p = *m_photo;
    m_panel->setEnabled(true);
    m_panel->setSettings(m_settings);
    m_panel->setPhotoInfo(QStringLiteral("ISO %1   %2 mm   f/%3   %4").arg(p.iso).arg(p.focalLength).arg(p.fNumber).arg(exposureText(p.exposureTime)));
    m_status->setText(QStringLiteral("%1   %2   %3").arg(p.fileName, p.captureTime.toString(QStringLiteral("yyyy-MM-dd HH:mm")), (p.make + ' ' + p.model).trimmed()));
    reloadHistory();
    auto engine = m_engines.get(p.path);
    QImage thumb;
    thumb.loadFromData(m_batch.thumbnail(p.id), "JPEG");
    if (!(engine && m_pane->engine() && engine->path() == m_pane->engine()->path()))
        m_pane->setPlaceholder(thumb);
    m_pane->setSettings(m_settings);
    m_pane->setEngine(engine);
    m_pane->setMessage(engine ? QString() : tr("Loading…"));
    if (engine)
        m_panel->setLensInfo(engine->lensDescription());
    updatePanes();
}

void MainWindow::navigate(int delta)
{
    if (!m_model->rowCount())
        return;
    int row = m_photo ? m_model->rowOf(m_photo->id) : -1;
    row = std::clamp(row + delta, 0, m_model->rowCount() - 1);
    selectPhoto(m_model->photoAt(row).id);
    m_filmstrip->selectionModel()->select(m_model->index(row), QItemSelectionModel::ClearAndSelect);
}

void MainWindow::onEngineReady(const QString& path)
{
    if (m_photo && m_photo->path == path) {
        auto e = m_engines.get(path);
        m_pane->setEngine(e);
        m_pane->setMessage(QString());
        if (e)
            m_panel->setLensInfo(e->lensDescription());
        if (m_viewMode == ViewMode::BeforeAfter)
            m_pane2->setEngine(e);
    }
    if (m_comparePhoto && m_comparePhoto->path == path && m_viewMode == ViewMode::Compare)
        m_pane2->setEngine(m_engines.get(path));
}

// ---- editing --------------------------------------------------------------------

void MainWindow::onPanelEdited(const DevelopSettings& s, const QString& label, bool final)
{
    if (!m_photo)
        return;
    m_settings = s;
    m_pane->setSettings(m_settings);
    if (final)
        commit(s, label);
}

void MainWindow::commit(const DevelopSettings& s, const QString& label)
{
    if (!m_photo)
        return;
    m_settings = s;
    if (s == m_committed) {
        applySettingsToUi();
        return;
    }
    m_batch.pushHistory(m_photo->id, label, s.toJsonString());
    m_committed = s;
    m_dataVersion = m_batch.dataVersion();
    applySettingsToUi();
    reloadHistory();
    scheduleThumbnail();
}

void MainWindow::applySettingsToUi()
{
    m_panel->setSettings(m_settings);
    m_pane->setSettings(m_settings);
    updatePanes();
}

void MainWindow::reloadHistory()
{
    m_historyList->clear();
    if (!m_photo)
        return;
    int pos = m_batch.historyPos(m_photo->id);
    auto h = m_batch.history(m_photo->id);
    for (int i = int(h.size()) - 1; i >= 0; --i) {
        auto* it = new QListWidgetItem(h[i].label, m_historyList);
        it->setData(Qt::UserRole, h[i].seq);
        if (h[i].seq == pos) {
            QFont f = it->font();
            f.setBold(true);
            it->setFont(f);
            m_historyList->setCurrentItem(it);
        } else if (h[i].seq > pos) {
            it->setForeground(QColor(130, 130, 130));
        }
    }
    m_actUndo->setEnabled(pos > 0);
    m_actRedo->setEnabled(pos < h.size() - 1);
}

void MainWindow::jumpToHistory(int seq)
{
    if (!m_photo)
        return;
    try {
        m_settings = m_committed = DevelopSettings::fromJsonString(m_batch.setHistoryPos(m_photo->id, seq));
    } catch (...) {
        return;
    }
    applySettingsToUi();
    reloadHistory();
    scheduleThumbnail();
}

void MainWindow::undo()
{
    if (m_photo && m_batch.historyPos(m_photo->id) > 0)
        jumpToHistory(m_batch.historyPos(m_photo->id) - 1);
}

void MainWindow::redo()
{
    if (m_photo && m_batch.historyPos(m_photo->id) < m_batch.history(m_photo->id).size() - 1)
        jumpToHistory(m_batch.historyPos(m_photo->id) + 1);
}

void MainWindow::autoTone()
{
    auto e = m_pane->engine();
    if (!e)
        return;
    commit(e->autoTone(m_settings), tr("Auto Tone"));
}

void MainWindow::autoUpright(int)
{
    statusBar()->showMessage(tr("Auto upright is not implemented yet"), 3000);
}

void MainWindow::autoCrop()
{
    statusBar()->showMessage(tr("Auto crop is not implemented yet"), 3000);
}

void MainWindow::setCropTool(bool on)
{
    m_cropTool = on;
    m_panel->setCropToolActive(on);
    statusBar()->showMessage(tr("The interactive crop tool is not implemented yet"), 3000);
}

void MainWindow::resetSettings()
{
    for (qint64 id : selectedPhotoIds()) {
        if (m_photo && id == m_photo->id)
            continue;
        m_batch.pushHistory(id, tr("Reset"), DevelopSettings().toJsonString());
        m_model->invalidateThumbnail(id);
        renderThumbnail(id);
    }
    commit(DevelopSettings(), tr("Reset"));
}

void MainWindow::copySettings()
{
    if (!m_photo)
        return;
    m_clipboard = m_committed;
    statusBar()->showMessage(tr("Settings copied"), 2000);
}

void MainWindow::pasteSettings()
{
    if (!m_clipboard)
        return;
    QList<qint64> ids = selectedPhotoIds();
    for (qint64 id : ids) {
        if (m_photo && id == m_photo->id)
            continue;
        m_batch.pushHistory(id, tr("Paste Settings"), m_clipboard->toJsonString());
        renderThumbnail(id);
    }
    if (m_photo && (ids.contains(m_photo->id) || ids.isEmpty()))
        commit(*m_clipboard, tr("Paste Settings"));
    m_model->refresh();
    statusBar()->showMessage(tr("Settings pasted to %n photo(s)", nullptr, int(std::max<qsizetype>(1, ids.size()))), 2000);
}

void MainWindow::scheduleThumbnail() { m_thumbTimer.start(); }

void MainWindow::renderThumbnail(qint64 id)
{
    auto p = m_batch.photo(id);
    if (!p)
        return;
    DevelopSettings s = DevelopSettings::fromJsonString(m_batch.settings(id));
    QString path = p->path;
    auto engine = m_engines.get(path, false);
    QPointer<MainWindow> self(this);
    QThreadPool::globalInstance()->start([self, id, s, path, engine] {
        QByteArray jpeg;
        try {
            auto e = engine ? engine : std::make_shared<DevelopEngine>(path, EngineLoadOptions{.halfSize = true});
            QSize full = e->outputSize(s);
            double k = 512.0 / std::max(full.width(), full.height());
            RenderResult r = e->render(s, {.width = std::max(1, int(full.width() * k)), .height = std::max(1, int(full.height() * k))});
            QBuffer buf(&jpeg);
            buf.open(QIODevice::WriteOnly);
            r.image.save(&buf, "JPEG", 85);
        } catch (...) {
            return;
        }
        QMetaObject::invokeMethod(qApp, [self, id, jpeg] {
            if (!self || !self->m_batch.isOpen())
                return;
            self->m_batch.setThumbnail(id, jpeg, kThumbVersionRendered);
            self->m_model->invalidateThumbnail(id);
        });
    });
}

// ---- culling / files -----------------------------------------------------------

QList<qint64> MainWindow::selectedPhotoIds() const
{
    QList<qint64> ids;
    for (const QModelIndex& i : m_filmstrip->selectionModel()->selectedIndexes())
        ids << i.data(PhotoListModel::IdRole).toLongLong();
    if (ids.isEmpty() && m_photo)
        ids << m_photo->id;
    return ids;
}

void MainWindow::setRejected(bool rejected)
{
    if (!m_batch.isOpen())
        return;
    QList<qint64> ids = selectedPhotoIds();
    for (qint64 id : ids)
        m_batch.setRejected(id, rejected);
    m_model->refresh();
    reloadImports();
    if (m_photo)
        m_photo = m_batch.photo(m_photo->id);
    statusBar()->showMessage(rejected ? tr("Rejected %n photo(s)", nullptr, int(ids.size())) : tr("Unrejected %n photo(s)", nullptr, int(ids.size())), 2000);
    // Lightroom-style: move on after rejecting a single photo.
    if (rejected && ids.size() == 1)
        navigate(1);
}

void MainWindow::importPhotos()
{
    if (!m_batch.isOpen()) {
        newBatch();
        return;
    }
    QSettings qs;
    QString dir = qs.value("import/lastDir", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
    QString filter = tr("Raw files (%1)").arg(rawFileNameFilters().join(' ')) + ";;" + tr("All files (*)");
    QStringList files = QFileDialog::getOpenFileNames(this, tr("Import Photos"), dir, filter);
    if (files.isEmpty())
        return;
    qs.setValue("import/lastDir", QFileInfo(files.first()).absolutePath());

    // Default import name: the earliest capture date.
    QList<QDateTime> dates = QtConcurrent::blockingMapped(files, [](const QString& f) {
        try {
            return readMetadataSummary(f).captureTime;
        } catch (...) {
            return QFileInfo(f).lastModified();
        }
    });
    QDateTime earliest;
    for (const auto& d : dates)
        if (d.isValid() && (!earliest.isValid() || d < earliest))
            earliest = d;
    QString defName = (earliest.isValid() ? earliest : QDateTime::currentDateTime()).toString(QStringLiteral("yyyy-MM-dd"));
    ImportDialog dlg(files, m_batch.imports(), defName, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    ImportOptions opts = dlg.options();

    auto* progress = new QProgressDialog(tr("Importing photos..."), QString(), 0, int(files.size()), this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setValue(0);
    QString batchFile = m_batch.file();
    QPointer<MainWindow> self(this);
    QPointer<QProgressDialog> pd(progress);
    auto future = QtConcurrent::run([batchFile, files, opts, pd] {
        Batch b; // own connection for the worker thread
        b.open(batchFile);
        return importFiles(b, files, opts, [pd](int done, int total, const QString& f) {
            QMetaObject::invokeMethod(qApp, [pd, done, total, f] {
                if (pd) {
                    pd->setMaximum(total);
                    pd->setValue(done);
                    pd->setLabelText(QFileInfo(f).fileName());
                }
            });
        });
    });
    auto* watcher = new QFutureWatcher<ImportResult>(this);
    connect(watcher, &QFutureWatcher<ImportResult>::finished, this, [this, watcher, progress] {
        progress->deleteLater();
        watcher->deleteLater();
        ImportResult r;
        try {
            r = watcher->result();
        } catch (const std::exception& e) {
            QMessageBox::critical(this, tr("Import"), QString::fromUtf8(e.what()));
            return;
        }
        m_dataVersion = m_batch.dataVersion();
        reloadImports();
        if (!r.importNames.isEmpty())
            if (auto im = m_batch.importByName(r.importNames.first()))
                selectImport(im->id, false);
        QString msg = tr("Imported %n photo(s)", nullptr, int(r.added.size()));
        if (!r.skipped.isEmpty())
            msg += tr("; %n duplicate(s) skipped", nullptr, int(r.skipped.size()));
        if (!r.errors.isEmpty())
            QMessageBox::warning(this, tr("Import"), msg + "\n\n" + r.errors.mid(0, 20).join('\n'));
        else
            statusBar()->showMessage(msg, 5000);
    });
    watcher->setFuture(future);
}

void MainWindow::exportPhotos()
{
    if (!m_batch.isOpen())
        return;
    QList<qint64> selected;
    for (const QModelIndex& i : m_filmstrip->selectionModel()->selectedIndexes())
        selected << i.data(PhotoListModel::IdRole).toLongLong();
    QList<PhotoInfo> all = m_batch.photos();
    ExportDialog dlg(int(selected.size()), m_model->rowCount(), int(all.size()), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QList<PhotoInfo> photos;
    switch (dlg.scope()) {
    case ExportScope::Selected:
        for (qint64 id : selected)
            if (auto p = m_batch.photo(id))
                photos << *p;
        break;
    case ExportScope::Import: photos = m_model->photos(); break;
    case ExportScope::Batch: photos = all; break;
    }
    if (!dlg.includeRejected())
        photos.erase(std::remove_if(photos.begin(), photos.end(), [](const PhotoInfo& p) { return p.rejected; }), photos.end());
    if (photos.isEmpty())
        return;
    ExportOptions o = dlg.options();
    QHash<qint64, QString> importNames;
    for (const auto& im : m_batch.imports())
        importNames[im.id] = im.name;
    struct Job {
        PhotoInfo p;
        DevelopSettings s;
        QString import;
        int seq;
    };
    QList<Job> jobs;
    for (int i = 0; i < photos.size(); ++i)
        jobs.append({photos[i], DevelopSettings::fromJsonString(m_batch.settings(photos[i].id)), importNames.value(photos[i].importId), i + 1});

    auto* progress = new QProgressDialog(tr("Exporting..."), tr("Cancel"), 0, int(jobs.size()), this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    connect(progress, &QProgressDialog::canceled, this, [cancelled] { *cancelled = true; });
    QPointer<QProgressDialog> pd(progress);
    auto done = std::make_shared<std::atomic<int>>(0);
    auto pool = std::make_shared<QThreadPool>();
    pool->setMaxThreadCount(2);
    auto future = QtConcurrent::mapped(pool.get(), jobs, [o, cancelled, done, pd, pool](const Job& j) -> QString {
        if (*cancelled)
            return QString();
        QString err;
        try {
            exportPhoto(j.p, j.s, j.import, j.seq, o);
        } catch (const std::exception& e) {
            err = j.p.fileName + ": " + QString::fromUtf8(e.what());
        }
        int n = ++*done;
        QMetaObject::invokeMethod(qApp, [pd, n] {
            if (pd)
                pd->setValue(n);
        });
        return err;
    });
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, progress, o, pool] {
        QStringList errors;
        for (const QString& e : watcher->future().results())
            if (!e.isEmpty())
                errors << e;
        progress->deleteLater();
        watcher->deleteLater();
        if (!errors.isEmpty())
            QMessageBox::warning(this, tr("Export"), errors.mid(0, 20).join('\n'));
        else
            statusBar()->showMessage(tr("Export finished: %1").arg(o.folder), 5000);
    });
    watcher->setFuture(future);
}

void MainWindow::deleteRejected()
{
    if (!m_batch.isOpen())
        return;
    QList<PhotoInfo> rejected;
    for (const auto& p : m_batch.photos())
        if (p.rejected)
            rejected << p;
    if (rejected.isEmpty()) {
        QMessageBox::information(this, tr("Delete Rejected Photos"), tr("There are no rejected photos in this batch."));
        return;
    }
    QMessageBox box(QMessageBox::Warning, tr("Delete Rejected Photos"),
                    tr("Permanently delete %n rejected photo file(s) from disk?\n\nThis cannot be undone.", nullptr, int(rejected.size())), QMessageBox::Cancel, this);
    QStringList names;
    for (const auto& p : rejected)
        names << p.path;
    box.setDetailedText(names.join('\n'));
    QPushButton* del = box.addButton(tr("Delete %n File(s)", nullptr, int(rejected.size())), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != static_cast<QAbstractButton*>(del))
        return;
    QStringList failed;
    for (const auto& p : rejected) {
        if (QFile::exists(p.path) && !QFile::remove(p.path)) {
            failed << p.path;
            continue;
        }
        m_batch.removePhoto(p.id);
        if (m_photo && m_photo->id == p.id)
            m_photo.reset();
    }
    m_dataVersion = m_batch.dataVersion();
    reloadImports();
    selectImport(m_model->importId(), !m_model->importId());
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("Delete Rejected Photos"), tr("Could not delete:\n%1").arg(failed.join('\n')));
}

// ---- view -----------------------------------------------------------------------

void MainWindow::setViewMode(ViewMode m)
{
    if (m == ViewMode::Compare) {
        // Reference: another selected photo, else the previously viewed one.
        std::optional<PhotoInfo> ref;
        for (qint64 id : selectedPhotoIds())
            if (!m_photo || id != m_photo->id) {
                ref = m_batch.photo(id);
                break;
            }
        if (!ref && m_previousPhotoId)
            ref = m_batch.photo(m_previousPhotoId);
        if (!ref) {
            statusBar()->showMessage(tr("Select a second photo to compare with"), 3000);
            m = ViewMode::Single;
        }
        m_comparePhoto = ref;
    }
    m_viewMode = m;
    m_actBeforeAfter->setChecked(m == ViewMode::BeforeAfter);
    m_actCompare->setChecked(m == ViewMode::Compare);
    updatePanes();
}

void MainWindow::updatePanes()
{
    if (!m_photo || m_viewMode == ViewMode::Single) {
        m_pane2->hide();
        m_pane->setLabel(QString());
        return;
    }
    m_pane2->show();
    if (m_viewMode == ViewMode::BeforeAfter) {
        m_pane2->setSettings(DevelopSettings());
        m_pane2->setEngine(m_pane->engine());
        m_pane2->setLabel(tr("Before"));
        m_pane->setLabel(tr("After"));
    } else if (m_comparePhoto) {
        auto e = m_engines.get(m_comparePhoto->path);
        m_pane2->setSettings(DevelopSettings::fromJsonString(m_batch.settings(m_comparePhoto->id)));
        m_pane2->setEngine(e);
        m_pane2->setLabel(m_comparePhoto->fileName);
        m_pane->setLabel(m_photo->fileName);
    }
    m_pane2->setViewState(m_pane->viewState());
}

void MainWindow::toggleSidePanels()
{
    bool visible = m_leftDock->isVisible() || m_rightDock->isVisible();
    m_leftDock->setVisible(!visible);
    m_rightDock->setVisible(!visible);
}

// ---- external changes / session ---------------------------------------------------

void MainWindow::pollDatabase()
{
    if (!m_batch.isOpen())
        return;
    qint64 v = m_batch.dataVersion();
    if (v == m_dataVersion)
        return;
    m_dataVersion = v;
    // Another process (e.g. the CLI driven by an agent) changed the batch.
    reloadImports();
    m_model->refresh();
    QString go = m_batch.state("gui.goto");
    if (!go.isEmpty()) {
        m_batch.setState("gui.goto", QString());
        m_dataVersion = m_batch.dataVersion();
        qint64 id = go.toLongLong();
        if (m_model->rowOf(id) < 0) {
            auto p = m_batch.photo(id);
            if (p)
                selectImport(p->importId, false);
        }
        selectPhoto(id);
    }
    if (m_photo) {
        auto p = m_batch.photo(m_photo->id);
        if (!p) {
            m_photo.reset();
            selectImport(m_model->importId(), !m_model->importId());
            return;
        }
        m_photo = p;
        DevelopSettings s = DevelopSettings::fromJsonString(m_batch.settings(p->id));
        if (!(s == m_committed)) {
            m_settings = m_committed = s;
            applySettingsToUi();
            scheduleThumbnail();
        }
        reloadHistory();
    }
}

void MainWindow::saveSession()
{
    if (!m_batch.isOpen())
        return;
    ViewState v = m_pane->viewState();
    m_batch.setState("ui.view", QStringLiteral("%1;%2;%3;%4").arg(v.fit).arg(v.zoom).arg(v.center.x()).arg(v.center.y()));
    m_batch.setState("ui.mode", QString::number(int(m_viewMode)));
    if (m_photo)
        m_batch.setState("ui.photo", QString::number(m_photo->id));
}

void MainWindow::restoreSession()
{
    reloadImports();
    QString im = m_batch.state("ui.import");
    qint64 photoId = m_batch.state("ui.photo").toLongLong();
    if (im == QLatin1String(kAllImports))
        selectImport(std::nullopt, true);
    else if (!im.isEmpty() && m_batch.importById(im.toLongLong()))
        selectImport(im.toLongLong(), false);
    else
        selectImport(std::nullopt, false);
    if (photoId && m_model->rowOf(photoId) >= 0)
        selectPhoto(photoId);
    auto parts = m_batch.state("ui.view").split(';');
    if (parts.size() == 4) {
        ViewState v;
        v.fit = parts[0].toInt() != 0;
        v.zoom = parts[1].toDouble();
        v.center = QPointF(parts[2].toDouble(), parts[3].toDouble());
        if (v.zoom > 0)
            m_pane->setViewState(v);
    }
    int mode = m_batch.state("ui.mode", "0").toInt();
    if (mode == int(ViewMode::BeforeAfter))
        setViewMode(ViewMode::BeforeAfter);
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (m_thumbTimer.isActive() && m_photo) {
        m_thumbTimer.stop();
        renderThumbnail(m_photo->id);
        QThreadPool::globalInstance()->waitForDone(5000);
        QCoreApplication::processEvents();
    }
    saveSession();
    QSettings s;
    s.setValue("window/geometry", saveGeometry());
    s.setValue("window/state", saveState());
    e->accept();
}

} // namespace silma
