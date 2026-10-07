#include "HealTool.h"

#include <QMouseEvent>
#include <QPainter>
#include <cmath>

namespace silma {

void HealTool::setSpots(const QList<HealSpot>& spots, std::optional<Geometry> g, double sensorLong)
{
    m_shown.clear();
    if (!g)
        return;
    QSizeF crop = g->cropSize();
    double cropLong = std::max(crop.width(), crop.height());
    for (const auto& s : spots)
        m_shown.append({g->sensorToCrop({s.x, s.y}), s.radius * sensorLong * g->lensZoom() / cropLong});
}

void HealTool::paint(QPainter& p, const ImagePane& pane)
{
    QRectF img = pane.imageRect();
    double longPx = std::max(img.width(), img.height());
    p.setRenderHint(QPainter::Antialiasing);
    for (const auto& s : m_shown) {
        QPointF c = pane.imageToWidget(s.center);
        double r = s.radius * longPx;
        p.setPen(QPen(QColor(0, 0, 0, 160), 3));
        p.drawEllipse(c, r, r);
        p.setPen(QPen(QColor(255, 255, 255, 220), 1.5));
        p.drawEllipse(c, r, r);
    }
    if (m_busy) {
        QPointF c = pane.imageToWidget(m_busy->first);
        double r = m_busy->second * longPx;
        p.setPen(QPen(QColor(255, 200, 60), 2, Qt::DashLine));
        p.drawEllipse(c, r, r);
    }
    if (m_hover.x() >= 0) {
        QPointF c = pane.imageToWidget(m_hover);
        double r = m_brush * longPx;
        p.setPen(QPen(QColor(255, 255, 255, 140), 1, Qt::DotLine));
        p.drawEllipse(c, r, r);
    }
}

bool HealTool::press(const ImagePane&, QPointF n, QMouseEvent* e)
{
    if (e->modifiers() & Qt::AltModifier) {
        // Remove the topmost spot under the cursor.
        for (int i = int(m_shown.size()) - 1; i >= 0; --i) {
            QPointF d = n - m_shown[i].center;
            if (std::hypot(d.x(), d.y()) <= m_shown[i].radius * 1.2) {
                emit removeRequested(i);
                return true;
            }
        }
        return true;
    }
    if (n.x() < 0 || n.y() < 0 || n.x() > 1 || n.y() > 1)
        return true;
    emit healRequested(n, m_brush);
    return true;
}

void HealTool::move(const ImagePane&, QPointF n, QMouseEvent*) { m_hover = n; }
void HealTool::release(const ImagePane&, QPointF, QMouseEvent*) {}

Qt::CursorShape HealTool::cursorAt(const ImagePane&, QPointF n) const
{
    const_cast<HealTool*>(this)->m_hover = n;
    return Qt::CrossCursor;
}

} // namespace silma
