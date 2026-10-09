#include "Filmstrip.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QMouseEvent>
#include <QWheelEvent>

namespace silma {

PhotoListModel::PhotoListModel(QObject* parent) : QAbstractListModel(parent) {}

void PhotoListModel::reload(std::optional<qint64> importId)
{
    beginResetModel();
    m_importId = importId;
    m_photos = m_batch ? m_batch->photos(importId) : QList<PhotoInfo>();
    m_thumbs.clear();
    m_missingThumbs.clear();
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
    if (!m_missingThumbs.isEmpty()) {
        // Thumbnails may have arrived (e.g. an import writes them after the photo rows);
        // item sizes depend on their aspect ratio, so lay the strip out again.
        m_missingThumbs.clear();
        emit layoutAboutToBeChanged();
        emit layoutChanged();
    }
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

void PhotoListModel::setHistoryPos(qint64 id, int pos)
{
    int r = rowOf(id);
    if (r < 0)
        return;
    m_photos[r].historyPos = pos;
    const bool edited = pos > 0;
    if (edited == m_edited.contains(id))
        return;
    if (edited)
        m_edited.insert(id);
    else
        m_edited.remove(id);
    emit dataChanged(index(r), index(r));
}

int PhotoListModel::rowOf(qint64 id) const
{
    for (int i = 0; i < m_photos.size(); ++i)
        if (m_photos[i].id == id)
            return i;
    return -1;
}

namespace {

// What made a derived photo, from the suffix addDerivedPhoto() gives its file name.
QString derivedBadge(const PhotoInfo& p)
{
    if (p.kind != PhotoKind::DerivedDng)
        return {};
    static const QRegularExpression re(QStringLiteral("-(Denoise|HDR|Pano)(-\\d+)?\\.dng$"), QRegularExpression::CaseInsensitiveOption);
    const QString what = re.match(p.fileName).captured(1).toLower();
    return what == "denoise" ? QStringLiteral("Denoise") : what == "hdr" ? QStringLiteral("HDR") : what == "pano" ? QStringLiteral("Pano") : QStringLiteral("DNG");
}

} // namespace

QVariant PhotoListModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.row() >= m_photos.size())
        return {};
    const PhotoInfo& p = m_photos[idx.row()];
    switch (role) {
    case Qt::DisplayRole: return p.fileName;
    case Qt::ToolTipRole: {
        QString tip = QStringLiteral("%1\n%2\nISO %3, f/%4, %5 mm").arg(p.fileName, p.captureTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))).arg(p.iso).arg(p.fNumber).arg(p.focalLength);
        if (p.kind == PhotoKind::DerivedDng && m_batch) {
            QStringList names;
            for (const QJsonValue v : QJsonDocument::fromJson(p.derivedFrom.toUtf8()).array())
                if (auto src = m_batch->photo(v.toInteger()))
                    names << src->fileName;
            const QString badge = derivedBadge(p);
            const QString what = badge == "Denoise" ? tr("AI denoised") : badge == "HDR" ? tr("HDR merge") : badge == "Pano" ? tr("Panorama") : tr("Made");
            tip += '\n' + (names.isEmpty() ? what : tr("%1 from %2").arg(what, names.join(", ")));
        }
        return tip;
    }
    case Qt::DecorationRole: {
        if (QPixmap* px = m_thumbs.object(p.id))
            return *px;
        QPixmap px;
        QByteArray jpeg = m_batch ? m_batch->thumbnail(p.id) : QByteArray();
        if (!jpeg.isEmpty())
            px.loadFromData(jpeg, "JPEG");
        // Only cache real thumbnails: a missing one may be written later.
        if (px.isNull())
            m_missingThumbs.insert(p.id);
        else
            m_thumbs.insert(p.id, new QPixmap(px));
        return px;
    }
    case IdRole: return p.id;
    case RejectedRole: return p.rejected;
    case EditedRole: return m_edited.contains(p.id);
    case PathRole: return p.path;
    case DerivedRole: return derivedBadge(p);
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
    // With several photos selected, wash the selected ones out (the opposite of the
    // dimmed rejected ones) so the selection reads at a glance.
    bool multi = selected && multiSelection;
    p->fillRect(r, multi ? QColor(150, 150, 150) : selected ? QColor(90, 90, 90) : QColor(52, 52, 52));
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
        if (multi)
            p->fillRect(ir, QColor(255, 255, 255, 70));
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
    if (const QString badge = idx.data(PhotoListModel::DerivedRole).toString(); !badge.isEmpty()) {
        // Photos made by Silmä from others: a small label in the top right corner.
        QFont f = p->font();
        f.setPixelSize(10);
        f.setBold(true);
        p->setFont(f);
        const QFontMetrics fm(f);
        QRect b(0, 0, fm.horizontalAdvance(badge) + 8, fm.height() + 2);
        b.moveTopRight(QPoint(r.right() - 5, r.top() + 5));
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(20, 20, 20, 190));
        p->drawRoundedRect(b, 3, 3);
        p->setPen(QColor(235, 235, 235));
        p->drawText(b, Qt::AlignCenter, badge);
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
    setAutoScroll(false);
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

