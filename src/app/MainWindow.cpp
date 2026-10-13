#include "MainWindow.h"

#include "AutoHidePanel.h"
#include "CameraProfile.h"
#include "ColorScheme.h"
#include "CropTool.h"
#include "Heal.h"
#include "HealTool.h"
#include "WbPickerTool.h"
#include "MetadataView.h"
#include "DevelopPanel.h"
#include "Upright.h"
#include "Versions.h"
#include "AiDenoise.h"
#include "Derived.h"
#include "TaskbarProgress.h"
#include "RenderedImage.h"
#include "Merge.h"
#include "DenoiseDialog.h"
#include "MergeDialog.h"
#include "Dialogs.h"
#include "Filmstrip.h"
#include "Importer.h"
#include "Metadata.h"
#include "RawDecoder.h"

#include <QAbstractButton>
#include <QActionGroup>
#include <QApplication>
#include <QBuffer>
#include <QCloseEvent>
#include <QDockWidget>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFormLayout>
#include <QSpinBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QThreadPool>
#include <omp.h>
#include <QToolBar>
#include <QToolButton>
#include <QtConcurrent>
#ifdef SILMA_HAVE_KCOLORSCHEME
#include <kcolorscheme_version.h>
#endif

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

namespace {
// Opens the files' folder with the files selected (freedesktop FileManager1, as Dolphin
// and most file managers implement), or just the folder.
void showInFileManager(const QStringList& paths)
{
    if (paths.isEmpty())
        return;
    QStringList urls;
    for (const QString& p : paths)
        urls << QUrl::fromLocalFile(p).toString();
    QDBusMessage m = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("/org/freedesktop/FileManager1"),
                                                    QStringLiteral("org.freedesktop.FileManager1"), QStringLiteral("ShowItems"));
    m << urls << QString();
    if (QDBusConnection::sessionBus().call(m, QDBus::Block, 3000).type() != QDBusMessage::ReplyMessage)
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(paths.first()).absolutePath()));
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

    m_cropToolObj = new CropTool(this);
    connect(m_cropToolObj, &CropTool::rectChanged, this, [this](const QRectF& r, bool final) {
        m_settings.crop = r;
        if (final)
            commit(m_settings, tr("Crop"));
    });
    connect(m_cropToolObj, &CropTool::rotationChanged, this, [this](double deg, bool final) {
        DevelopSettings s = m_settings;
        s.rotate = deg;
        if (final) {
            commit(s, tr("Rotate %1°").arg(deg), QStringLiteral("rotate"));
            return;
        }
        m_settings = withConstrainedCrop(s);
        m_cropToolObj->setRect(m_settings.crop);
        m_pane->setSettings(m_settings);
    });

    m_healToolObj = new HealTool(this);
    m_healToolObj->setBrushSize(m_panel->healSize());
    connect(m_healToolObj, &HealTool::healRequested, this, &MainWindow::healAt);
    connect(m_healToolObj, &HealTool::removeRequested, this, [this](int i) {
        DevelopSettings s = m_settings;
        if (i >= 0 && i < s.heals.size()) {
            s.heals.removeAt(i);
            commit(s, tr("Remove Heal Spot"));
        }
    });
    connect(m_panel, &DevelopPanel::healToolToggled, this, &MainWindow::setHealTool);

    m_wbPickerObj = new WbPickerTool(this);
    connect(m_wbPickerObj, &WbPickerTool::picked, this, &MainWindow::pickWhiteBalance);
    connect(m_panel, &DevelopPanel::wbPickerToggled, this, &MainWindow::setWbPicker);
    connect(m_panel, &DevelopPanel::healSizeChanged, this, [this](double r) {
        m_healToolObj->setBrushSize(r);
        m_pane->update();
    });
    connect(m_panel, &DevelopPanel::healClearRequested, this, [this] {
        DevelopSettings s = m_settings;
        if (!s.heals.isEmpty()) {
            s.heals.clear();
            commit(s, tr("Clear Heal"));
        }
    });

    m_healProgress = new QProgressBar(this);
    m_healProgress->setRange(0, 0); // indeterminate
    m_healProgress->setMaximumWidth(120);
    m_healProgress->setMaximumHeight(statusBar()->fontMetrics().height());
    m_healProgress->setTextVisible(false);
    m_healProgress->hide();
    statusBar()->addPermanentWidget(m_healProgress);
    m_healAnim.setInterval(40);
    connect(&m_healAnim, &QTimer::timeout, this, [this] {
        m_healToolObj->tick();
        if (m_healTool)
            m_pane->update();
    });
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
    // The restored state may show edge strips of panels that are pinned now.
    for (auto edge : {Qt::LeftEdge, Qt::RightEdge, Qt::BottomEdge})
        autoHidePanel(edge)->triggerDock()->hide();
    for (auto it = m_autoHideActions.begin(); it != m_autoHideActions.end(); ++it)
        it.value()->setChecked(QSettings().value(QStringLiteral("ui/autoHide/%1").arg(it.key()), false).toBool());
}

