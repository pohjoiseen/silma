#pragma once

#include "Develop.h"
#include "Geometry.h"
#include "ImagePane.h"

#include <QObject>
#include <optional>

namespace silma {

// Click to heal a round spot, or drag to heal along a brush stroke; Alt+click on a healed
// area removes it. Coordinates are normalized to the crop (the displayed image).
class HealTool : public QObject, public PaneTool {
    Q_OBJECT
public:
    explicit HealTool(QObject* parent = nullptr) : QObject(parent) {}

    // Brush radius relative to the displayed image's long edge.
    void setBrushSize(double r) { m_brush = r; }
    double brushSize() const { return m_brush; }
    void setSpots(const QList<HealSpot>& spots, std::optional<Geometry> g, double sensorLong);
    // Areas being generated (shown animated); call tick() to animate.
    void addBusy(const QList<QPointF>& stroke, double radius);
    void removeBusy(const QList<QPointF>& stroke);
    bool isBusy() const { return !m_busy.isEmpty(); }
    void tick() { m_phase += 1.5; }

    void paint(QPainter& p, const ImagePane& pane) override;
    bool press(const ImagePane& pane, QPointF n, QMouseEvent* e) override;
    void move(const ImagePane& pane, QPointF n, QMouseEvent* e) override;
    void release(const ImagePane& pane, QPointF n, QMouseEvent* e) override;
    Qt::CursorShape cursorAt(const ImagePane& pane, QPointF n) const override;

signals:
    // A stroke (one point for a round spot) to heal.
    void healRequested(const QList<QPointF>& cropPoints, double radius);
    void removeRequested(int spotIndex);

private:
    struct Shown {
        QList<QPointF> pts; // crop-normalized
        double radius;      // relative to the crop's long edge
    };
    // Crop-normalized -> units of the crop's long edge (isotropic).
    QPointF iso(QPointF n) const { return {n.x() * m_aspect.width(), n.y() * m_aspect.height()}; }
    double distanceTo(const Shown& s, QPointF n) const;
    double extentWith(QPointF n) const; // stroke extent incl. brush if `n` were added
    double radius() const { return std::min(m_brush, m_maxExtent / 2); }
    void paintStroke(QPainter& p, const ImagePane& pane, const Shown& s, const QPen& outer, const QPen& inner) const;

    QList<Shown> m_shown;
    QList<Shown> m_busy;
    QList<QPointF> m_stroke; // being drawn
    bool m_drawing = false;
    bool m_capped = false; // the stroke reached the size limit
    double m_brush = 0.03;
    double m_maxExtent = 1;       // largest heal, relative to the crop's long edge
    QSizeF m_aspect{1, 1};        // crop size / its long edge
    double m_phase = 0;
    QPointF m_hover{-1, -1};
};

} // namespace silma
