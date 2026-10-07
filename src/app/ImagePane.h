#pragma once

#include "Engine.h"

#include <QImage>
#include <QPointF>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <memory>

namespace silma {

struct ViewState {
    bool fit = true;
    double zoom = 1.0;        // logical screen px per full-res image px (when !fit)
    QPointF center{0.5, 0.5}; // crop-normalized point at the view centre
    bool operator==(const ViewState&) const = default;
};

// Draws overlays (guides, crop frame) on top of the image; coordinates map crop-normalized
// points to widget coordinates via the supplied function.
using OverlayPainter = std::function<void(QPainter&, const std::function<QPointF(QPointF)>& toWidget, const QRectF& imageRect)>;

// One image view: shows a photo rendered with given settings, with zoom and pan.
class ImagePane : public QWidget {
    Q_OBJECT
public:
    explicit ImagePane(QWidget* parent = nullptr);

    void setEngine(std::shared_ptr<DevelopEngine> engine);
    void setSettings(const DevelopSettings& s);
    void setPlaceholder(const QImage& img);
    void setLabel(const QString& label);
    void setMessage(const QString& msg);
    void setIgnoreCrop(bool on);
    void setOverlay(OverlayPainter p);
    void setInteractionEnabled(bool on) { m_interaction = on; }

    ViewState viewState() const { return m_view; }
    void setViewState(const ViewState& v);
    void zoomTo(double zoom, QPointF anchorWidget);
    void toggleZoom(QPointF anchorWidget);
    double fitZoom() const;
    double currentZoom() const { return m_view.fit ? fitZoom() : m_view.zoom; }
    // Widget point -> crop-normalized coordinates.
    QPointF widgetToImage(QPointF w) const;
    QPointF imageToWidget(QPointF n) const;
    QRectF imageRect() const;

    std::shared_ptr<DevelopEngine> engine() const { return m_engine; }
    bool hasRender() const { return !m_base.image.isNull(); }

signals:
    void viewStateChanged(const ViewState& v);
    void rendered(const Histogram& h, const WbValues& wb);
    // Raw mouse events in crop-normalized coordinates, for tools (crop etc.).
    void imagePressed(QPointF n, QMouseEvent* e);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    struct Rendered {
        QImage image;
        QRectF roi; // crop-normalized region it covers
        quint64 gen = 0;
    };
    QSizeF cropPx() const;
    void scheduleBase();
    void scheduleDetail();
    void runBase();
    void runDetail();
    void clampCenter();

    std::shared_ptr<DevelopEngine> m_engine;
    DevelopSettings m_settings;
    bool m_ignoreCrop = false;
    QSizeF m_cropPx; // cached geometry crop size
    ViewState m_view;
    QImage m_placeholder;
    QString m_label, m_message;
    OverlayPainter m_overlay;
    bool m_interaction = true;

    Rendered m_base, m_detail;
    quint64 m_baseGen = 0, m_detailGen = 0;
    bool m_baseBusy = false, m_baseDirty = false, m_detailBusy = false, m_detailDirty = false;
    QTimer m_detailTimer;

    QPointF m_pressPos, m_lastPos;
    bool m_dragging = false;
    bool m_pressed = false;
};

} // namespace silma
