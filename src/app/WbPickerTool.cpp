#include "WbPickerTool.h"

#include <QMouseEvent>
#include <QPainter>

namespace silma {

void WbPickerTool::paint(QPainter& p, const ImagePane& pane)
{
    if (m_hover.x() < 0 || m_hover.y() < 0 || m_hover.x() > 1 || m_hover.y() > 1)
        return;
    // The sampled spot, outlined for any background.
    const QPointF c = pane.imageToWidget(m_hover);
    const QRectF r(c - QPointF(5, 5), QSizeF(10, 10));
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 180), 3));
    p.drawRect(r);
    p.setPen(QPen(Qt::white, 1));
    p.drawRect(r);
    p.restore();
}

bool WbPickerTool::press(const ImagePane&, QPointF n, QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || n.x() < 0 || n.y() < 0 || n.x() > 1 || n.y() > 1)
        return false;
    emit picked(n);
    return true;
}

Qt::CursorShape WbPickerTool::cursorAt(const ImagePane&, QPointF n) const
{
    const_cast<WbPickerTool*>(this)->m_hover = n;
    return Qt::CrossCursor;
}

} // namespace silma
