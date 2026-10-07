#include "Filmstrip.h"

#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>

namespace silma {

PhotoListModel::PhotoListModel(QObject* parent) : QAbstractListModel(parent) {}

void PhotoListModel::reload(std::optional<qint64> importId)
{
    beginResetModel();
    m_importId = importId;
    m_photos = m_batch ? m_batch->photos(importId) : QList<PhotoInfo>();
    m_edited.clear();
    for (const auto& p : m_photos)
        if (p.historyPos > 0)
            m_edited.insert(p.id);
    endResetModel();
}

void PhotoListModel::refresh()
{
    if (!m_batch)
        return;
    QList<PhotoInfo> fresh = m_batch->photos(m_importId);
    bool sameIds = fresh.size() == m_photos.size();
    for (int i = 0; sameIds && i < fresh.size(); ++i)
        sameIds = fresh[i].id == m_photos[i].id;
    if (!sameIds) {
        reload(m_importId);
        return;
    }
    m_photos = fresh;
    m_edited.clear();
    for (const auto& p : m_photos)
        if (p.historyPos > 0)
            m_edited.insert(p.id);
    if (!m_photos.isEmpty())
        emit dataChanged(index(0), index(int(m_photos.size()) - 1));
}

void PhotoListModel::invalidateThumbnail(qint64 id)
{
    m_thumbs.remove(id);
    int r = rowOf(id);
    if (r >= 0)
        emit dataChanged(index(r), index(r));
}

int PhotoListModel::rowOf(qint64 id) const
{
    for (int i = 0; i < m_photos.size(); ++i)
        if (m_photos[i].id == id)
            return i;
    return -1;
}

QVariant PhotoListModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.row() >= m_photos.size())
        return {};
    const PhotoInfo& p = m_photos[idx.row()];
    switch (role) {
    case Qt::DisplayRole: return p.fileName;
    case Qt::ToolTipRole:
        return QStringLiteral("%1\n%2\nISO %3, f/%4, %5 mm").arg(p.fileName, p.captureTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))).arg(p.iso).arg(p.fNumber).arg(p.focalLength);
    case Qt::DecorationRole: {
        if (QPixmap* px = m_thumbs.object(p.id))
            return *px;
        QPixmap px;
        QByteArray jpeg = m_batch ? m_batch->thumbnail(p.id) : QByteArray();
        if (!jpeg.isEmpty())
            px.loadFromData(jpeg, "JPEG");
        m_thumbs.insert(p.id, new QPixmap(px));
        return px;
    }
    case IdRole: return p.id;
    case RejectedRole: return p.rejected;
    case EditedRole: return m_edited.contains(p.id);
    case PathRole: return p.path;
    default: return {};
    }
}

QSize FilmstripDelegate::sizeHint(const QStyleOptionViewItem&, const QModelIndex& idx) const
{
    QPixmap px = idx.data(Qt::DecorationRole).value<QPixmap>();
    double aspect = px.isNull() ? 1.5 : double(px.width()) / px.height();
    return QSize(int(thumbHeight * std::clamp(aspect, 0.6, 1.8)) + 10, thumbHeight + 10);
}

void FilmstripDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const
{
    p->save();
    p->setRenderHint(QPainter::SmoothPixmapTransform);
    QRect r = opt.rect.adjusted(2, 2, -2, -2);
    bool selected = opt.state & QStyle::State_Selected;
    bool current = opt.state & QStyle::State_HasFocus || (opt.widget && static_cast<const QListView*>(opt.widget)->currentIndex() == idx);
    p->fillRect(r, selected ? QColor(90, 90, 90) : QColor(52, 52, 52));
    QPixmap px = idx.data(Qt::DecorationRole).value<QPixmap>();
    QRect inner = r.adjusted(3, 3, -3, -3);
    bool rejected = idx.data(PhotoListModel::RejectedRole).toBool();
    if (!px.isNull()) {
        QSize s = px.size().scaled(inner.size(), Qt::KeepAspectRatio);
        QRect ir(QPoint(inner.center().x() - s.width() / 2, inner.center().y() - s.height() / 2), s);
        if (rejected)
            p->setOpacity(0.35);
        p->drawPixmap(ir, px);
        p->setOpacity(1.0);
    }
    if (current) {
        p->setPen(QPen(QColor(230, 230, 230), 2));
        p->drawRect(r.adjusted(1, 1, -1, -1));
    }
    if (rejected) {
        QRect b(r.left() + 5, r.top() + 5, 16, 16);
        p->setPen(QPen(QColor(255, 90, 80), 2.5));
        p->drawLine(b.topLeft(), b.bottomRight());
        p->drawLine(b.topRight(), b.bottomLeft());
    }
    if (idx.data(PhotoListModel::EditedRole).toBool()) {
        // Small "edited" marker in the bottom right corner.
        QRect b(r.right() - 12, r.bottom() - 12, 8, 8);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(220, 220, 220, 200));
        p->drawEllipse(b);
    }
    p->restore();
}

Filmstrip::Filmstrip(QWidget* parent) : QListView(parent)
{
    setViewMode(QListView::IconMode);
    setFlow(QListView::LeftToRight);
    setWrapping(false);
    setMovement(QListView::Static);
    setResizeMode(QListView::Adjust);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setUniformItemSizes(false);
    setSpacing(0);
    m_delegate = new FilmstripDelegate(this);
    setItemDelegate(m_delegate);
    setStyleSheet(QStringLiteral("QListView { background: #262626; border: none; }"));
    setMinimumHeight(60);
}

void Filmstrip::setThumbHeight(int h)
{
    if (h == m_delegate->thumbHeight)
        return;
    m_delegate->thumbHeight = h;
    // Item sizes depend on the height: force relayout.
    doItemsLayout();
}

void Filmstrip::resizeEvent(QResizeEvent* e)
{
    int sb = horizontalScrollBar()->isVisible() ? horizontalScrollBar()->height() : 0;
    setThumbHeight(std::max(40, viewport()->height() - 12 + (sb ? 0 : 0)));
    QListView::resizeEvent(e);
}

void Filmstrip::wheelEvent(QWheelEvent* e)
{
    // Vertical wheel scrolls horizontally.
    int d = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d);
    e->accept();
}

} // namespace silma
