#pragma once
// AI Denoise dialog: model, strength and a 1:1 preview of a square region of one photo, which
// can be dragged around (in the preview or on the overview). Press and hold shows the original.

#include "Develop.h"
#include "Heal.h"

#include <QDialog>
#include <QImage>
#include <QTimer>
#include <memory>

class QLabel;
class QSlider;
class QSpinBox;

namespace silma {

class DenoisePreview;
class DenoiseOverview;

class DenoiseDialog : public QDialog {
    Q_OBJECT
public:
    // `path`/`settings`/`patches`: the photo previewed; `photoCount` raw photos will be
    // denoised, `renderedLeftOut` JPEG/HEIC photos are skipped.
    DenoiseDialog(const QString& modelId, const QString& path, const DevelopSettings& settings, const QMap<qint64, HealPatch>& patches,
                  int photoCount, int renderedLeftOut, QWidget* parent = nullptr);
    ~DenoiseDialog() override;

    int strength() const; // percent

    // For tests: the preview is up to date (the denoised region at the current strength).
    bool previewReady() const;
    QImage previewImage() const;
    QImage originalImage() const;
    QPointF regionCenter() const { return m_center; }
    void setRegionCenter(QPointF c); // normalized crop coordinates
    void setStrength(int percent);

    struct State;

private:
    void requestRegion();
    void requestStrength();
    void loaded(const QString& error, const QImage& overview, QSizeF outputSize);
    void regionRendered(int gen, const QImage& before, const QImage& after, const QString& error);
    void strengthRendered(int gen, int strength, const QImage& after);
    QSize previewPixels() const;
    void updateStatus();

    std::shared_ptr<State> m_state;
    DevelopSettings m_settings;
    QPointF m_center{0.5, 0.5};
    QSizeF m_outputSize;
    bool m_loaded = false;
    int m_regionGen = 0, m_strengthGen = 0;
    int m_shownRegion = -1, m_shownStrength = -1; // what the after image shows
    bool m_denoising = false;
    QImage m_before, m_after; // shown region: original and denoised
    QTimer m_strengthTimer;
    DenoisePreview* m_preview;
    DenoiseOverview* m_overview;
    QSlider* m_slider;
    QSpinBox* m_spin;
    QLabel* m_status;
};

} // namespace silma
