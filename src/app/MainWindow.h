#pragma once

#include "Batch.h"
#include "Develop.h"
#include "EngineCache.h"
#include "ImagePane.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>
#include <atomic>
#include <functional>
#include <optional>

class QAction;
class QDockWidget;
class QLabel;
class QProgressBar;
class QListWidget;
class QSplitter;
class QTabWidget;

namespace silma {

class AutoHidePanel;
class CropTool;
class HealTool;
class WbPickerTool;
class MetadataView;
class DevelopPanel;
class Filmstrip;
class PhotoListModel;

enum class ViewMode { Single, BeforeAfter, Compare };

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool openBatch(const QString& path);

    // For tests and automation.
    std::optional<PhotoInfo> currentPhoto() const { return m_photo; }
    DevelopSettings currentSettings() const { return m_settings; }
    ImagePane* mainPane() const { return m_pane; }
    ImagePane* secondPane() const { return m_pane2; }
    DevelopPanel* developPanel() const { return m_panel; }
    void testUndo() { undo(); }
    void testRedo() { redo(); }
    void testReject() { setRejected(true); }
    void testCopySettings() { copySettings(); }
    void testPasteSettings(bool ask = true) { pasteSettings(ask); }
    void testSetViewMode(ViewMode m) { setViewMode(m); }
    void testPoll() { pollDatabase(); }
    void testResetSettings() { resetSettings(); }
    void testRunDerivedJob(const QString& title, std::function<QList<qint64>(Batch&, const std::function<void(const QString&, int, int)>&, const std::atomic<bool>*)> job)
    {
        runDerivedJob(title, std::move(job));
    }
    QList<qint64> testSelectedIds() const { return selectedPhotoIds(); }
    void testAutoUpright(int mode) { autoUpright(mode); }
    void testTurn(int quarterTurnsCw) { turnPhotos(quarterTurnsCw); }
    void testResetTransform() { resetTransform(); }
    void testCommit(const DevelopSettings& s, const QString& label) { commit(s, label); }
    void testCropTool(bool on) { setCropTool(on); }
    void testWbPicker(bool on) { setWbPicker(on); }
    void testHealTool(bool on) { setHealTool(on); }
    void testPickWhiteBalance(QPointF p) { pickWhiteBalance(p); }
    bool wbPickerActive() const { return m_wbPicker; }
    CropTool* cropTool() const { return m_cropToolObj; }
    void testHealAt(QPointF p, double r) { healAt({p}, r); }
    void testHealStroke(const QList<QPointF>& stroke, double r) { healAt(stroke, r); }
    int pendingHealJobs() const { return m_healJobs; }

    // Auto hide & show for the left panel, right panel and filmstrip (Qt::LeftEdge,
    // Qt::RightEdge, Qt::BottomEdge).
    void setAutoHide(Qt::Edge edge, bool on);
    bool isAutoHide(Qt::Edge edge) const;
    AutoHidePanel* autoHidePanel(Qt::Edge edge) const;

protected:
    void closeEvent(QCloseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    // setup
    void createActions();
    void createDocks();
    void restoreWindowState();
    QDockWidget* dockFor(Qt::Edge edge) const;
    void layoutAutoHidePanels();

    // batch / navigation
    void newBatch();
    void openBatchDialog();
    void closeBatch();
    void reloadImports();
    void selectImport(std::optional<qint64> importId, bool all);
    void selectPhoto(qint64 id);
    void showCurrentPhoto();
    void navigate(int delta);
    void updateTitle();
    void updateRecentMenu();

    // editing
    void onPanelEdited(const DevelopSettings& s, const QString& label, bool final);
    void commit(const DevelopSettings& s, const QString& label);
    void applySettingsToUi();
    void reloadHistory();
    void undo();
    void redo();
    void jumpToHistory(int seq);
    void autoTone();
    void autoUpright(int mode);
    void autoCrop();
    void resetSettings();
    void resetTransform();
    void turnPhotos(int quarterTurnsCw);
    void copySettings();
    // `ask`: choose the settings to paste in a dialog, else paste the previous choice.
    void pasteSettings(bool ask = true);
    void scheduleThumbnail();
    void historyChanged(qint64 id);
    void renderThumbnail(qint64 id);

    // culling / files
    void setRejected(bool rejected);
    void importPhotos();
    void exportPhotos();
    void deleteRejected();
    void aiDenoise();
    void mergePhotos(bool hdr);
    // Runs a job creating derived photos in the background with a progress dialog.
    void runDerivedJob(const QString& title, std::function<QList<qint64>(Batch&, const std::function<void(const QString&, int, int)>&, const std::atomic<bool>*)> job);

    // view
    void setViewMode(ViewMode m);
    void updatePanes();
    void onEngineReady(const QString& path);
    void toggleSidePanels();
    void setCropTool(bool on);
    void setHealTool(bool on);
    // Makes sure an AI model is installed, offering to download it (modal). False if not.
    bool ensureAiModel(const QString& id);
    void setWbPicker(bool on);
    void pickWhiteBalance(QPointF cropPoint);
    // Heals along a stroke (crop-normalized points; one point = round spot).
    void healAt(const QList<QPointF>& stroke, double radius);
    void updateHealBusy();
    void ensureHealPatches(DevelopEngine* e, const DevelopSettings& s);
    void updateHealOverlay();
    QList<qint64> selectedPhotoIds() const;

    // external changes (CLI)
    void pollDatabase();
    void saveSession();
    void restoreSession();

    Batch m_batch;
    EngineCache m_engines;
    PhotoListModel* m_model;
    Filmstrip* m_filmstrip;
    DevelopPanel* m_panel;
    ImagePane* m_pane;  // the active (edited) photo
    ImagePane* m_pane2; // before / compare reference
    QListWidget* m_importList;
    QListWidget* m_historyList;
    QDockWidget* m_leftDock;
    QDockWidget* m_rightDock;
    QDockWidget* m_bottomDock;
    QDockWidget* m_metaDock;
    AutoHidePanel* m_autoLeft = nullptr;
    AutoHidePanel* m_autoRight = nullptr;
    AutoHidePanel* m_autoBottom = nullptr;
    QTabWidget* m_leftTabs = nullptr; // left panel content while auto-hidden
    QHash<int, QAction*> m_autoHideActions;
    MetadataView* m_metaView;
    QHash<qint64, int> m_knownHistory; // photo id -> history position, to spot external edits
    QLabel* m_status;
    CropTool* m_cropToolObj;
    HealTool* m_healToolObj;
    bool m_healTool = false;
    QTimer m_healAnim; // animates areas being generated
    QProgressBar* m_healProgress = nullptr;
    WbPickerTool* m_wbPickerObj;
    bool m_wbPicker = false;
    int m_healJobs = 0;

    std::optional<PhotoInfo> m_photo;
    std::optional<PhotoInfo> m_comparePhoto;
    qint64 m_previousPhotoId = 0;
    DevelopSettings m_settings;  // live (possibly uncommitted while dragging)
    DevelopSettings m_committed; // what the history says
    std::optional<DevelopSettings> m_clipboard;
    ViewMode m_viewMode = ViewMode::Single;
    bool m_cropTool = false;
    QString m_activeSlider;
    QTimer m_thumbTimer;
    QTimer m_pollTimer;
    qint64 m_dataVersion = -1;
    bool m_restoring = false;

    QAction* m_actUndo;
    QAction* m_actRedo;
    QAction* m_actBeforeAfter;
    QAction* m_actCompare;
    QMenu* m_recentMenu;
};

} // namespace silma
