#include "MetadataView.h"

#include "Metadata.h"

#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace silma {

MetadataView::MetadataView(QWidget* parent) : QWidget(parent)
{
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(4, 4, 4, 4);
    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->addWidget(m_summary);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter tags..."));
    m_filter->setClearButtonEnabled(true);
    l->addWidget(m_filter);
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Tag"), tr("Value")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tree->setColumnWidth(0, 160);
    l->addWidget(m_tree, 1);
    connect(m_filter, &QLineEdit::textChanged, this, &MetadataView::applyFilter);
}

void MetadataView::showFile(const QString& path)
{
    if (path == m_path)
        return;
    m_path = path;
    m_tree->clear();
    if (path.isEmpty()) {
        m_summary->clear();
        return;
    }
    try {
        MetadataSummary s = readMetadataSummary(path);
        QStringList lines;
        lines << QStringLiteral("<b>%1</b>").arg(QFileInfo(path).fileName().toHtmlEscaped());
        lines << s.captureTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        lines << QStringLiteral("%1 %2").arg(s.make, s.model).toHtmlEscaped();
        if (!s.lens.isEmpty())
            lines << s.lens.toHtmlEscaped();
        QString exp = s.exposureTime > 0 ? (s.exposureTime < 0.5 ? QStringLiteral("1/%1 s").arg(qRound(1 / s.exposureTime)) : QStringLiteral("%1 s").arg(s.exposureTime)) : QString();
        lines << QStringLiteral("ISO %1 · %2 · f/%3 · %4 mm%5").arg(s.iso).arg(exp).arg(s.fNumber).arg(s.focalLength)
                     .arg(s.focalLength35 > 0 ? QStringLiteral(" (%1 mm eq.)").arg(s.focalLength35) : QString());
        lines << QStringLiteral("%1 × %2 · %3").arg(s.width).arg(s.height).arg(QLocale().formattedDataSize(QFileInfo(path).size()));
        if (s.latitude && s.longitude) {
            QString url = QStringLiteral("https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=14/%1/%2").arg(*s.latitude, 0, 'f', 6).arg(*s.longitude, 0, 'f', 6);
            lines << QStringLiteral("<a href=\"%1\">%2, %3</a>%4").arg(url).arg(*s.latitude, 0, 'f', 5).arg(*s.longitude, 0, 'f', 5)
                         .arg(s.altitude ? QStringLiteral(" · %1 m").arg(qRound(*s.altitude)) : QString());
        }
        m_summary->setText(lines.join("<br>"));
        m_summary->setOpenExternalLinks(true);
        QHash<QString, QTreeWidgetItem*> groups;
        for (const auto& t : readAllMetadata(path)) {
            QTreeWidgetItem*& g = groups[t.group];
            if (!g) {
                g = new QTreeWidgetItem(m_tree, {t.group});
                g->setFirstColumnSpanned(true);
            }
            auto* it = new QTreeWidgetItem(g, {t.label, t.value});
            it->setToolTip(0, t.key);
            it->setToolTip(1, t.value);
        }
        // The interesting groups first and expanded; structural TIFF groups collapsed.
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            auto* g = m_tree->topLevelItem(i);
            g->setExpanded(g->text(0) == QLatin1String("Exif.Photo") || g->text(0) == QLatin1String("Exif.GPSInfo"));
        }
        m_tree->sortItems(0, Qt::AscendingOrder);
        for (const char* first : {"Exif.GPSInfo", "Exif.Photo"})
            for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
                if (m_tree->topLevelItem(i)->text(0) == QLatin1String(first))
                    m_tree->insertTopLevelItem(0, m_tree->takeTopLevelItem(i));
    } catch (const std::exception& e) {
        m_summary->setText(tr("Cannot read metadata: %1").arg(QString::fromUtf8(e.what())));
    }
    applyFilter();
}

void MetadataView::applyFilter()
{
    const QString f = m_filter->text().trimmed();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto* g = m_tree->topLevelItem(i);
        int visible = 0;
        for (int j = 0; j < g->childCount(); ++j) {
            auto* c = g->child(j);
            bool show = f.isEmpty() || c->text(0).contains(f, Qt::CaseInsensitive) || c->text(1).contains(f, Qt::CaseInsensitive) ||
                        c->toolTip(0).contains(f, Qt::CaseInsensitive);
            c->setHidden(!show);
            visible += show;
        }
        g->setHidden(visible == 0);
    }
}

} // namespace silma