MainWindow::~MainWindow()
{
    // Child widgets are destroyed after our members and can still emit signals then
    // (e.g. a focused slider spin box losing focus): make sure none reaches us.
    for (QObject* o : findChildren<QObject*>())
        disconnect(o, nullptr, this, nullptr);
}

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
    connect(m_panel, &DevelopPanel::installProfilesRequested, this, &MainWindow::installCameraProfiles);
    connect(m_panel, &DevelopPanel::autoUprightRequested, this, &MainWindow::autoUpright);
    connect(m_panel, &DevelopPanel::autoCropRequested, this, &MainWindow::autoCrop);
    connect(m_panel, &DevelopPanel::transformResetRequested, this, &MainWindow::resetTransform);
    connect(m_panel, &DevelopPanel::turnRequested, this, &MainWindow::turnPhotos);
    connect(m_panel, &DevelopPanel::cropToolToggled, this, &MainWindow::setCropTool);
    connect(m_panel, &DevelopPanel::aiDenoiseRequested, this, &MainWindow::aiDenoise);
    connect(m_panel, &DevelopPanel::sliderActive, this, [this](const QString& key, bool active) {
        QString k = active ? key : QString();
        if (k != m_activeSlider) {
            m_activeSlider = k;
            updatePanes();
        }
    });

    // Metadata (tabbed with the left panel, hidden by default).
    m_metaDock = new QDockWidget(tr("Metadata"), this);
    m_metaDock->setObjectName("metadata");
    m_metaView = new MetadataView(m_metaDock);
    m_metaDock->setWidget(m_metaView);
    addDockWidget(Qt::LeftDockWidgetArea, m_metaDock);
    tabifyDockWidget(m_leftDock, m_metaDock);
    m_metaDock->hide();
    m_leftDock->raise();
    connect(m_metaDock, &QDockWidget::visibilityChanged, this, [this](bool v) {
        if (v && m_photo)
            m_metaView->showFile(m_photo->path);
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

    // Overlays used when a panel is set to auto hide & show.
    m_autoLeft = new AutoHidePanel(Qt::LeftEdge, this);
    m_autoRight = new AutoHidePanel(Qt::RightEdge, this);
    m_autoBottom = new AutoHidePanel(Qt::BottomEdge, this);
    m_autoRight->setTitle(tr("Develop"));
    m_autoBottom->setTitle(tr("Filmstrip"));
    for (auto edge : {Qt::LeftEdge, Qt::RightEdge, Qt::BottomEdge})
        connect(autoHidePanel(edge), &AutoHidePanel::pinRequested, this, [this, edge] { setAutoHide(edge, false); });
    for (auto [dock, edge] : {std::pair{m_leftDock, Qt::LeftEdge}, {m_metaDock, Qt::LeftEdge}, {m_rightDock, Qt::RightEdge}, {m_bottomDock, Qt::BottomEdge}}) {
        auto* bar = new DockTitleBar(dock);
        dock->setTitleBarWidget(bar);
        connect(bar, &DockTitleBar::autoHideRequested, this, [this, edge] { setAutoHide(edge, true); });
    }
    connect(m_autoLeft, &AutoHidePanel::revealed, this, [this] {
        if (m_photo && m_metaView->isVisible())
            m_metaView->showFile(m_photo->path);
    });
    for (QDockWidget* d : {m_leftDock, m_rightDock, m_bottomDock, m_metaDock, m_autoLeft->triggerDock(), m_autoRight->triggerDock(), m_autoBottom->triggerDock()})
        d->installEventFilter(this);
}

void MainWindow::createActions()
{
    auto* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New Batch..."), QKeySequence::New, this, &MainWindow::newBatch);
    file->addAction(tr("&Open Batch..."), QKeySequence::Open, this, &MainWindow::openBatchDialog);
    m_recentMenu = file->addMenu(tr("Open &Recent"));
    file->addSeparator();
    file->addAction(tr("&Import Photos..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I), this, &MainWindow::importPhotos);
    QAction* actExport = file->addAction(tr("&Export..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), this, &MainWindow::exportPhotos);
    file->addSeparator();
    file->addAction(tr("&Delete Rejected Photos..."), this, &MainWindow::deleteRejected);
    file->addSeparator();
    file->addAction(tr("Install &Camera Profiles..."), this, &MainWindow::installCameraProfiles);
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    auto* edit = menuBar()->addMenu(tr("&Edit"));
    m_actUndo = edit->addAction(tr("&Undo"), QKeySequence::Undo, this, &MainWindow::undo);
    m_actRedo = edit->addAction(tr("&Redo"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), this, &MainWindow::redo);
    m_actRedo->setShortcuts({QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), QKeySequence(Qt::CTRL | Qt::Key_Y)});
    edit->addSeparator();
    QAction* actCopy = edit->addAction(tr("&Copy Settings"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this, &MainWindow::copySettings);
    QAction* actPaste = edit->addAction(tr("&Paste Settings..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V), this, [this] { pasteSettings(true); });
    QAction* actPasteAgain = edit->addAction(tr("Paste Settings (&Previous Choice)"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_V), this,
                                             [this] { pasteSettings(false); });
    QAction* actReset = edit->addAction(tr("R&eset Settings"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), this, &MainWindow::resetSettings);
    edit->addSeparator();
    QAction* actRemove = edit->addAction(tr("&Delete Photos..."), QKeySequence(Qt::Key_Delete), this, [this] { removePhotos(false); });
    QAction* actExclude = edit->addAction(tr("E&xclude from Batch..."), this, [this] { removePhotos(true); });
    edit->addSeparator();
    QAction* actSelectAll = edit->addAction(tr("Select &All"), QKeySequence::SelectAll, m_filmstrip, &QListView::selectAll);

    auto* photo = menuBar()->addMenu(tr("&Photo"));
    QAction* actReject = photo->addAction(tr("&Reject"), QKeySequence(Qt::Key_X), this, [this] { setRejected(true); });
    QAction* actUnreject = photo->addAction(tr("&Unreject"), QKeySequence(Qt::Key_U), this, [this] { setRejected(false); });
    photo->addSeparator();
    photo->addAction(tr("Auto &Tone"), QKeySequence(Qt::CTRL | Qt::Key_U), this, &MainWindow::autoTone);
    photo->addAction(tr("Auto &White Balance"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_U), this, [this] {
        DevelopSettings s = m_settings;
        s.wbMode = WbMode::Auto;
        commit(s, tr("Auto White Balance"));
    });
    photo->addAction(tr("White Balance &Picker"), QKeySequence(Qt::Key_W), this, [this] { setWbPicker(!m_wbPicker); });
    auto* esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, [this] {
        if (m_wbPicker)
            setWbPicker(false);
        if (m_cropTool)
            cancelCropTool();
    });
    // Enter closes the crop tool keeping its changes (only while it is open: elsewhere Enter
    // belongs to the focused field).
    for (int i = 0; i < 2; ++i) {
        m_cropAccept[i] = new QShortcut(QKeySequence(i == 0 ? Qt::Key_Return : Qt::Key_Enter), this);
        m_cropAccept[i]->setEnabled(false);
        connect(m_cropAccept[i], &QShortcut::activated, this, [this] { setCropTool(false); });
    }
    photo->addAction(tr("&Crop Tool"), QKeySequence(Qt::Key_R), this, [this] { setCropTool(!m_cropTool); });
    QAction* actTurnCcw = photo->addAction(tr("Rotate &Left (90° CCW)"), QKeySequence(Qt::CTRL | Qt::Key_BracketLeft), this, [this] { turnPhotos(-1); });
    QAction* actTurnCw = photo->addAction(tr("Rotate Ri&ght (90° CW)"), QKeySequence(Qt::CTRL | Qt::Key_BracketRight), this, [this] { turnPhotos(1); });
    photo->addAction(tr("AI &Heal Tool"), QKeySequence(Qt::Key_Q), this, [this] { setHealTool(!m_healTool); });
    photo->addSeparator();
    QAction* actDenoise = photo->addAction(tr("AI &Denoise..."), this, &MainWindow::aiDenoise);
    QAction* actHdr = photo->addAction(tr("Merge to &HDR..."), QKeySequence(Qt::CTRL | Qt::Key_H), this, [this] { mergePhotos(true); });
    QAction* actPano = photo->addAction(tr("Merge to Pa&norama..."), QKeySequence(Qt::CTRL | Qt::Key_M), this, [this] { mergePhotos(false); });
    photo->addSeparator();
    photo->addAction(tr("&Next Photo"), QKeySequence(Qt::Key_Right), this, [this] { navigate(1); });
    photo->addAction(tr("&Previous Photo"), QKeySequence(Qt::Key_Left), this, [this] { navigate(-1); });

    auto* view = menuBar()->addMenu(tr("&View"));
    QAction* actToggleZoom = view->addAction(tr("Toggle &Zoom (Fit / 1:1)"), this, [this] { m_pane->toggleZoom(m_pane->rect().center()); });
    // Space too, unless a button or combo box has focus (see eventFilter).
    actToggleZoom->setShortcuts({QKeySequence(Qt::Key_Z), QKeySequence(Qt::Key_Space)});
    qApp->installEventFilter(this);
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
    view->addAction(tr("Toggle Side &Panels"), QKeySequence(Qt::CTRL | Qt::Key_Tab), this, &MainWindow::toggleSidePanels);
    auto addToggle = [&](QDockWidget* d, const QString& text, const QKeySequence& key) {
        QAction* a = d->toggleViewAction();
        a->setText(text);
        a->setShortcut(key);
        view->addAction(a);
    };
    addToggle(m_leftDock, tr("&Left Panel"), QKeySequence(Qt::Key_F7));
    addToggle(m_rightDock, tr("&Right Panel"), QKeySequence(Qt::Key_F8));
    addToggle(m_bottomDock, tr("&Filmstrip"), QKeySequence(Qt::Key_F6));
    QAction* meta = m_metaDock->toggleViewAction();
    meta->setText(tr("&Metadata"));
    meta->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    view->addAction(meta);
    connect(meta, &QAction::triggered, this, [this](bool on) {
        if (on)
            m_metaDock->raise();
    });
    view->addSeparator();
    auto addAutoHide = [&](Qt::Edge edge, const QString& text, const QKeySequence& key) {
        QAction* a = view->addAction(text);
        a->setCheckable(true);
        a->setShortcut(key);
        a->setToolTip(tr("Hide the panel and show it while the mouse is at the window edge"));
        connect(a, &QAction::toggled, this, [this, edge](bool on) { setAutoHide(edge, on); });
        m_autoHideActions.insert(int(edge), a);
    };
    addAutoHide(Qt::LeftEdge, tr("Auto-Hide Le&ft Panel"), QKeySequence(Qt::SHIFT | Qt::Key_F7));
    addAutoHide(Qt::RightEdge, tr("Auto-Hide Ri&ght Panel"), QKeySequence(Qt::SHIFT | Qt::Key_F8));
    addAutoHide(Qt::BottomEdge, tr("Auto-Hide F&ilmstrip"), QKeySequence(Qt::SHIFT | Qt::Key_F6));
    view->addSeparator();
    view->addMenu(createColorSchemeMenu(view));

    // Filmstrip context menu: the same actions, for the photo(s) under the mouse.
    m_filmstrip->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_filmstrip, &QWidget::customContextMenuRequested, this, [=, this](const QPoint& pos) {
        QMenu menu(this);
        // Copies, so items can be left out here without touching the main menu.
        auto add = [&menu](QAction* a, const QString& text = {}) {
            QAction* c = menu.addAction(text.isEmpty() ? a->text() : text, a, &QAction::trigger);
            c->setShortcut(a->shortcut());
            return c;
        };
        // Right-clicking a photo selects it (unless it is part of the selection already).
        const QModelIndex at = m_filmstrip->indexAt(pos);
        const QList<qint64> ids = selectedPhotoIds();
        if (!at.isValid() || ids.isEmpty()) {
            add(actSelectAll);
            menu.addAction(tr("&Import Photos..."), this, &MainWindow::importPhotos);
        } else {
            const int n = int(ids.size());
            bool anyRejected = false, anyKept = false;
            for (qint64 id : ids) {
                int r = m_model->rowOf(id);
                (r >= 0 && m_model->photoAt(r).rejected ? anyRejected : anyKept) = true;
            }
            if (anyKept)
                add(actReject, n > 1 ? tr("&Reject %n Photos", nullptr, n) : QString());
            if (anyRejected)
                add(actUnreject, n > 1 ? tr("&Unreject %n Photos", nullptr, n) : QString());
            menu.addSeparator();
            if (n == 1)
                add(actCopy);
            add(actPaste, n > 1 ? tr("&Paste Settings to %n Photos...", nullptr, n) : QString())->setEnabled(m_clipboard.has_value());
            add(actPasteAgain)->setEnabled(m_clipboard.has_value());
            add(actReset, n > 1 ? tr("R&eset Settings of %n Photos", nullptr, n) : QString());
            menu.addSeparator();
            add(actTurnCcw);
            add(actTurnCw);
            menu.addSeparator();
            add(actDenoise);
            if (n > 1) {
                add(actHdr);
                add(actPano);
            }
            add(actExport, n > 1 ? tr("&Export %n Photos...", nullptr, n) : QString());
            menu.addSeparator();
            add(actRemove, n > 1 ? tr("&Delete %n Photos...", nullptr, n) : QString());
            add(actExclude, n > 1 ? tr("E&xclude %n Photos from Batch...", nullptr, n) : QString());
            menu.addSeparator();
            const QString path = at.data(PhotoListModel::PathRole).toString();
            menu.addAction(tr("Show in &File Manager"), this, [path] { showInFileManager({path}); });
            menu.addAction(tr("Copy File Pa&th"), this, [path] { QGuiApplication::clipboard()->setText(path); });
        }
        menu.exec(m_filmstrip->viewport()->mapToGlobal(pos));
    });

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
                                    "<tr><td><b>W</b></td><td>White balance picker (click a neutral point)</td></tr>"
                                    "<tr><td><b>Q</b></td><td>AI heal: click a spot or drag along something; Alt+click removes</td></tr>"
                                    "<tr><td><b>Ctrl+U</b></td><td>Auto tone</td></tr>"
                                    "<tr><td><b>Ctrl+[ / Ctrl+]</b></td><td>Rotate 90° left / right (all selected)</td></tr>"
                                    "<tr><td><b>Ctrl+Z / Ctrl+Shift+Z</b></td><td>Undo / redo</td></tr>"
                                    "<tr><td><b>Ctrl+Shift+C / V</b></td><td>Copy / paste settings (to all selected)</td></tr>"
                                    "<tr><td><b>Ctrl+Alt+Shift+V</b></td><td>Paste the same settings as last time, without asking</td></tr>"
                                    "<tr><td><b>Ctrl+Tab, F6, F7, F8</b></td><td>Hide panels</td></tr>"
                                    "<tr><td><b>Shift+F6, F7, F8</b></td><td>Auto hide &amp; show panels</td></tr>"
                                    "</table>"));
    });
    help->addAction(tr("&About Silmä"), this, [this] {
        QString libs = QStringLiteral("<tr><td>Qt</td><td>%1</td></tr>").arg(QString::fromLatin1(qVersion()));
#ifdef SILMA_HAVE_KCOLORSCHEME
        libs += QStringLiteral("<tr><td>KDE Frameworks (KColorScheme)</td><td>%1</td></tr>").arg(QStringLiteral(KCOLORSCHEME_VERSION_STRING));
#endif
        for (const auto& [name, version] : libraryVersions())
            libs += QStringLiteral("<tr><td>%1</td><td>%2</td></tr>").arg(name, version);
        QString models;
        for (const AiModelInfo& m : aiDenoiseModels() + QList<AiModelInfo>{aiInpaintModel()})
            models += QStringLiteral("<tr><td>%1</td><td>%2, %3%4</td></tr>")
                          .arg(m.name, m.license, m.about, aiModelInstalled(m.id) ? QString() : tr(" (not downloaded)"));
        QMessageBox box(this);
        box.setWindowTitle(tr("About Silmä"));
        box.setTextFormat(Qt::RichText);
        box.setText(tr("<h3>Silmä %1</h3>"
                       "<p>A streamlined raw photo developer.<br>"
                       "By Claude Opus 5.5, steered by Alexander Ulyanov.<br>"
                       "Free software under the GNU GPL, version 3 or later.</p>"
                       "<p><b>Libraries</b></p><table cellspacing=\"6\">%2</table>"
                       "<p><b>AI models</b></p><table cellspacing=\"6\">%3</table>")
                        .arg(QStringLiteral(SILMA_VERSION), libs, models));
        box.setTextInteractionFlags(Qt::TextBrowserInteraction); // clickable links
        box.setIconPixmap(windowIcon().pixmap(64, 64));
        box.exec();
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
    m_thumbQueue.clear();
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

void MainWindow::selectImport(std::optional<qint64> importId, bool all, qint64 preferredPhoto)
{
    m_knownHistory.clear();
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
    for (const auto& p : m_model->photos())
        m_knownHistory[p.id] = p.historyPos;
    m_batch.setState("ui.import", all ? QString::fromLatin1(kAllImports) : QString::number(*importId));
    // Keep the current photo if it is in this list, else select the first.
    qint64 want = m_photo ? m_photo->id : preferredPhoto;
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
    m_navBackwards = false;
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
    // Scroll only as far as needed (centring would make the strip jump on every click).
    if (row >= 0)
        m_filmstrip->scrollTo(m_model->index(row), QAbstractItemView::EnsureVisible);
    m_batch.setState("ui.photo", QString::number(id));
    m_panel->setDefaults(DevelopSettings::defaults(p->kind == PhotoKind::Rendered));
    if (m_cropTool)
        setCropTool(false);
    if (m_healTool)
        setHealTool(false);
    if (m_wbPicker)
        setWbPicker(false);
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
    if (m_metaView->isVisible())
        m_metaView->showFile(p.path);
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
    ensureHealPatches(engine.get(), m_settings);
    m_pane->setSettings(m_settings);
    m_pane->setEngine(engine);
    // A failed decode is remembered (and reported) once: show it again when coming back.
    const QString error = engine ? QString() : m_engines.errorFor(p.path);
    m_pane->setMessage(engine ? QString() : error.isEmpty() ? tr("Loading…") : tr("Cannot decode: %1").arg(error));
    if (engine) {
        m_panel->setLensInfo(engine->lensDescription());
        m_panel->setProfiles(engine->profileChoices());
    }
    updatePanes();
}

void MainWindow::installCameraProfiles()
{
    QSettings qs;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Install Camera Profiles"), qs.value("profiles/lastDir", QDir::homePath()).toString(),
        tr("Camera profiles (*.dcp *.DCP *.xmp *.XMP)"));
    if (files.isEmpty())
        return;
    qs.setValue("profiles/lastDir", QFileInfo(files.first()).absolutePath());
    QStringList errors;
    const QStringList names = installProfiles(files, &errors);
    QString text = names.isEmpty() ? tr("No profiles were installed.") : tr("Installed:\n%1").arg(names.join('\n'));
    if (!errors.isEmpty())
        text += tr("\n\nSkipped:\n%1").arg(errors.join('\n'));
    const QStringList missing = looksMissingBase();
    if (!missing.isEmpty())
        text += tr("\n\nNot usable yet:\n%1\n(Camera Raw keeps the camera profiles in CameraRaw/CameraProfiles/Adobe Standard.)").arg(missing.join('\n'));
    text += tr("\n\nProfiles are kept in %1").arg(profileDir());
    if (errors.isEmpty() && missing.isEmpty())
        QMessageBox::information(this, tr("Install Camera Profiles"), text);
    else
        QMessageBox::warning(this, tr("Install Camera Profiles"), text);
    if (m_photo)
        if (auto e = m_engines.get(m_photo->path)) {
            m_panel->setProfiles(e->profileChoices());
            m_pane->refresh();
        }
}

void MainWindow::navigate(int delta)
{
    if (!m_model->rowCount())
        return;
    int row = m_photo ? m_model->rowOf(m_photo->id) : -1;
    row = std::clamp(row + delta, 0, m_model->rowCount() - 1);
    selectPhoto(m_model->photoAt(row).id);
    m_filmstrip->selectionModel()->select(m_model->index(row), QItemSelectionModel::ClearAndSelect);
    m_navBackwards = delta < 0;
}

void MainWindow::onEngineReady(const QString& path)
{
    if (m_photo && m_photo->path == path) {
        auto e = m_engines.get(path);
        ensureHealPatches(e.get(), m_settings);
        m_pane->setEngine(e);
        if (m_healTool)
            updateHealOverlay();
        m_pane->setMessage(QString());
        if (e) {
            m_panel->setLensInfo(e->lensDescription());
            m_panel->setProfiles(e->profileChoices());
        }
        if (m_viewMode == ViewMode::BeforeAfter)
            m_pane2->setEngine(e);
    }
    if (m_comparePhoto && m_comparePhoto->path == path && m_viewMode == ViewMode::Compare)
        m_pane2->setEngine(m_engines.get(path));
}

// ---- editing --------------------------------------------------------------------

void MainWindow::onPanelEdited(const DevelopSettings& s, const QString& label, bool final, const QString& mergeKey)
{
    if (!m_photo)
        return;
    if (final) {
        commit(s, label, mergeKey);
        return;
    }
    m_settings = withConstrainedCrop(s);
    m_pane->setSettings(m_settings);
}

DevelopSettings MainWindow::withConstrainedCrop(const DevelopSettings& s) const
{
    auto e = m_pane->engine();
    return e ? constrainedCrop(*e, m_committed, s) : s;
}

void MainWindow::commit(const DevelopSettings& edited, const QString& label, const QString& mergeKey)
{
    if (!m_photo)
        return;
    const DevelopSettings s = withConstrainedCrop(edited);
    m_settings = s;
    if (s == m_committed) {
        applySettingsToUi();
        return;
    }
    const qint64 id = m_photo->id;
    auto& last = m_lastMergeable;
    const bool merge = !mergeKey.isEmpty() && mergeKey == last.key && id == last.photoId && last.since.isValid()
                       && last.since.elapsed() < 1500
                       // Not after an undo, or a change from elsewhere (the CLI).
                       && m_batch.historyPos(id) == last.seq && m_batch.dataVersion() == m_dataVersion;
    if (merge)
        m_batch.replaceHistory(id, label, s.toJsonString());
    else
        last.seq = m_batch.pushHistory(id, label, s.toJsonString());
    last.key = mergeKey;
    last.photoId = id;
    last.since.start();
    m_committed = s;
    m_dataVersion = m_batch.dataVersion();
    historyChanged(m_photo->id);
    applySettingsToUi();
    reloadHistory();
    scheduleThumbnail();
}

void MainWindow::applySettingsToUi()
{
    ensureHealPatches(m_pane->engine().get(), m_settings);
    if (m_healTool)
        updateHealOverlay();
    if (m_cropTool) {
        m_cropToolObj->setRect(m_settings.crop);
        m_cropToolObj->setLockAspect(m_settings.cropLockAspect);
        m_cropToolObj->setRotation(m_settings.rotate);
        m_pane->update();
    }
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
    historyChanged(m_photo->id);
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

void MainWindow::autoUpright(int mode)
{
    auto e = m_pane->engine();
    if (!e)
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    UprightResult r = silma::autoUpright(*e, m_settings, UprightMode(mode));
    QApplication::restoreOverrideCursor();
    if (!r.ok) {
        statusBar()->showMessage(tr("Upright: not enough straight lines found"), 4000);
        return;
    }
    DevelopSettings s = m_settings;
    s.perspVertical = r.vertical;
    s.perspHorizontal = r.horizontal;
    s.rotate = r.rotate;
    const char* names[] = {"Vertical", "Horizontal", "Both"};
    commit(s, tr("Upright (%1)").arg(tr(names[std::clamp(mode, 0, 2)])));
}

void MainWindow::autoCrop()
{
    auto e = m_pane->engine();
    if (!e)
        return;
    DevelopSettings s = m_settings;
    Geometry g = e->geometry(s, true);
    QSizeF cs = g.canvasSize();
    double aspect = cs.width() / cs.height();
    if (s.cropLockAspect && s.crop != QRectF(0, 0, 1, 1))
        aspect = s.crop.width() * cs.width() / (s.crop.height() * cs.height());
    else if (g.hasValidMask())
        aspect = 0; // panorama: the largest covered area, whatever its shape
    s.crop = autoCropRect(g, aspect);
    commit(s, tr("Auto Crop"));
}

void MainWindow::setCropTool(bool on)
{
    if (on && !m_pane->engine())
        on = false;
    if (on && m_healTool)
        setHealTool(false);
    if (on && m_wbPicker)
        setWbPicker(false);
    if (on && !m_cropTool)
        m_cropEntryPos = m_batch.historyPos(m_photo->id);
    m_cropTool = on;
    for (QShortcut* s : m_cropAccept)
        s->setEnabled(on);
    m_panel->setCropToolActive(on);
    if (on) {
        m_cropToolObj->setRect(m_settings.crop);
        m_cropToolObj->setLockAspect(m_settings.cropLockAspect);
        m_cropToolObj->setRotation(m_settings.rotate);
        m_cropToolObj->setCanvasSize(m_pane->engine()->geometry(m_settings, true).canvasSize());
        m_pane->setViewState(ViewState{});
    }
    m_pane->setIgnoreCrop(on);
    m_pane->setTool(on ? m_cropToolObj : nullptr);
}

void MainWindow::cancelCropTool()
{
    // The tool's edits are committed as they are made: back to where it was opened (they
    // stay available as redo steps).
    const int pos = m_cropEntryPos;
    setCropTool(false);
    if (m_photo && pos >= 0 && m_batch.historyPos(m_photo->id) > pos)
        jumpToHistory(pos);
}

void MainWindow::resetSettings()
{
    for (qint64 id : selectedPhotoIds()) {
        if (m_photo && id == m_photo->id)
            continue;
        auto ph = m_batch.photo(id);
        DevelopSettings s = newPhotoSettings(ph && ph->kind == PhotoKind::Rendered);
        s.quarterTurns = DevelopSettings::fromJsonString(m_batch.settings(id)).quarterTurns; // a fix for the photo, not an edit
        m_batch.pushHistory(id, tr("Reset"), s.toJsonString());
        historyChanged(id);
        m_model->invalidateThumbnail(id);
        renderThumbnail(id);
    }
    DevelopSettings s = newPhotoSettings(m_photo && m_photo->kind == PhotoKind::Rendered);
    s.quarterTurns = m_settings.quarterTurns;
    commit(s, tr("Reset"));
}

void MainWindow::turnPhotos(int quarterTurnsCw)
{
    const QString label = quarterTurnsCw > 0 ? tr("Rotate Right") : tr("Rotate Left");
    const QList<qint64> ids = selectedPhotoIds();
    for (qint64 id : ids) {
        if (m_photo && id == m_photo->id)
            continue;
        const DevelopSettings s = turnedSettings(DevelopSettings::fromJsonString(m_batch.settings(id)), quarterTurnsCw);
        m_batch.pushHistory(id, label, s.toJsonString());
        historyChanged(id);
        m_model->invalidateThumbnail(id);
        renderThumbnail(id);
    }
    if (m_photo && (ids.contains(m_photo->id) || ids.isEmpty()))
        commit(turnedSettings(m_settings, quarterTurnsCw), label);
}

void MainWindow::resetTransform()
{
    if (!m_photo)
        return;
    DevelopSettings s = m_settings;
    const DevelopSettings def;
    s.perspVertical = def.perspVertical;
    s.perspHorizontal = def.perspHorizontal;
    s.rotate = def.rotate;
    s.scale = def.scale;
    s.crop = def.crop;
    commit(s, tr("Reset Transform"));
}

void MainWindow::copySettings()
{
    if (!m_photo)
        return;
    m_clipboard = m_committed;
    statusBar()->showMessage(tr("Settings copied"), 2000);
}

void MainWindow::pasteSettings(bool ask)
{
    if (!m_clipboard)
        return;
    QList<qint64> ids = selectedPhotoIds();
    QSet<QString> parts = PasteSettingsDialog::savedParts();
    if (ask) {
        PasteSettingsDialog dlg(int(std::max<qsizetype>(1, ids.size())), this);
        if (dlg.exec() != QDialog::Accepted)
            return;
        parts = dlg.parts();
    }
    for (qint64 id : ids) {
        if (m_photo && id == m_photo->id)
            continue;
        const DevelopSettings before = DevelopSettings::fromJsonString(m_batch.settings(id));
        const DevelopSettings after = copySettingsParts(before, *m_clipboard, parts);
        if (after == before)
            continue;
        m_batch.pushHistory(id, tr("Paste Settings"), after.toJsonString());
        historyChanged(id);
        renderThumbnail(id);
    }
    if (m_photo && (ids.contains(m_photo->id) || ids.isEmpty()))
        commit(copySettingsParts(m_committed, *m_clipboard, parts), tr("Paste Settings"));
    m_model->refresh();
    statusBar()->showMessage(tr("Settings pasted to %n photo(s)", nullptr, int(std::max<qsizetype>(1, ids.size()))), 2000);
}

void MainWindow::scheduleThumbnail() { m_thumbTimer.start(); }

void MainWindow::historyChanged(qint64 id)
{
    const int pos = m_batch.historyPos(id);
    m_model->setHistoryPos(id, pos);
    m_knownHistory[id] = pos; // our own change: not one to pick up from another process
}

// Thumbnail updates run one or two at a time with few threads each, so that pasting settings to
// hundreds of photos does not take every core; visible photos and the current one go first.
constexpr int kThumbJobs = 2;

void MainWindow::renderThumbnail(qint64 id)
{
    m_thumbQueue.removeOne(id);
    if (m_photo && id == m_photo->id)
        m_thumbQueue.prepend(id);
    else
        m_thumbQueue.append(id);
    pumpThumbnails();
}

void MainWindow::pumpThumbnails()
{
    while (m_thumbsRunning < kThumbJobs && !m_thumbQueue.isEmpty() && m_batch.isOpen()) {
        // The current photo, else the first one in view, else the oldest request.
        qsizetype pick = 0;
        if (!(m_photo && m_thumbQueue.first() == m_photo->id)) {
            const QRect view = m_filmstrip->viewport()->rect();
            for (qsizetype k = 0; k < m_thumbQueue.size(); ++k) {
                const int row = m_model->rowOf(m_thumbQueue[k]);
                if (row >= 0 && m_filmstrip->visualRect(m_model->index(row)).intersects(view)) {
                    pick = k;
                    break;
                }
            }
        }
        const qint64 id = m_thumbQueue.takeAt(pick);
        auto p = m_batch.photo(id);
        if (!p)
            continue;
        DevelopSettings s = DevelopSettings::fromJsonString(m_batch.settings(id));
        QString path = p->path;
        auto engine = m_engines.get(path, false);
        auto patches = loadHealPatches(m_batch, s);
        QPointer<MainWindow> self(this);
        const QString batchFile = m_batch.file();
        ++m_thumbsRunning;
        m_thumbPool.start([self, id, s, path, engine, patches, batchFile] {
            // Each job's own parallel loops use a few threads only.
            omp_set_num_threads(std::max(1, QThread::idealThreadCount() / 4));
            QByteArray jpeg;
            try {
                auto e = engine ? engine : std::make_shared<DevelopEngine>(path, EngineLoadOptions{.halfSize = true});
                addHealPatches(*e, patches);
                QSize full = e->outputSize(s);
                double k = 512.0 / std::max(full.width(), full.height());
                RenderResult r = e->render(s, {.width = std::max(1, int(full.width() * k)), .height = std::max(1, int(full.height() * k))});
                QBuffer buf(&jpeg);
                buf.open(QIODevice::WriteOnly);
                r.image.save(&buf, "JPEG", 85);
            } catch (...) {
            }
            QMetaObject::invokeMethod(qApp, [self, id, jpeg, batchFile] {
                if (!self)
                    return;
                --self->m_thumbsRunning;
                if (!jpeg.isEmpty() && self->m_batch.isOpen() && self->m_batch.file() == batchFile) {
                    self->m_batch.setThumbnail(id, jpeg, kThumbVersionRendered);
                    self->m_model->invalidateThumbnail(id);
                }
                self->pumpThumbnails();
            });
        });
    }
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
    // Lightroom-style: move on after rejecting a single photo, in the direction of travel
    // (backwards after Previous Photo: going through a batch from the end).
    if (rejected && ids.size() == 1)
        navigate(m_navBackwards ? -1 : 1);
}

void MainWindow::importPhotos()
{
    if (!m_batch.isOpen()) {
        newBatch();
        return;
    }
    QSettings qs;
    QString dir = qs.value("import/lastDir", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)).toString();
    QString filter = tr("Photos (%1)").arg((rawFileNameFilters() + renderedFileNameFilters()).join(' ')) + ";;"
                     + tr("Raw files (%1)").arg(rawFileNameFilters().join(' ')) + ";;"
                     + tr("JPEG and HEIC (%1)").arg(renderedFileNameFilters().join(' ')) + ";;" + tr("All files (*)");
    QStringList files = QFileDialog::getOpenFileNames(this, tr("Import Photos"), dir, filter);
    if (files.isEmpty())
        return;
    qs.setValue("import/lastDir", QFileInfo(files.first()).absolutePath());

    // Default import name: the earliest capture date. Reading it from every file takes
    // a while, so the dialog opens right away and gets the name when it is ready.
    ImportDialog dlg(files, m_batch.imports(), QString(), this);
    auto* dates = new QFutureWatcher<QDateTime>(&dlg);
    connect(dates, &QFutureWatcher<QDateTime>::finished, &dlg, [dates, &dlg] {
        if (dates->isCanceled())
            return;
        QDateTime earliest;
        for (const QDateTime& d : dates->future().results())
            if (d.isValid() && (!earliest.isValid() || d < earliest))
                earliest = d;
        dlg.setDefaultName((earliest.isValid() ? earliest : QDateTime::currentDateTime()).toString(QStringLiteral("yyyy-MM-dd")));
    });
    dates->setFuture(QtConcurrent::mapped(files, [](const QString& f) {
        try {
            return readMetadataSummary(f).captureTime;
        } catch (...) {
            return QFileInfo(f).lastModified();
        }
    }));
    const int accepted = dlg.exec();
    dates->cancel();
    if (accepted != QDialog::Accepted)
        return;
    ImportOptions opts = dlg.options();

    auto* progress = new QProgressDialog(tr("Importing photos..."), QString(), 0, int(files.size()), this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setValue(0);
    TaskbarProgress::instance().follow(progress);
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
        if (!r.sidecars.isEmpty())
            msg += tr("; %n JPEG(s) of RAW+JPEG pairs left out", nullptr, int(r.sidecars.size()));
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
    const bool openFolder = dlg.openFolderAfter();
    QHash<qint64, QString> importNames;
    for (const auto& im : m_batch.imports())
        importNames[im.id] = im.name;
    struct Job {
        PhotoInfo p;
        DevelopSettings s;
        QString import;
        int seq;
        QMap<qint64, HealPatch> patches;
    };
    QList<Job> jobs;
    for (int i = 0; i < photos.size(); ++i) {
        DevelopSettings js = DevelopSettings::fromJsonString(m_batch.settings(photos[i].id));
        jobs.append({photos[i], js, importNames.value(photos[i].importId), i + 1, loadHealPatches(m_batch, js)});
    }

    auto* progress = new QProgressDialog(tr("Exporting..."), tr("Cancel"), 0, int(jobs.size()), this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    TaskbarProgress::instance().follow(progress);
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    connect(progress, &QProgressDialog::canceled, this, [cancelled] { *cancelled = true; });
    QPointer<QProgressDialog> pd(progress);
    auto done = std::make_shared<std::atomic<int>>(0);
    auto pool = std::make_shared<QThreadPool>();
    pool->setMaxThreadCount(2);
    struct Outcome {
        QString path, error;
        QStringList warnings;
    };
    auto future = QtConcurrent::mapped(pool.get(), jobs, [o, cancelled, done, pd, pool](const Job& j) -> Outcome {
        Outcome r;
        if (*cancelled)
            return r;
        try {
            r.path = exportPhoto(j.p, j.s, j.import, j.seq, o, j.patches, &r.warnings);
        } catch (const std::exception& e) {
            r.error = j.p.fileName + ": " + QString::fromUtf8(e.what());
        }
        int n = ++*done;
        QMetaObject::invokeMethod(qApp, [pd, n] {
            if (pd)
                pd->setValue(n);
        });
        return r;
    });
    auto* watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher, progress, o, pool, cancelled, openFolder] {
        QStringList files, errors, warnings;
        for (const Outcome& r : watcher->future().results()) {
            if (!r.path.isEmpty())
                files << r.path;
            if (!r.error.isEmpty())
                errors << r.error;
            warnings << r.warnings;
        }
        progress->deleteLater();
        watcher->deleteLater();
        if (errors.isEmpty() && warnings.isEmpty())
            statusBar()->showMessage(tr("Export finished: %1").arg(o.folder), 5000);
        else
            showExportProblems(errors, warnings);
        if (openFolder && !*cancelled)
            showInFileManager(files);
    });
    watcher->setFuture(future);
}

void MainWindow::showExportProblems(const QStringList& errors, const QStringList& warnings)
{
    QStringList parts;
    if (!errors.isEmpty())
        parts << tr("%n photo(s) could not be exported:", nullptr, int(errors.size())) + "\n" + errors.join('\n');
    if (!warnings.isEmpty())
        parts << tr("%n exported photo(s) may need a look:", nullptr, int(warnings.size())) + "\n" + warnings.join('\n');
    const QString all = parts.join(QStringLiteral("\n\n"));
    QMessageBox box(errors.isEmpty() ? QMessageBox::Information : QMessageBox::Warning, tr("Export"), QString(), QMessageBox::Ok, this);
    // Long lists go to the details (scrollable).
    const int lines = int(errors.size() + warnings.size());
    if (lines <= 15) {
        box.setText(all);
    } else {
        QStringList summary;
        if (!errors.isEmpty())
            summary << tr("%n photo(s) could not be exported.", nullptr, int(errors.size()));
        if (!warnings.isEmpty())
            summary << tr("%n exported photo(s) may need a look.", nullptr, int(warnings.size()));
        box.setText(summary.join(' ') + "\n" + tr("See the details."));
        box.setDetailedText(all);
    }
    box.exec();
}

void MainWindow::removePhotos(bool excludeOnly)
{
    QList<PhotoInfo> photos;
    for (qint64 id : selectedPhotoIds())
        if (auto p = m_batch.photo(id))
            photos << *p;
    if (photos.isEmpty())
        return;
    const int n = int(photos.size());
    QStringList paths;
    for (const auto& p : photos)
        paths << p.path;
    const QString title = excludeOnly ? tr("Exclude from Batch") : tr("Delete Photos");
    const QString what = n == 1 ? photos.first().fileName : tr("%n photos", nullptr, n);
    QMessageBox box(QMessageBox::Question, title,
                    excludeOnly ? tr("Exclude %1 from the batch?\n\nThe files stay on disk; their edits and history are dropped.").arg(what)
                                : tr("Move %1 to the trash, or only exclude from the batch (files stay on disk)?\n\nEdits and history are dropped either way.").arg(what),
                    QMessageBox::Cancel, this);
    box.setDetailedText(paths.join('\n'));
    QPushButton* trash = excludeOnly ? nullptr : box.addButton(tr("Move to Trash"), QMessageBox::DestructiveRole);
    QPushButton* exclude = box.addButton(tr("Exclude from Batch"), excludeOnly ? QMessageBox::AcceptRole : QMessageBox::ActionRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    const bool toTrash = trash && box.clickedButton() == static_cast<QAbstractButton*>(trash);
    if (!toTrash && box.clickedButton() != static_cast<QAbstractButton*>(exclude))
        return;

    // Where to go next: the first photo after the removed ones, else the one before.
    QSet<qint64> removing;
    for (const auto& p : photos)
        removing.insert(p.id);
    qint64 next = 0;
    if (m_photo && removing.contains(m_photo->id)) {
        const int row = m_model->rowOf(m_photo->id);
        for (int r = row + 1; r < m_model->rowCount() && !next; ++r)
            if (!removing.contains(m_model->photoAt(r).id))
                next = m_model->photoAt(r).id;
        for (int r = row - 1; r >= 0 && !next; --r)
            if (!removing.contains(m_model->photoAt(r).id))
                next = m_model->photoAt(r).id;
    }

    QStringList failed;
    QList<PhotoInfo> removed;
    for (const auto& p : photos) {
        QString trashed;
        if (toTrash && QFile::exists(p.path) && !QFile::moveToTrash(p.path, &trashed)) {
            failed << p.path;
            continue;
        }
        if (!trashed.isEmpty())
            m_trashed << trashed;
        removed << p;
    }
    if (!failed.isEmpty()) {
        // No trash on this file system (or no permission): offer to delete for good.
        QMessageBox ask(QMessageBox::Warning, title,
                        tr("%n file(s) could not be moved to the trash. Delete them permanently?\n\nThis cannot be undone.", nullptr, int(failed.size())),
                        QMessageBox::Cancel, this);
        ask.setDetailedText(failed.join('\n'));
        QPushButton* del = ask.addButton(tr("Delete Permanently"), QMessageBox::DestructiveRole);
        ask.setDefaultButton(QMessageBox::Cancel);
        ask.exec();
        if (ask.clickedButton() == static_cast<QAbstractButton*>(del)) {
            QStringList still;
            for (const auto& p : photos)
                if (failed.contains(p.path)) {
                    if (QFile::remove(p.path))
                        removed << p;
                    else
                        still << p.path;
                }
            if (!still.isEmpty())
                QMessageBox::warning(this, title, tr("Could not delete:\n%1").arg(still.join('\n')));
        }
    }
    for (const auto& p : removed) {
        m_batch.removePhoto(p.id);
        if (m_photo && m_photo->id == p.id)
            m_photo.reset();
        if (m_comparePhoto && m_comparePhoto->id == p.id)
            m_comparePhoto.reset();
        if (m_previousPhotoId == p.id)
            m_previousPhotoId = 0;
    }
    if (removed.isEmpty())
        return;
    m_dataVersion = m_batch.dataVersion();
    reloadImports();
    selectImport(m_model->importId(), !m_model->importId(), next);
    statusBar()->showMessage(toTrash ? tr("%n photo(s) moved to the trash", nullptr, int(removed.size()))
                                     : tr("%n photo(s) excluded from the batch", nullptr, int(removed.size())), 4000);
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

namespace {
// "42 s", "3:05" or "1:02:03".
QString formatDuration(qint64 seconds)
{
    if (seconds < 60)
        return QObject::tr("%1 s").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
    return QStringLiteral("%1:%2:%3").arg(seconds / 3600).arg(seconds / 60 % 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0'));
}
} // namespace

void MainWindow::runDerivedJob(const QString& title,
                               std::function<QList<qint64>(Batch&, const std::function<void(const QString&, int, int)>&, const std::atomic<bool>*)> job)
{
    auto* progress = new QProgressDialog(title, tr("Cancel"), 0, 0, this);
    progress->setWindowTitle(title);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setMinimumWidth(420);
    // Stay open between steps (by default the dialog closes when a step reaches its maximum).
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    TaskbarProgress::instance().follow(progress);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    connect(progress, &QProgressDialog::canceled, this, [cancel] { *cancel = true; });
    QPointer<QProgressDialog> pd(progress);
    // Elapsed time, and the time left estimated from the current step's pace.
    struct Timing {
        QElapsedTimer job, step;
        QString stepName, text;
        int done = 0, total = 0;
    };
    auto timing = std::make_shared<Timing>();
    timing->job.start();
    timing->step.start();
    auto showTiming = [pd, timing] {
        if (!pd)
            return;
        QString t = tr("Elapsed %1").arg(formatDuration(timing->job.elapsed() / 1000));
        // After a few units of work the pace is meaningful.
        if (timing->total > 0 && timing->done >= 3 && timing->done < timing->total && timing->step.elapsed() > 2000)
            t += tr(" · about %1 left").arg(formatDuration(timing->step.elapsed() * (timing->total - timing->done) / timing->done / 1000));
        pd->setLabelText(timing->text + '\n' + t);
    };
    auto* tick = new QTimer(progress);
    connect(tick, &QTimer::timeout, progress, showTiming);
    tick->start(1000);
    QString batchFile = m_batch.file();
    auto future = QtConcurrent::run([batchFile, job, cancel, pd, timing, showTiming]() -> QPair<QList<qint64>, QString> {
        try {
            Batch b;
            b.open(batchFile);
            auto ids = job(b, [pd, timing, showTiming](const QString& step, int done, int total) {
                QMetaObject::invokeMethod(qApp, [pd, step, done, total, timing, showTiming] {
                    if (!pd)
                        return;
                    if (step != timing->stepName) { // a new step: its own pace
                        timing->stepName = step;
                        timing->step.restart();
                    }
                    timing->done = done;
                    timing->total = total;
                    timing->text = total > 0 ? QStringLiteral("%1 (%2 / %3)").arg(step).arg(done).arg(total) : step;
                    pd->setMaximum(std::max(0, total));
                    pd->setValue(total > 0 ? done : 0);
                    showTiming();
                });
            }, cancel.get());
            return {ids, QString()};
        } catch (const std::exception& e) {
            return {{}, QString::fromUtf8(e.what())};
        }
    });
    auto* watcher = new QFutureWatcher<QPair<QList<qint64>, QString>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, progress, cancel, title] {
        auto [ids, error] = watcher->result();
        watcher->deleteLater();
        progress->deleteLater();
        m_dataVersion = m_batch.dataVersion();
        reloadImports();
        m_model->refresh();
        if (!error.isEmpty() && !*cancel)
            QMessageBox::warning(this, title, error);
        if (!ids.isEmpty())
            selectPhoto(ids.last());
    });
    watcher->setFuture(future);
}

void MainWindow::aiDenoise()
{
    // JPEG/HEIC photos were denoised by the camera already; the model works on raw data.
    QList<qint64> ids;
    int rendered = 0;
    for (qint64 id : selectedPhotoIds()) {
        auto p = m_batch.photo(id);
        if (p && p->kind == PhotoKind::Rendered)
            ++rendered;
        else if (p)
            ids << id;
    }
    if (ids.isEmpty()) {
        if (rendered)
            QMessageBox::information(this, tr("AI Denoise"), tr("AI Denoise works on raw photos: JPEG and HEIC photos are already denoised by the camera."));
        return;
    }
    const QString modelId = defaultAiDenoiseModel();
    // The preview needs the model: download it first.
    if (!ensureAiModel(modelId))
        return;
    // Preview the current photo if it is one of them, else the first.
    const qint64 previewId = m_photo && ids.contains(m_photo->id) ? m_photo->id : ids.first();
    auto pp = m_batch.photo(previewId);
    if (!pp)
        return;
    const DevelopSettings ps = previewId == (m_photo ? m_photo->id : 0) ? m_settings : DevelopSettings::fromJsonString(m_batch.settings(previewId));
    DenoiseDialog dlg(modelId, pp->path, ps, loadHealPatches(m_batch, ps), int(ids.size()), rendered, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const double s = dlg.strength() / 100.0;
    runDerivedJob(tr("AI Denoise"), [ids, modelId, s](Batch& b, const auto& progress, const std::atomic<bool>* cancel) {
        QList<qint64> out;
        for (int i = 0; i < ids.size(); ++i) {
            auto p = b.photo(ids[i]);
            QString name = p ? p->fileName : QString();
            out << createAiDenoised(b, ids[i], modelId, s, [&](const QString& step, int d, int t) {
                progress(QStringLiteral("%1 [%2/%3]: %4").arg(name).arg(i + 1).arg(ids.size()).arg(step), d, t);
            }, cancel);
        }
        return out;
    });
}

void MainWindow::mergePhotos(bool hdr)
{
    QList<qint64> ids;
    for (const QModelIndex& i : m_filmstrip->selectionModel()->selectedIndexes())
        ids << i.data(PhotoListModel::IdRole).toLongLong();
    QString title = hdr ? tr("Merge to HDR") : tr("Merge to Panorama");
    if (ids.size() < 2) {
        QMessageBox::information(this, title, tr("Select at least two photos in the filmstrip (Ctrl+click or Shift+click)."));
        return;
    }
    MergeDialog dlg(hdr ? MergeDialog::Hdr : MergeDialog::Panorama, mergeSources(m_batch, ids), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (hdr) {
        const HdrOptions o = dlg.hdrOptions();
        runDerivedJob(title, [ids, o](Batch& b, const auto& progress, const std::atomic<bool>* cancel) {
            return QList<qint64>{createHdrMerge(b, ids, o, progress, cancel)};
        });
    } else {
        const PanoOptions o = dlg.panoOptions();
        runDerivedJob(title, [ids, o](Batch& b, const auto& progress, const std::atomic<bool>* cancel) {
            return QList<qint64>{createPanorama(b, ids, o, progress, cancel)};
        });
    }
}

bool MainWindow::ensureAiModel(const QString& id)
{
    if (aiModelInstalled(id))
        return true;
    const AiModelInfo* m = findAiModel(id);
    if (!m)
        return false;
    qint64 size = 0;
    for (const auto& f : m->files)
        size += f.size;
    const QString sizeText = size > 0 ? tr("%1 MB").arg(size / 1000000) : tr("a few hundred MB");
    if (QMessageBox::question(this, tr("Download AI Model"),
                              tr("This needs the %1 model (%2, %3 license), which is downloaded once and then kept in\n%4\n\nDownload it now?")
                                  .arg(m->name, sizeText, m->license, QDir::toNativeSeparators(aiModelDir(id))))
        != QMessageBox::Yes)
        return false;
    QProgressDialog pd(tr("Downloading %1...").arg(m->name), tr("Cancel"), 0, 1000, this);
    pd.setWindowModality(Qt::WindowModal);
    pd.setMinimumDuration(0);
    pd.setAutoClose(false);
    pd.setAutoReset(false);
    pd.setValue(0);
    TaskbarProgress::instance().follow(&pd);
    std::atomic<bool> cancel = false;
    connect(&pd, &QProgressDialog::canceled, this, [&cancel] { cancel = true; });
    try {
        downloadAiModel(id, [&pd, m](qint64 received, qint64 total) {
            if (total > 0) {
                pd.setValue(int(received * 1000 / total));
                pd.setLabelText(tr("Downloading %1...\n%2 of %3 MB").arg(m->name).arg(received / 1000000).arg(total / 1000000));
            }
        }, &cancel);
    } catch (const std::exception& e) {
        pd.close();
        if (!cancel)
            QMessageBox::warning(this, tr("Download AI Model"),
                                 tr("The download did not succeed:\n%1\n\nThe server may be temporarily unavailable; you can try again later.").arg(QString::fromUtf8(e.what())));
        return false;
    }
    return aiModelInstalled(id);
}

void MainWindow::setHealTool(bool on)
{
    if (on && !m_pane->engine())
        on = false;
    if (on && !ensureAiModel(aiInpaintModel().id))
        on = false;
    if (on && m_cropTool)
        setCropTool(false);
    if (on && m_wbPicker)
        setWbPicker(false);
    m_healTool = on;
    m_panel->setHealToolActive(on);
    m_pane->setTool(on ? m_healToolObj : nullptr);
    if (on)
        updateHealOverlay();
}

void MainWindow::setWbPicker(bool on)
{
    if (on && !m_pane->engine())
        on = false;
    if (on && m_cropTool)
        setCropTool(false);
    if (on && m_healTool)
        setHealTool(false);
    m_wbPicker = on;
    m_panel->setWbPickerActive(on);
    if (on)
        m_pane->setTool(m_wbPickerObj);
    else if (m_pane->tool() == m_wbPickerObj)
        m_pane->setTool(nullptr);
    if (on)
        statusBar()->showMessage(tr("Click a point that should be neutral grey (Esc or W to cancel)"));
    else
        statusBar()->clearMessage();
}

void MainWindow::pickWhiteBalance(QPointF cropPoint)
{
    auto e = m_pane->engine();
    if (!m_photo || !e)
        return;
    auto wb = e->wbFromPoint(m_settings, cropPoint);
    if (!wb) {
        // Stay in the tool: the user can try another point.
        statusBar()->showMessage(tr("That point is too bright (clipped) or too dark to judge its colour; try another one"));
        return;
    }
    setWbPicker(false);
    DevelopSettings s = m_settings;
    s.wbMode = WbMode::Custom;
    s.temperature = std::round(wb->temperature);
    s.tint = std::round(wb->tint);
    commit(s, tr("White Balance: Picked"));
}

void MainWindow::updateHealOverlay()
{
    auto e = m_pane->engine();
    std::optional<Geometry> g;
    if (e)
        g = e->geometry(m_settings);
    m_healToolObj->setSpots(m_settings.heals, g, e ? std::max(e->raw().fullWidth, e->raw().fullHeight) : 1);
    m_pane->update();
}

void MainWindow::ensureHealPatches(DevelopEngine* e, const DevelopSettings& s)
{
    if (!e || s.heals.isEmpty())
        return;
    DevelopSettings missing;
    for (const auto& h : s.heals)
        if (!e->hasHealPatch(h.patchId))
            missing.heals.append(h);
    if (!missing.heals.isEmpty())
        addHealPatches(*e, loadHealPatches(m_batch, missing));
}

void MainWindow::updateHealBusy()
{
    const bool busy = m_healJobs > 0;
    m_healProgress->setVisible(busy);
    TaskbarProgress::instance().setCount(m_healJobs);
    if (busy && !m_healAnim.isActive())
        m_healAnim.start();
    else if (!busy)
        m_healAnim.stop();
    if (busy)
        statusBar()->showMessage(m_healJobs == 1 ? tr("Generating fill...") : tr("Generating %1 fills...").arg(m_healJobs));
    else
        statusBar()->clearMessage();
    m_pane->update();
}

void MainWindow::healAt(const QList<QPointF>& stroke, double radius)
{
    auto e = m_pane->engine();
    if (!m_photo || !e || stroke.isEmpty())
        return;
    if (e->isHalfSize()) {
        statusBar()->showMessage(tr("Still loading the full-resolution image; try again in a moment"), 3000);
        return;
    }
    Geometry g = e->geometry(m_settings);
    QList<QPointF> sensor;
    for (const QPointF& p : stroke) {
        float src[6], gain[3];
        if (g.mapCrop(p.x(), p.y(), src, gain))
            sensor.append(QPointF(src[2], src[3]));
    }
    if (sensor.isEmpty())
        return;
    QSizeF crop = g.cropSize();
    HealSpot spot;
    spot.x = sensor[0].x();
    spot.y = sensor[0].y();
    spot.path = sensor.mid(1);
    spot.radius = radius * std::max(crop.width(), crop.height()) / std::max(e->raw().fullWidth, e->raw().fullHeight) / g.lensZoom();
    m_healToolObj->addBusy(stroke, radius);
    ++m_healJobs;
    updateHealBusy();
    qint64 photoId = m_photo->id;
    QPointer<MainWindow> self(this);
    auto future = QtConcurrent::run([e, spot]() -> std::pair<HealPatch, QString> {
        try {
            return {generateHealPatch(e->raw(), spot), QString()};
        } catch (const std::exception& ex) {
            return {HealPatch{}, QString::fromUtf8(ex.what())};
        }
    });
    auto* w = new QFutureWatcher<std::pair<HealPatch, QString>>(this);
    connect(w, &QFutureWatcherBase::finished, this, [this, w, e, spot, stroke, photoId] {
        auto [patch, err] = w->result();
        w->deleteLater();
        --m_healJobs;
        m_healToolObj->removeBusy(stroke);
        updateHealBusy();
        if (!err.isEmpty()) {
            QMessageBox::warning(this, tr("AI Heal"), err);
            return;
        }
        if (!m_batch.photo(photoId))
            return; // deleted or excluded meanwhile
        HealSpot sp = spot;
        sp.patchId = storeHealPatch(m_batch, photoId, patch);
        e->addHealPatch(sp.patchId, patch);
        if (!m_photo || m_photo->id != photoId) {
            // The user moved on: record the heal on that photo's history anyway.
            DevelopSettings s = DevelopSettings::fromJsonString(m_batch.settings(photoId));
            s.heals.append(sp);
            m_batch.pushHistory(photoId, tr("Heal"), s.toJsonString());
            historyChanged(photoId);
            renderThumbnail(photoId);
            return;
        }
        DevelopSettings s = m_settings;
        s.heals.append(sp);
        commit(s, tr("Heal"));
    });
    w->setFuture(future);
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
    // Straight-line guides while adjusting geometry.
    static const QStringList guideKeys = {"vertical", "horizontal", "rotate", "scale", "lens_distortion"};
    if (guideKeys.contains(m_activeSlider)) {
        m_pane->setOverlay([](QPainter& p, const std::function<QPointF(QPointF)>&, const QRectF& r) {
            p.setPen(QPen(QColor(255, 255, 255, 70), 1));
            // About 30 across the long edge, but neither crowded nor sparse on screen.
            const double step = std::clamp(std::max(r.width(), r.height()) / 30, 24.0, 64.0);
            for (double x = r.left() + step / 2; x < r.right(); x += step)
                p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            for (double y = r.top() + step / 2; y < r.bottom(); y += step)
                p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        });
    } else {
        m_pane->setOverlay({});
    }
    if (!m_photo || m_viewMode == ViewMode::Single) {
        m_pane2->hide();
        m_pane->setLabel(QString());
        return;
    }
    m_pane2->show();
    if (m_viewMode == ViewMode::BeforeAfter) {
        DevelopSettings before = DevelopSettings::defaults(m_photo && m_photo->kind == PhotoKind::Rendered);
        before.quarterTurns = m_settings.quarterTurns;
        m_pane2->setSettings(before);
        m_pane2->setEngine(m_pane->engine());
        m_pane2->setLabel(tr("Before"));
        m_pane->setLabel(tr("After"));
    } else if (m_comparePhoto) {
        auto e = m_engines.get(m_comparePhoto->path);
        DevelopSettings cs = DevelopSettings::fromJsonString(m_batch.settings(m_comparePhoto->id));
        ensureHealPatches(e.get(), cs);
        m_pane2->setSettings(cs);
        m_pane2->setEngine(e);
        m_pane2->setLabel(m_comparePhoto->fileName);
        m_pane->setLabel(m_photo->fileName);
    }
    m_pane2->setViewState(m_pane->viewState());
}

void MainWindow::toggleSidePanels()
{
    // Auto-hidden panels are left alone.
    QList<QDockWidget*> docks;
    if (!isAutoHide(Qt::LeftEdge))
        docks << m_leftDock;
    if (!isAutoHide(Qt::RightEdge))
        docks << m_rightDock;
    bool visible = std::any_of(docks.begin(), docks.end(), [](QDockWidget* d) { return d->isVisible(); });
    for (QDockWidget* d : docks)
        d->setVisible(!visible);
}

// ---- auto hide & show panels ----------------------------------------------------

namespace {
QString edgeKey(Qt::Edge e) { return QString::number(int(e)); }
} // namespace

QDockWidget* MainWindow::dockFor(Qt::Edge edge) const
{
    return edge == Qt::LeftEdge ? m_leftDock : edge == Qt::RightEdge ? m_rightDock : m_bottomDock;
}

AutoHidePanel* MainWindow::autoHidePanel(Qt::Edge edge) const
{
    return edge == Qt::LeftEdge ? m_autoLeft : edge == Qt::RightEdge ? m_autoRight : m_autoBottom;
}

bool MainWindow::isAutoHide(Qt::Edge edge) const { return autoHidePanel(edge)->isActive(); }

void MainWindow::setAutoHide(Qt::Edge edge, bool on)
{
    AutoHidePanel* panel = autoHidePanel(edge);
    if (on == panel->isActive())
        return;
    QDockWidget* dock = dockFor(edge);
    const bool vertical = edge == Qt::BottomEdge;
    QSettings settings;
    const QString extentKey = QStringLiteral("ui/autoHide/%1Extent").arg(edgeKey(edge));
    settings.setValue(QStringLiteral("ui/autoHide/%1").arg(edgeKey(edge)), on);
    if (on) {
        int extent = settings.value(extentKey, edge == Qt::LeftEdge ? 230 : edge == Qt::RightEdge ? 330 : 130).toInt();
        if (dock->isVisible() && isVisible())
            extent = vertical ? dock->height() : dock->width();
        else if (edge == Qt::LeftEdge && m_metaDock->isVisible() && isVisible())
            extent = m_metaDock->width();
        settings.setValue(extentKey, extent);
        QWidget* content;
        if (edge == Qt::LeftEdge) {
            // The metadata view shares the left side: show both as tabs.
            m_leftTabs = new QTabWidget();
            m_leftTabs->setDocumentMode(true);
            const bool metaShown = m_metaDock->isVisible() && !m_metaDock->visibleRegion().isEmpty();
            m_leftTabs->addTab(m_leftDock->widget(), tr("Imports && History"));
            m_leftTabs->addTab(m_metaDock->widget(), tr("Metadata"));
            m_leftTabs->setCurrentIndex(metaShown ? 1 : 0);
            QToolButton* pin = makePinButton(false, m_leftTabs);
            // Queued: pinning deletes the tab widget and with it this button.
            connect(pin, &QToolButton::clicked, this, [this] { setAutoHide(Qt::LeftEdge, false); }, Qt::QueuedConnection);
            m_leftTabs->setCornerWidget(pin);
            connect(m_leftTabs, &QTabWidget::currentChanged, this, [this] {
                if (m_photo && m_metaView->isVisible())
                    m_metaView->showFile(m_photo->path);
            });
            m_metaDock->hide();
            m_metaDock->toggleViewAction()->setEnabled(false);
            content = m_leftTabs;
        } else {
            content = dock->widget();
        }
        dock->hide();
        dock->toggleViewAction()->setEnabled(false);
        panel->setExtent(extent);
        panel->setContent(content);
        panel->setActive(true);
    } else {
        panel->setActive(false);
        QWidget* content = panel->takeContent();
        if (edge == Qt::LeftEdge) {
            QWidget* left = m_leftTabs->widget(0);
            QWidget* meta = m_leftTabs->widget(1);
            const bool metaShown = m_leftTabs->currentIndex() == 1;
            m_leftDock->setWidget(left);
            m_metaDock->setWidget(meta);
            delete m_leftTabs;
            m_leftTabs = nullptr;
            m_metaDock->toggleViewAction()->setEnabled(true);
            if (metaShown) {
                m_metaDock->show();
                m_metaDock->raise();
            }
        } else {
            dock->setWidget(content);
        }
        dock->toggleViewAction()->setEnabled(true);
        dock->show();
        resizeDocks({dock}, {panel->extent()}, vertical ? Qt::Vertical : Qt::Horizontal);
    }
    layoutAutoHidePanels();
    // Last: the action calls back into this function.
    if (QAction* a = m_autoHideActions.value(int(edge)); a && a->isChecked() != on)
        a->setChecked(on);
}

void MainWindow::layoutAutoHidePanels()
{
    if (!m_autoLeft)
        return;
    int top = menuBar()->isVisible() ? menuBar()->geometry().bottom() + 1 : 0;
    int bottom = statusBar()->isVisible() ? statusBar()->geometry().top() : height();
    QRect area(0, top, width(), bottom - top);
    m_autoLeft->setArea(area);
    m_autoRight->setArea(area);
    // The filmstrip overlay spans between pinned side panels (or their edge strips).
    int l = area.left(), r = area.right();
    for (QDockWidget* d : {m_leftDock, m_metaDock, m_autoLeft->triggerDock()})
        if (d->isVisible() && !d->isFloating() && dockWidgetArea(d) == Qt::LeftDockWidgetArea)
            l = std::max(l, d->geometry().right() + 1);
    for (QDockWidget* d : {m_rightDock, m_autoRight->triggerDock()})
        if (d->isVisible() && !d->isFloating() && dockWidgetArea(d) == Qt::RightDockWidgetArea)
            r = std::min(r, d->geometry().left() - 1);
    m_autoBottom->setArea(QRect(QPoint(l, area.top()), QPoint(r, area.bottom())));
}

void MainWindow::resizeEvent(QResizeEvent* e)
{
    QMainWindow::resizeEvent(e);
    layoutAutoHidePanels();
}

bool MainWindow::eventFilter(QObject* o, QEvent* e)
{
    switch (e->type()) {
    case QEvent::ShortcutOverride:
        // Space presses a focused button or opens a focused combo box rather than toggling
        // the zoom (text fields claim it themselves).
        if (auto* k = static_cast<QKeyEvent*>(e); k->key() == Qt::Key_Space && k->modifiers() == Qt::NoModifier
                                                  && (qobject_cast<QAbstractButton*>(o) || qobject_cast<QComboBox*>(o))
                                                  && static_cast<QWidget*>(o)->window() == this) {
            e->accept();
            return true;
        }
        break;
    case QEvent::Resize:
    case QEvent::Move:
    case QEvent::Show:
    case QEvent::Hide:
        if (qobject_cast<QDockWidget*>(o))
            QTimer::singleShot(0, this, &MainWindow::layoutAutoHidePanels);
        break;
    default: break;
    }
    return QMainWindow::eventFilter(o, e);
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
    for (const auto& p : m_model->photos()) {
        auto it = m_knownHistory.find(p.id);
        if (it != m_knownHistory.end() && it.value() != p.historyPos && (!m_photo || p.id != m_photo->id))
            renderThumbnail(p.id);
        m_knownHistory[p.id] = p.historyPos;
    }
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
    // A pending "silma show" request wins over the saved session.
    if (qint64 go = m_batch.state("gui.goto").toLongLong(); go > 0) {
        m_batch.setState("gui.goto", QString());
        if (auto p = m_batch.photo(go)) {
            photoId = go;
            im = QString::number(p->importId);
        }
    }
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
        m_thumbQueue.clear();
        renderThumbnail(m_photo->id);
        m_thumbPool.waitForDone(5000);
        QCoreApplication::processEvents();
    }
    saveSession();
    QSettings s;
    s.setValue("window/geometry", saveGeometry());
    s.setValue("window/state", saveState());
    e->accept();
}

} // namespace silma
