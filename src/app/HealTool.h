#pragma once

#include "Develop.h"
#include "Geometry.h"
#include "ImagePane.h"

#include <QObject>
#include <optional>

namespace silma {

// Click to heal a spot (brush-sized circle); Alt+click on a spot removes it.
class HealTool : public QObject, public PaneTool {
    Q_OBJECT
public:
    explicit HealTool(QObject* parent = nullptr) : QObject(parent) {}

    // Spot radius relative to the displayed image's long edge.
    void setBrushSize(double r) { m_brush = r; }
    double brushSize() const { return m_brush; }
    void setSpots(const QList<HealSpot>& spots, std::optional<Geometry> g, double sensorLong);
    void setBusy(QPointF at, double radius) { m_busy = {at, radius}; }
    void clearBusy() { m_busy.reset(); }

    void paint(QPainter& p, const ImagePane& pane) override;
    bool press(const ImagePane& pane, QPointF n, QMouseEvent* e) override;
    void move(const ImagePane& pane, QPointF n, QMouseEvent* e) override;
    void release(const ImagePane& pane, QPointF n, QMouseEvent* e) override;
    Qt::CursorShape cursorAt(const ImagePane& pane, QPointF n) const override;

signals:
    void healRequested(QPointF cropPoint, double radius);
    void removeRequested(int spotIndex);

private:
    struct Shown {
        QPointF center; // crop-normalized
        double radius;  // relative to the crop's long edge
    };
    QList<Shown> m_shown;
    double m_brush = 0.03;
    std::optional<std::pair<QPointF, double>> m_busy;
    QPointF m_hover{-1, -1};
};

} // namespace silma
