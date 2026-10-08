#include "HealTool.h"

#include "Heal.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <cmath>

namespace silma {

void HealTool::setSpots(const QList<HealSpot>& spots, std::optional<Geometry> g, double sensorLong)
{
    m_shown.clear();
    if (!g)
        return;
    QSizeF crop = g->cropSize();
    double cropLong = std::max(crop.width(), crop.height());
    m_aspect = QSizeF(crop.width() / cropLong, crop.height() / cropLong);
    // The size cap is relative to the sensor; express it in crop units.
    m_maxExtent = kMaxHealExtent * sensorLong * g->lensZoom() / cropLong;
    for (const auto& s : spots) {
        Shown sh{{g->sensorToCrop({s.x, s.y})}, s.radius * sensorLong * g->lensZoom() / cropLong};
        for (const QPointF& p : s.path)
            sh.pts.append(g->sensorToCrop(p));
        m_shown.append(sh);
    }
}

void HealTool::addBusy(const QList<QPointF>& stroke, double radius) { m_busy.append({stroke, radius}); }

void HealTool::removeBusy(const QList<QPointF>& stroke)
{
    for (qsizetype i = 0; i < m_busy.size(); ++i)
        if (m_busy[i].pts == stroke) {
            m_busy.removeAt(i);
            return;
        }
}

double HealTool::distanceTo(const Shown& s, QPointF n) const
{
    const QPointF p = iso(n);
    double best = 1e30;
    for (qsizetype i = 0; i < s.pts.size(); ++i) {
        QPointF a = iso(s.pts[i]), b = iso(s.pts[std::min(i + 1, s.pts.size() - 1)]);
        QPointF ab = b - a, ap = p - a;
        double len2 = ab.x() * ab.x() + ab.y() * ab.y();
        double t = len2 > 0 ? std::clamp((ap.x() * ab.x() + ap.y() * ab.y()) / len2, 0.0, 1.0) : 0.0;
        QPointF d = ap - ab * t;
        best = std::min(best, std::hypot(d.x(), d.y()));
    }
    return best;
}

double HealTool::extentWith(QPointF n) const
{
    QPointF lo = iso(n), hi = lo;
    for (const QPointF& q : m_stroke) {
        QPointF p = iso(q);
        lo = QPointF(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()));
        hi = QPointF(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()));
    }
    return std::max(hi.x() - lo.x(), hi.y() - lo.y()) + 2 * radius();
}

void HealTool::paintStroke(QPainter& p, const ImagePane& pane, const Shown& s, const QPen& outer, const QPen& inner) const
{
    if (s.pts.isEmpty())
        return;
    const QRectF img = pane.imageRect();
    const double r = s.radius * std::max(img.width(), img.height());
    QPainterPath outline;
    if (s.pts.size() == 1) {
        outline.addEllipse(pane.imageToWidget(s.pts[0]), r, r);
    } else {
        // The area a round brush covers along the stroke.
        QPainterPath line(pane.imageToWidget(s.pts[0]));
        for (qsizetype i = 1; i < s.pts.size(); ++i)
            line.lineTo(pane.imageToWidget(s.pts[i]));
        QPainterPathStroker stroker;
        stroker.setWidth(2 * r);
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);
        outline = stroker.createStroke(line).simplified();
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(outer);
    p.drawPath(outline);
    p.setPen(inner);
    p.drawPath(outline);
}

void HealTool::paint(QPainter& p, const ImagePane& pane)
{
    QRectF img = pane.imageRect();
    double longPx = std::max(img.width(), img.height());
    p.setRenderHint(QPainter::Antialiasing);
    const QPen dark(QColor(0, 0, 0, 160), 3), light(QColor(255, 255, 255, 220), 1.5);
    for (const auto& s : m_shown)
        paintStroke(p, pane, s, dark, light);
    // Being generated: marching ants.
    QPen ants(QColor(255, 200, 60), 2, Qt::DashLine);
    ants.setDashOffset(-m_phase);
    for (const auto& s : m_busy)
        paintStroke(p, pane, s, dark, ants);
    if (m_drawing) {
        const QPen pen(m_capped ? QColor(255, 140, 40) : QColor(255, 255, 255, 220), m_capped ? 2.0 : 1.5);
        paintStroke(p, pane, {m_stroke, radius()}, dark, pen);
    } else if (m_hover.x() >= 0) {
        QPointF c = pane.imageToWidget(m_hover);
        double r = radius() * longPx;
        p.setPen(QPen(QColor(255, 255, 255, 140), 1, Qt::DotLine));
        p.drawEllipse(c, r, r);
    }
}

bool HealTool::press(const ImagePane&, QPointF n, QMouseEvent* e)
{
    if (e->modifiers() & Qt::AltModifier) {
        // Remove the topmost healed area under the cursor.
        for (int i = int(m_shown.size()) - 1; i >= 0; --i)
            if (distanceTo(m_shown[i], n) <= m_shown[i].radius * 1.2) {
                emit removeRequested(i);
                return true;
            }
        return true;
    }
    if (n.x() < 0 || n.y() < 0 || n.x() > 1 || n.y() > 1)
        return true;
    m_stroke = {n};
    m_drawing = true;
    m_capped = false;
    return true;
}

void HealTool::move(const ImagePane&, QPointF n, QMouseEvent*)
{
    m_hover = n;
    if (!m_drawing)
        return;
    n = QPointF(std::clamp(n.x(), 0.0, 1.0), std::clamp(n.y(), 0.0, 1.0));
    // Keep points about a third of the brush apart (smooth enough, few segments).
    QPointF d = iso(n) - iso(m_stroke.last());
    if (std::hypot(d.x(), d.y()) < radius() / 3)
        return;
    if (extentWith(n) > m_maxExtent) {
        m_capped = true; // LaMa works at 512 px: bigger areas would come out too soft
        return;
    }
    m_stroke.append(n);
}

void HealTool::release(const ImagePane&, QPointF, QMouseEvent*)
{
    if (!m_drawing)
        return;
    m_drawing = false;
    const QList<QPointF> stroke = m_stroke;
    m_stroke.clear();
    emit healRequested(stroke, radius());
}

Qt::CursorShape HealTool::cursorAt(const ImagePane&, QPointF n) const
{
    const_cast<HealTool*>(this)->m_hover = n;
    return Qt::CrossCursor;
}

} // namespace silma
