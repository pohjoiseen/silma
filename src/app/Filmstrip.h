#pragma once

#include "Batch.h"

#include <QAbstractListModel>
#include <QCache>
#include <QListView>
#include <QPixmap>
#include <QStyledItemDelegate>

namespace silma {

class PhotoListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { IdRole = Qt::UserRole + 1, RejectedRole, EditedRole, PathRole };

    explicit PhotoListModel(QObject* parent = nullptr);
    void setBatch(Batch* b) { m_batch = b; }
    // Reloads photos (of one import, or all when importId is empty).
    void reload(std::optional<qint64> importId);
    // Refreshes rows from the database while keeping the list structure where possible.
    void refresh();
    void invalidateThumbnail(qint64 id);

    int rowCount(const QModelIndex& = {}) const override { return int(m_photos.size()); }
    QVariant data(const QModelIndex& idx, int role) const override;
    int rowOf(qint64 id) const;
    const PhotoInfo& photoAt(int row) const { return m_photos[row]; }
    QList<PhotoInfo> photos() const { return m_photos; }
    std::optional<qint64> importId() const { return m_importId; }

private:
    Batch* m_batch = nullptr;
    std::optional<qint64> m_importId;
    QList<PhotoInfo> m_photos;
    QSet<qint64> m_edited;
    mutable QCache<qint64, QPixmap> m_thumbs{400};
};

class FilmstripDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;
    int thumbHeight = 96;
};

class Filmstrip : public QListView {
    Q_OBJECT
public:
    explicit Filmstrip(QWidget* parent = nullptr);
    void setThumbHeight(int h);

protected:
    void wheelEvent(QWheelEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    FilmstripDelegate* m_delegate;
};

} // namespace silma
