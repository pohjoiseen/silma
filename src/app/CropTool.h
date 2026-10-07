#pragma once

#include "ImagePane.h"

#include <QObject>
#include <QRectF>

namespace silma {

// Crop rectangle editing on a pane that shows the whole transformed canvas.
// Drag corners/edges to resize, inside to move, outside to rotate.
class CropTool : public QObject, public PaneTool {
    Q_OBJECT
public:
    explicit CropTool(QObject* parent = nullptr) : QObject(parent) {}

    void setRect(const QRectF& r) { m_rect = r; }
    QRectF rect() const { return m_rect; }
    void setLockAspect(bool on) { m_lock = on; }
    // Canvas size in pixels, for aspect ratio handling.
    void setCanvasSize(QSizeF s) { m_canvas = s; }
    void setRotation(double deg) { m_rotation = deg; }

    void paint(QPainter& p, const ImagePane& pane) override;
    bool press(const ImagePane& pane, QPointF n, QMouseEvent* e) override;
    void move(const ImagePane& pane, QPointF n, QMouseEvent* e) override;
    void release(const ImagePane& pane, QPointF n, QMouseEvent* e) override;
    Qt::CursorShape cursorAt(const ImagePane& pane, QPointF n) const override;

signals:
    void rectChanged(const QRectF& r, bool final);
    void rotationChanged(double deg, bool final);

private:
    enum Handle { None = 0, Left = 1, Right = 2, Top = 4, Bottom = 8, Move = 16, Rotate = 32 };
    int hitTest(const ImagePane& pane, QPointF n) const;

    QRectF m_rect{0, 0, 1, 1};
    QSizeF m_canvas{3, 2};
    bool m_lock = true;
    double m_rotation = 0;
    int m_handle = None;
    QPointF m_start;
    QRectF m_startRect;
    double m_startRotation = 0, m_startAngle = 0;
};

} // namespace silma
