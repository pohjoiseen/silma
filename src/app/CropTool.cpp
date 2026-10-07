#include "CropTool.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

namespace silma {

namespace {
constexpr double kHandlePx = 10;
constexpr double kMinSize = 0.03;
} // namespace

void CropTool::paint(QPainter& p, const ImagePane& pane)
{
    QRectF img = pane.imageRect();
    QRectF r(pane.imageToWidget(m_rect.topLeft()), pane.imageToWidget(m_rect.bottomRight()));
    // Darken outside the crop.
    QPainterPath outside;
    outside.addRect(img);
    QPainterPath inside;
    inside.addRect(r);
    p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 150));
    p.setRenderHint(QPainter::Antialiasing, false);
    // Rule of thirds.
    p.setPen(QPen(QColor(255, 255, 255, 110), 1));
    for (int i = 1; i < 3; ++i) {
        p.drawLine(QPointF(r.left() + r.width() * i / 3, r.top()), QPointF(r.left() + r.width() * i / 3, r.bottom()));
        p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 3), QPointF(r.right(), r.top() + r.height() * i / 3));
    }
    p.setPen(QPen(QColor(255, 255, 255), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
    // Corner handles.
    p.setPen(QPen(QColor(255, 255, 255), 3));
    const double L = 14;
    for (QPointF c : {r.topLeft(), r.topRight(), r.bottomLeft(), r.bottomRight()}) {
        double sx = c.x() == r.left() ? 1 : -1, sy = c.y() == r.top() ? 1 : -1;
        p.drawLine(c, c + QPointF(sx * L, 0));
        p.drawLine(c, c + QPointF(0, sy * L));
    }
}

int CropTool::hitTest(const ImagePane& pane, QPointF n) const
{
    QPointF w = pane.imageToWidget(n);
    QRectF r(pane.imageToWidget(m_rect.topLeft()), pane.imageToWidget(m_rect.bottomRight()));
    int h = None;
    bool inY = w.y() > r.top() - kHandlePx && w.y() < r.bottom() + kHandlePx;
    bool inX = w.x() > r.left() - kHandlePx && w.x() < r.right() + kHandlePx;
    if (inY && std::abs(w.x() - r.left()) < kHandlePx)
        h |= Left;
    if (inY && std::abs(w.x() - r.right()) < kHandlePx)
        h |= Right;
    if (inX && std::abs(w.y() - r.top()) < kHandlePx)
        h |= Top;
    if (inX && std::abs(w.y() - r.bottom()) < kHandlePx)
        h |= Bottom;
    if (h == None)
        h = r.contains(w) ? Move : Rotate;
    return h;
}

Qt::CursorShape CropTool::cursorAt(const ImagePane& pane, QPointF n) const
{
    int h = hitTest(pane, n);
    if (h == Move)
        return Qt::SizeAllCursor;
    if (h == Rotate)
        return Qt::CrossCursor;
    if (h == (Left | Top) || h == (Right | Bottom))
        return Qt::SizeFDiagCursor;
    if (h == (Right | Top) || h == (Left | Bottom))
        return Qt::SizeBDiagCursor;
    if (h & (Left | Right))
        return Qt::SizeHorCursor;
    return Qt::SizeVerCursor;
}

bool CropTool::press(const ImagePane& pane, QPointF n, QMouseEvent*)
{
    m_handle = hitTest(pane, n);
    m_start = n;
    m_startRect = m_rect;
    m_startRotation = m_rotation;
    m_startAngle = std::atan2((n.y() - 0.5) * m_canvas.height(), (n.x() - 0.5) * m_canvas.width());
    return true;
}

void CropTool::move(const ImagePane&, QPointF n, QMouseEvent*)
{
    QPointF d = n - m_start;
    if (m_handle == Rotate) {
        double a = std::atan2((n.y() - 0.5) * m_canvas.height(), (n.x() - 0.5) * m_canvas.width());
        double deg = m_startRotation + (a - m_startAngle) * 180 / M_PI;
        deg = std::clamp(std::round(deg * 10) / 10, -45.0, 45.0);
        if (deg != m_rotation) {
            m_rotation = deg;
            emit rotationChanged(deg, false);
        }
        return;
    }
    QRectF r = m_startRect;
    if (m_handle == Move) {
        r.translate(d);
        r.moveLeft(std::clamp(r.left(), 0.0, 1.0 - r.width()));
        r.moveTop(std::clamp(r.top(), 0.0, 1.0 - r.height()));
    } else {
        if (m_handle & Left)
            r.setLeft(std::clamp(r.left() + d.x(), 0.0, r.right() - kMinSize));
        if (m_handle & Right)
            r.setRight(std::clamp(r.right() + d.x(), r.left() + kMinSize, 1.0));
        if (m_handle & Top)
            r.setTop(std::clamp(r.top() + d.y(), 0.0, r.bottom() - kMinSize));
        if (m_handle & Bottom)
            r.setBottom(std::clamp(r.bottom() + d.y(), r.top() + kMinSize, 1.0));
        if (m_lock) {
            // Keep the pixel aspect ratio of the starting rectangle, anchored at the opposite side.
            double aspectN = m_startRect.width() / m_startRect.height(); // in normalized units
            bool horizontalDrive = (m_handle & (Left | Right)) && !((m_handle & (Top | Bottom)) && std::abs(d.y()) > std::abs(d.x()));
            if (horizontalDrive) {
                double hgt = r.width() / aspectN;
                if (m_handle & Top)
                    r.setTop(r.bottom() - hgt);
                else if (m_handle & Bottom)
                    r.setBottom(r.top() + hgt);
                else {
                    double c = m_startRect.center().y();
                    r.setTop(c - hgt / 2);
                    r.setBottom(c + hgt / 2);
                }
            } else {
                double wid = r.height() * aspectN;
                if (m_handle & Left)
                    r.setLeft(r.right() - wid);
                else if (m_handle & Right)
                    r.setRight(r.left() + wid);
                else {
                    double c = m_startRect.center().x();
                    r.setLeft(c - wid / 2);
                    r.setRight(c + wid / 2);
                }
            }
            // Out of bounds: shrink uniformly to fit.
            QRectF clipped = r.intersected(QRectF(0, 0, 1, 1));
            if (clipped != r) {
                double k = std::min(clipped.width() / r.width(), clipped.height() / r.height());
                QPointF anchor((m_handle & Left) ? r.right() : (m_handle & Right) ? r.left() : r.center().x(),
                               (m_handle & Top) ? r.bottom() : (m_handle & Bottom) ? r.top() : r.center().y());
                r = QRectF(anchor + (r.topLeft() - anchor) * k, anchor + (r.bottomRight() - anchor) * k).normalized();
            }
        }
    }
    m_rect = r.normalized().intersected(QRectF(0, 0, 1, 1));
    emit rectChanged(m_rect, false);
}

void CropTool::release(const ImagePane&, QPointF, QMouseEvent*)
{
    if (m_handle == Rotate)
        emit rotationChanged(m_rotation, true);
    else if (m_rect != m_startRect)
        emit rectChanged(m_rect, true);
    m_handle = None;
}

} // namespace silma
