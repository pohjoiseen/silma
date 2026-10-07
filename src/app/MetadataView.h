#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QTreeWidget;

namespace silma {

// Shows the metadata of a photo: a short summary and every tag, filterable.
class MetadataView : public QWidget {
    Q_OBJECT
public:
    explicit MetadataView(QWidget* parent = nullptr);
    void showFile(const QString& path);

private:
    void applyFilter();
    QLabel* m_summary;
    QLineEdit* m_filter;
    QTreeWidget* m_tree;
    QString m_path;
};

} // namespace silma