void Filmstrip::setModel(QAbstractItemModel* model)
{
    QListView::setModel(model);
    // A reset (photos added or removed, e.g. by a derived photo job) would clear the
    // selection and current photo; Qt then makes the first visible photo current when the
    // strip gets focus, so actions would silently apply to a photo next to the shown one.
    connect(model, &QAbstractItemModel::modelAboutToBeReset, this, [this] {
        m_keptSelection.clear();
        for (const QModelIndex& i : selectionModel()->selectedIndexes())
            m_keptSelection << i.data(PhotoListModel::IdRole).toLongLong();
        m_keptCurrent = currentIndex().data(PhotoListModel::IdRole).toLongLong();
    });
    connect(model, &QAbstractItemModel::modelReset, this, [this] {
        auto* m = qobject_cast<PhotoListModel*>(this->model());
        if (!m)
            return;
        QItemSelection sel;
        for (qint64 id : std::as_const(m_keptSelection))
            if (int r = m->rowOf(id); r >= 0)
                sel.select(m->index(r), m->index(r));
        const int cur = m->rowOf(m_keptCurrent);
        if (cur >= 0)
            selectionModel()->setCurrentIndex(m->index(cur), QItemSelectionModel::NoUpdate);
        selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
    });
}

void Filmstrip::dataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight, const QList<int>& roles)
{
    QListView::dataChanged(topLeft, bottomRight, roles);
    // A new thumbnail can have another aspect ratio (a photo turned by 90 degrees): the item
    // takes its new width, but the items after it keep their positions from the last layout.
    const int last = std::min(bottomRight.row(), model()->rowCount() - 2);
    for (int r = topLeft.row(); r <= last; ++r) {
        const QRect item = visualRect(model()->index(r, 0)), next = visualRect(model()->index(r + 1, 0));
        if (next.left() != item.right() + 1 + spacing()) {
            scheduleDelayedItemsLayout();
            return;
        }
    }
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
    // Room for the scroll bar is always left: sizing the thumbnails by the viewport could loop
    // (smaller thumbnails fit without the scroll bar, which makes them bigger again, ...).
    const int sb = horizontalScrollBar()->sizeHint().height();
    setThumbHeight(std::max(40, contentsRect().height() - sb - 12));
    QListView::resizeEvent(e);
}

void Filmstrip::selectionChanged(const QItemSelection& selected, const QItemSelection& deselected)
{
    QListView::selectionChanged(selected, deselected);
    bool multi = selectionModel()->selectedIndexes().size() > 1;
    if (multi != m_delegate->multiSelection) {
        // Going from one to several selected photos (or back) changes how all of them look.
        m_delegate->multiSelection = multi;
        viewport()->update();
    }
}

void Filmstrip::mouseMoveEvent(QMouseEvent* e)
{
    // No drag selection (it also auto-scrolls the strip at its ends): selecting is by
    // click, Ctrl+click and Shift+click.
    if (e->buttons() != Qt::NoButton) {
        e->accept();
        return;
    }
    QListView::mouseMoveEvent(e);
}

void Filmstrip::wheelEvent(QWheelEvent* e)
{
    // Vertical wheel scrolls horizontally.
    int d = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d);
    e->accept();
}

} // namespace silma
