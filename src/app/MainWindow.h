#pragma once

#include "Batch.h"
#include "Develop.h"
#include "EngineCache.h"
#include "ImagePane.h"

#include <QMainWindow>
#include <QPointer>
#include <QTimer>
#include <atomic>
#include <functional>
#include <optional>

class QAction;
class QDockWidget;
class QLabel;
class QListWidget;
class QSplitter;

namespace silma {

class CropTool;
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
    void testSetViewMode(ViewMode m) { setViewMode(m); }
    void testPoll() { pollDatabase(); }
    void testAutoUpright(int mode) { autoUpright(mode); }
    void testCropTool(bool on) { setCropTool(on); }
    CropTool* cropTool() const { return m_cropToolObj; }

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    // setup
    void createActions();
    void createDocks();
    void restoreWindowState();

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
    void copySettings();
    void pasteSettings();
    void scheduleThumbnail();
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
    QLabel* m_status;
    CropTool* m_cropToolObj;

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
